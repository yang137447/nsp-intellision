#include "deferred_doc_runtime.hpp"

#include "document_owner.hpp"
#include "hlsl_ast.hpp"
#include "inlay_hints_runtime.hpp"
#include "lsp_helpers.hpp"
#include "preprocessor_view.hpp"
#include "semantic_snapshot.hpp"
#include "server_documents.hpp"
#include "server_request_handlers.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace {

struct DeferredDocJob {
  Document document;
  DeferredDocBuildContext context;
  std::chrono::steady_clock::time_point enqueuedAt;
  std::chrono::steady_clock::time_point notBefore;
};

std::mutex gDeferredDocMutex;
std::condition_variable gDeferredDocCv;
std::deque<std::string> gDeferredDocQueue;
std::unordered_map<std::string, DeferredDocJob> gDeferredDocPendingByUri;
std::unordered_map<std::string, int> gDeferredDocLatestVersionByUri;
bool gDeferredDocStopping = false;
std::thread gDeferredDocWorker;
bool gDeferredDocWorkerStarted = false;
constexpr auto kDeferredDocLatestOnlyCoalesceWindow =
    std::chrono::milliseconds(120);

std::mutex gDeferredDocMetricsMutex;
DeferredDocRuntimeMetricsSnapshot gDeferredDocMetrics;

uint64_t currentTimeMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line))
    lines.push_back(line);
  if (!text.empty() && text.back() == '\n')
    lines.emplace_back();
  return lines;
}

std::string buildDiagnosticsFingerprint(const DiagnosticsBuildOptions &options) {
  std::ostringstream oss;
  oss << (options.enableExpensiveRules ? "1" : "0") << "|"
      << options.timeBudgetMs << "|" << options.maxItems << "|"
      << (options.semanticCacheEnabled ? "1" : "0") << "|"
      << options.documentEpoch << "|"
      << (options.indeterminateEnabled ? "1" : "0") << "|"
      << options.indeterminateSeverity << "|"
      << options.indeterminateMaxItems << "|"
      << (options.indeterminateSuppressWhenErrors ? "1" : "0");
  return oss.str();
}

void recordDeferredQueueWait(double waitMs) {
  std::lock_guard<std::mutex> lock(gDeferredDocMetricsMutex);
  gDeferredDocMetrics.queueWaitSamples++;
  gDeferredDocMetrics.queueWaitTotalMs += waitMs;
  gDeferredDocMetrics.queueWaitMaxMs =
      std::max(gDeferredDocMetrics.queueWaitMaxMs, waitMs);
}

void recordDeferredBuildDuration(double buildMs) {
  std::lock_guard<std::mutex> lock(gDeferredDocMetricsMutex);
  gDeferredDocMetrics.buildCount++;
  gDeferredDocMetrics.buildTotalMs += buildMs;
  gDeferredDocMetrics.buildMaxMs =
      std::max(gDeferredDocMetrics.buildMaxMs, buildMs);
}

void recordDeferredScheduled(bool mergedLatestOnly) {
  std::lock_guard<std::mutex> lock(gDeferredDocMetricsMutex);
  gDeferredDocMetrics.scheduled++;
  if (mergedLatestOnly)
    gDeferredDocMetrics.mergedLatestOnly++;
}

void recordDeferredStaleDrop(bool latestOnlySuperseded = false) {
  std::lock_guard<std::mutex> lock(gDeferredDocMetricsMutex);
  gDeferredDocMetrics.droppedStale++;
  if (latestOnlySuperseded)
    gDeferredDocMetrics.mergedLatestOnly++;
}

DeferredDocBuildContext makeDeferredContextFromRuntime(
    const DocumentRuntime &runtime, const DeferredDocBuildContext &fallback) {
  DeferredDocBuildContext context = fallback;
  if (!runtime.activeUnitSnapshot.workspaceFolders.empty() ||
      !runtime.activeUnitSnapshot.includePaths.empty() ||
      !runtime.activeUnitSnapshot.shaderExtensions.empty() ||
      !runtime.activeUnitSnapshot.defines.empty()) {
    context.workspaceFolders = runtime.activeUnitSnapshot.workspaceFolders;
    context.includePaths = runtime.activeUnitSnapshot.includePaths;
    context.shaderExtensions = runtime.activeUnitSnapshot.shaderExtensions;
    context.defines = runtime.activeUnitSnapshot.defines;
  }
  return context;
}

int utf16LineLength(const std::vector<std::string> &lines, int line) {
  if (line < 0 || line >= static_cast<int>(lines.size()))
    return 0;
  return byteOffsetInLineToUtf16(lines[line],
                                 static_cast<int>(lines[line].size()));
}

Json makeRangeSpan(int startLine, int startChar, int endLine, int endChar) {
  Json range = makeObject();
  range.o["start"] = makePosition(startLine, startChar);
  range.o["end"] = makePosition(endLine, endChar);
  return range;
}

bool findSymbolRangeOnLine(const std::string &lineText, const std::string &symbol,
                           int &startOut, int &endOut) {
  startOut = 0;
  endOut = 0;
  if (lineText.empty() || symbol.empty())
    return false;

  size_t searchFrom = 0;
  while (searchFrom <= lineText.size()) {
    const size_t pos = lineText.find(symbol, searchFrom);
    if (pos == std::string::npos)
      return false;
    const size_t end = pos + symbol.size();
    const bool leftBoundary =
        pos == 0 || !isIdentifierChar(lineText[pos - 1]);
    const bool rightBoundary =
        end >= lineText.size() || !isIdentifierChar(lineText[end]);
    if (leftBoundary && rightBoundary) {
      startOut = byteOffsetInLineToUtf16(lineText, static_cast<int>(pos));
      endOut = byteOffsetInLineToUtf16(lineText, static_cast<int>(end));
      return true;
    }
    searchFrom = pos + 1;
  }
  return false;
}

int findBlockEndLine(const std::vector<std::string> &lines, int startLine) {
  if (startLine < 0 || startLine >= static_cast<int>(lines.size()))
    return startLine;

  bool inBlockComment = false;
  bool inString = false;
  int braceDepth = 0;
  bool seenOpenBrace = false;
  for (int line = startLine; line < static_cast<int>(lines.size()); line++) {
    bool inLineComment = false;
    const std::string &lineText = lines[line];
    for (size_t i = 0; i < lineText.size(); i++) {
      const char ch = lineText[i];
      const char next = i + 1 < lineText.size() ? lineText[i + 1] : '\0';
      if (inLineComment)
        break;
      if (inBlockComment) {
        if (ch == '*' && next == '/') {
          inBlockComment = false;
          i++;
        }
        continue;
      }
      if (inString) {
        if (ch == '"' && (i == 0 || lineText[i - 1] != '\\'))
          inString = false;
        continue;
      }
      if (ch == '/' && next == '/') {
        inLineComment = true;
        i++;
        continue;
      }
      if (ch == '/' && next == '*') {
        inBlockComment = true;
        i++;
        continue;
      }
      if (ch == '"') {
        inString = true;
        continue;
      }
      if (ch == '{') {
        braceDepth++;
        seenOpenBrace = true;
      } else if (ch == '}' && braceDepth > 0) {
        braceDepth--;
        if (seenOpenBrace && braceDepth == 0)
          return line;
      }
    }
  }
  return startLine;
}

void appendDocumentSymbol(Json &symbols, const std::string &name, int kind,
                          int rangeStartLine, int rangeStartChar,
                          int rangeEndLine, int rangeEndChar,
                          int selectionLine, int selectionStartChar,
                          int selectionEndChar, Json children) {
  Json symbol = makeObject();
  symbol.o["name"] = makeString(name);
  symbol.o["kind"] = makeNumber(kind);
  symbol.o["range"] = makeRangeSpan(rangeStartLine, rangeStartChar, rangeEndLine,
                                     rangeEndChar);
  symbol.o["selectionRange"] =
      makeRangeSpan(selectionLine, selectionStartChar, selectionLine,
                    selectionEndChar);
  symbol.o["children"] = std::move(children);
  symbols.a.push_back(std::move(symbol));
}

const SemanticSnapshot::StructInfo *
findSemanticStructInfo(const SemanticSnapshot *snapshot,
                       const HlslAstStructDecl &structDecl) {
  if (!snapshot)
    return nullptr;
  auto it = snapshot->structByName.find(structDecl.name);
  if (it == snapshot->structByName.end() || it->second >= snapshot->structs.size())
    return nullptr;
  const auto &info = snapshot->structs[it->second];
  return info.line == structDecl.line ? &info : nullptr;
}

Json buildDocumentSymbolsFromAst(const std::string &text,
                                 const HlslAstDocument *astDocument,
                                 const SemanticSnapshot *semanticSnapshot) {
  if (!astDocument)
    return makeArray();

  const std::vector<std::string> lines = splitLines(text);
  Json symbols = makeArray();

  auto appendTechniqueOrPassSymbols = [&]() {
    std::istringstream stream(text);
    std::string lineText;
    int line = 0;
    while (std::getline(stream, lineText)) {
      const size_t trimStart = lineText.find_first_not_of(" \t");
      const std::string view =
          trimStart == std::string::npos ? "" : lineText.substr(trimStart);
      auto tryAppend = [&](const std::string &prefix, int kind) {
        if (view.rfind(prefix, 0) != 0)
          return false;
        size_t nameStart = trimStart + prefix.size();
        while (nameStart < lineText.size() &&
               std::isspace(static_cast<unsigned char>(lineText[nameStart]))) {
          nameStart++;
        }
        if (nameStart >= lineText.size())
          return false;
        size_t nameEnd = lineText.find_first_of(" \t{(:", nameStart);
        if (nameEnd == std::string::npos)
          nameEnd = lineText.size();
        if (nameEnd <= nameStart)
          return false;
        const std::string name = lineText.substr(nameStart, nameEnd - nameStart);
        const int startChar = byteOffsetInLineToUtf16(
            lineText, static_cast<int>(nameStart));
        const int endChar =
            byteOffsetInLineToUtf16(lineText, static_cast<int>(nameEnd));
        const int endLine = findBlockEndLine(lines, line);
        appendDocumentSymbol(symbols, name, kind, line, startChar, endLine,
                             utf16LineLength(lines, endLine), line, startChar,
                             endChar, makeArray());
        return true;
      };
      if (!tryAppend("technique ", 2))
        tryAppend("pass ", 6);
      line++;
    }
  };

  for (const auto &decl : astDocument->topLevelDecls) {
    if (decl.kind == HlslTopLevelDeclKind::Function) {
      auto it = std::find_if(
          astDocument->functions.begin(), astDocument->functions.end(),
          [&](const HlslAstFunctionDecl &fn) {
            return fn.line == decl.line && fn.name == decl.name;
          });
      if (it == astDocument->functions.end())
        continue;
      const int endLine =
          it->hasBody && it->bodyEndLine >= it->line
              ? it->bodyEndLine
              : (it->signatureEndLine >= it->line ? it->signatureEndLine
                                                  : it->line);
      int startChar = it->character >= 0 ? it->character : 0;
      const std::string lineText = getLineAt(text, it->line);
      int endChar = startChar;
      findSymbolRangeOnLine(lineText, it->name, startChar, endChar);
      appendDocumentSymbol(symbols, it->name, 12, it->line, startChar, endLine,
                           utf16LineLength(lines, endLine), it->line, startChar,
                           endChar, makeArray());
      continue;
    }

    if (decl.kind == HlslTopLevelDeclKind::Struct) {
      auto it = std::find_if(
          astDocument->structs.begin(), astDocument->structs.end(),
          [&](const HlslAstStructDecl &structDecl) {
            return structDecl.line == decl.line && structDecl.name == decl.name;
          });
      if (it == astDocument->structs.end())
        continue;
      const std::string lineText = getLineAt(text, it->line);
      int startChar = 0;
      int endChar = 0;
      findSymbolRangeOnLine(lineText, it->name, startChar, endChar);
      const int endLine =
          it->hasBody ? findBlockEndLine(lines, it->line) : it->line;
      Json children = makeArray();
      const SemanticSnapshot::StructInfo *semanticInfo =
          findSemanticStructInfo(semanticSnapshot, *it);
      if (semanticInfo && !semanticInfo->fields.empty()) {
        for (const auto &field : semanticInfo->fields) {
          const std::string fieldLineText = getLineAt(text, field.line);
          int fieldStart = 0;
          int fieldEnd = 0;
          if (!findSymbolRangeOnLine(fieldLineText, field.name, fieldStart,
                                     fieldEnd)) {
            continue;
          }
          appendDocumentSymbol(children, field.name, 8, field.line, fieldStart,
                               field.line, fieldEnd, field.line, fieldStart,
                               fieldEnd, makeArray());
        }
      } else {
        for (const auto &field : it->fields) {
          const std::string fieldLineText = getLineAt(text, field.line);
          int fieldStart = 0;
          int fieldEnd = 0;
          if (!findSymbolRangeOnLine(fieldLineText, field.name, fieldStart,
                                     fieldEnd)) {
            continue;
          }
          appendDocumentSymbol(children, field.name, 8, field.line, fieldStart,
                               field.line, fieldEnd, field.line, fieldStart,
                               fieldEnd, makeArray());
        }
      }
      appendDocumentSymbol(symbols, it->name, 23, it->line, startChar, endLine,
                           utf16LineLength(lines, endLine), it->line, startChar,
                           endChar, std::move(children));
      continue;
    }

    if (decl.kind == HlslTopLevelDeclKind::CBuffer) {
      auto it = std::find_if(
          astDocument->cbuffers.begin(), astDocument->cbuffers.end(),
          [&](const HlslAstCBufferDecl &cbufferDecl) {
            return cbufferDecl.line == decl.line && cbufferDecl.name == decl.name;
          });
      if (it == astDocument->cbuffers.end())
        continue;
      const std::string lineText = getLineAt(text, it->line);
      int startChar = 0;
      int endChar = 0;
      findSymbolRangeOnLine(lineText, it->name, startChar, endChar);
      const int endLine =
          it->hasBody ? findBlockEndLine(lines, it->line) : it->line;
      Json children = makeArray();
      for (const auto &field : it->fields) {
        const std::string fieldLineText = getLineAt(text, field.line);
        int fieldStart = 0;
        int fieldEnd = 0;
        if (!findSymbolRangeOnLine(fieldLineText, field.name, fieldStart,
                                   fieldEnd)) {
          continue;
        }
        appendDocumentSymbol(children, field.name, 8, field.line, fieldStart,
                             field.line, fieldEnd, field.line, fieldStart,
                             fieldEnd, makeArray());
      }
      appendDocumentSymbol(symbols, it->name, 3, it->line, startChar, endLine,
                           utf16LineLength(lines, endLine), it->line, startChar,
                           endChar, std::move(children));
    }
  }

  appendTechniqueOrPassSymbols();
  return symbols;
}

std::shared_ptr<const DeferredDocSnapshot> buildDeferredSnapshotFromInputs(
    const Document &doc, const DeferredDocBuildContext &context,
    const AnalysisSnapshotKey &analysisKey) {
  auto deferred = std::make_shared<DeferredDocSnapshot>();
  deferred->key = analysisKey;
  deferred->documentEpoch = doc.epoch;
  deferred->documentVersion = doc.version;
  deferred->astDocument = std::make_shared<HlslAstDocument>(
      buildHlslAstDocument(
          buildLinePreservingExpandedSource(doc.text, context.defines)));
  deferred->semanticSnapshot = getSemanticSnapshotView(
      doc.uri, doc.text, doc.epoch, context.workspaceFolders,
      context.includePaths, context.shaderExtensions, context.defines);
  deferred->semanticTokensFull =
      buildSemanticTokensFull(doc.text, context.semanticLegend);
  deferred->hasSemanticTokensFull = true;
  deferred->documentSymbols =
      buildDocumentSymbolsFromAst(doc.text, deferred->astDocument.get(),
                                  deferred->semanticSnapshot.get());
  deferred->hasDocumentSymbols = true;
  if (context.prewarmFullDiagnostics) {
    DiagnosticsBuildOptions diagnosticsOptions = context.diagnosticsOptions;
    diagnosticsOptions.documentEpoch = doc.epoch;
    DiagnosticsBuildResult diagnostics = buildDiagnosticsWithOptions(
        doc.uri, doc.text, context.workspaceFolders, context.includePaths,
        context.shaderExtensions, context.defines, diagnosticsOptions);
    deferred->fullDiagnostics = diagnostics.diagnostics;
    deferred->hasFullDiagnostics = true;
    deferred->fullDiagnosticsFingerprint =
        buildDiagnosticsFingerprint(diagnosticsOptions);
  }
  if (context.prewarmInlayHints && context.inlayHintsEnabled &&
      context.inlayHintsParameterNamesEnabled) {
    ServerRequestContext requestContext;
    requestContext.documents = context.documents;
    requestContext.workspaceFolders = context.workspaceFolders;
    requestContext.includePaths = context.includePaths;
    requestContext.shaderExtensions = context.shaderExtensions;
    requestContext.semanticLegend = context.semanticLegend;
    requestContext.preprocessorDefines = context.defines;
    requestContext.inlayHintsEnabled = context.inlayHintsEnabled;
    requestContext.inlayHintsParameterNamesEnabled =
        context.inlayHintsParameterNamesEnabled;
    deferred->inlayHintsFull =
        inlayHintsRuntimeBuildFullDocument(doc.uri, doc, requestContext);
    deferred->hasInlayHintsFull = true;
  }
  deferred->builtAtMs = currentTimeMs();
  return deferred;
}

void runDeferredDocWorker() {
  while (true) {
    DeferredDocJob job;
    {
      std::unique_lock<std::mutex> lock(gDeferredDocMutex);
      while (true) {
        gDeferredDocCv.wait(lock, []() {
          return gDeferredDocStopping || !gDeferredDocQueue.empty();
        });
        if (gDeferredDocStopping && gDeferredDocQueue.empty())
          return;
        if (gDeferredDocQueue.empty())
          continue;

        const std::string &uri = gDeferredDocQueue.front();
        auto it = gDeferredDocPendingByUri.find(uri);
        if (it == gDeferredDocPendingByUri.end()) {
          gDeferredDocQueue.pop_front();
          continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (it->second.notBefore > now) {
          gDeferredDocCv.wait_until(lock, it->second.notBefore, [&]() {
            return gDeferredDocStopping;
          });
          continue;
        }

        job = std::move(it->second);
        gDeferredDocPendingByUri.erase(it);
        gDeferredDocQueue.pop_front();
        break;
      }
    }

    DocumentRuntime runtime;
    if (!documentOwnerGetRuntime(job.document.uri, runtime)) {
      recordDeferredStaleDrop();
      continue;
    }
    if (runtime.version != job.document.version ||
        runtime.epoch != job.document.epoch) {
      recordDeferredStaleDrop(true);
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(gDeferredDocMutex);
      auto latestIt = gDeferredDocLatestVersionByUri.find(job.document.uri);
      if (latestIt != gDeferredDocLatestVersionByUri.end() &&
          latestIt->second > job.document.version) {
        recordDeferredStaleDrop(true);
        continue;
      }
    }

    if (job.enqueuedAt.time_since_epoch().count() > 0) {
      const double queueWaitMs = static_cast<double>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - job.enqueuedAt)
              .count());
      recordDeferredQueueWait(queueWaitMs);
    }
    const auto buildStartedAt = std::chrono::steady_clock::now();
    DeferredDocBuildContext effectiveContext =
        makeDeferredContextFromRuntime(runtime, job.context);
    auto deferred =
        buildDeferredSnapshotFromInputs(job.document, effectiveContext,
                                        runtime.analysisSnapshotKey);
    const double buildMs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - buildStartedAt)
            .count());
    recordDeferredBuildDuration(buildMs);
    documentOwnerStoreDeferredSnapshot(job.document.uri, deferred);
  }
}

void ensureDeferredDocWorkerStarted() {
  std::lock_guard<std::mutex> lock(gDeferredDocMutex);
  if (gDeferredDocWorkerStarted)
    return;
  gDeferredDocStopping = false;
  gDeferredDocWorker = std::thread(runDeferredDocWorker);
  gDeferredDocWorkerStarted = true;
}

} // namespace

std::shared_ptr<const DeferredDocSnapshot> getOrBuildDeferredDocSnapshot(
    const std::string &uri, const Document &doc, const ServerRequestContext &ctx) {
  DocumentRuntime runtime;
  if (!documentOwnerGetRuntime(uri, runtime))
    return nullptr;
  if (runtime.deferredDocSnapshot &&
      runtime.deferredDocSnapshot->key.fullFingerprint ==
          runtime.analysisSnapshotKey.fullFingerprint &&
      runtime.deferredDocSnapshot->documentEpoch == doc.epoch &&
      runtime.deferredDocSnapshot->documentVersion == doc.version) {
    return runtime.deferredDocSnapshot;
  }

  DeferredDocBuildContext buildContext;
  buildContext.workspaceFolders = runtime.activeUnitSnapshot.workspaceFolders;
  buildContext.includePaths = runtime.activeUnitSnapshot.includePaths;
  buildContext.shaderExtensions = runtime.activeUnitSnapshot.shaderExtensions;
  buildContext.defines = runtime.activeUnitSnapshot.defines;
  if (buildContext.workspaceFolders.empty() &&
      buildContext.includePaths.empty() &&
      buildContext.shaderExtensions.empty() && buildContext.defines.empty()) {
    buildContext.workspaceFolders = ctx.workspaceFolders;
    buildContext.includePaths = ctx.includePaths;
    buildContext.shaderExtensions = ctx.shaderExtensions;
    buildContext.defines = ctx.preprocessorDefines;
  }
  buildContext.semanticLegend = ctx.semanticLegend;
  const auto buildStartedAt = std::chrono::steady_clock::now();
  auto deferred =
      buildDeferredSnapshotFromInputs(doc, buildContext, runtime.analysisSnapshotKey);
  const double buildMs = static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - buildStartedAt)
          .count());
  recordDeferredBuildDuration(buildMs);
  documentOwnerStoreDeferredSnapshot(uri, deferred);
  return deferred;
}

Json buildDeferredSemanticTokensFull(const std::string &uri, const Document &doc,
                                     const ServerRequestContext &ctx) {
  auto deferred = getOrBuildDeferredDocSnapshot(uri, doc, ctx);
  if (!deferred)
    return buildSemanticTokensFull(doc.text, ctx.semanticLegend);
  if (deferred->hasSemanticTokensFull)
    return deferred->semanticTokensFull;

  auto writable = std::make_shared<DeferredDocSnapshot>(*deferred);
  writable->semanticTokensFull =
      buildSemanticTokensFull(doc.text, ctx.semanticLegend);
  writable->hasSemanticTokensFull = true;
  writable->builtAtMs = currentTimeMs();
  documentOwnerStoreDeferredSnapshot(uri, writable);
  return writable->semanticTokensFull;
}

Json buildDeferredSemanticTokensRange(const std::string &uri, const Document &doc,
                                      int startLine, int startCharacter,
                                      int endLine, int endCharacter,
                                      const ServerRequestContext &ctx) {
  getOrBuildDeferredDocSnapshot(uri, doc, ctx);
  return buildSemanticTokensRange(doc.text, startLine, startCharacter, endLine,
                                  endCharacter, ctx.semanticLegend);
}

Json buildDeferredDocumentSymbols(const std::string &uri, const Document &doc,
                                  const ServerRequestContext &ctx) {
  auto deferred = getOrBuildDeferredDocSnapshot(uri, doc, ctx);
  if (!deferred)
    return makeArray();
  if (deferred->hasDocumentSymbols)
    return deferred->documentSymbols;

  auto writable = std::make_shared<DeferredDocSnapshot>(*deferred);
  writable->documentSymbols =
      buildDocumentSymbolsFromAst(doc.text, writable->astDocument.get(),
                                  writable->semanticSnapshot.get());
  writable->hasDocumentSymbols = true;
  writable->builtAtMs = currentTimeMs();
  documentOwnerStoreDeferredSnapshot(uri, writable);
  return writable->documentSymbols;
}

DiagnosticsBuildResult buildDeferredFullDiagnostics(
    const std::string &uri, const Document &doc, const ServerRequestContext &ctx,
    const DiagnosticsBuildOptions &options) {
  DiagnosticsBuildResult result;
  const std::string fingerprint = buildDiagnosticsFingerprint(options);
  DocumentRuntime runtime;
  if (documentOwnerGetRuntime(uri, runtime) && runtime.deferredDocSnapshot &&
      runtime.deferredDocSnapshot->key.fullFingerprint ==
          runtime.analysisSnapshotKey.fullFingerprint &&
      runtime.deferredDocSnapshot->documentEpoch == doc.epoch &&
      runtime.deferredDocSnapshot->documentVersion == doc.version &&
      runtime.deferredDocSnapshot->hasFullDiagnostics &&
      runtime.deferredDocSnapshot->fullDiagnosticsFingerprint == fingerprint) {
    result.diagnostics = runtime.deferredDocSnapshot->fullDiagnostics;
    return result;
  }

  std::vector<std::string> workspaceFolders = runtime.activeUnitSnapshot.workspaceFolders;
  std::vector<std::string> includePaths = runtime.activeUnitSnapshot.includePaths;
  std::vector<std::string> shaderExtensions = runtime.activeUnitSnapshot.shaderExtensions;
  std::unordered_map<std::string, int> defines = runtime.activeUnitSnapshot.defines;
  if (workspaceFolders.empty() && includePaths.empty() &&
      shaderExtensions.empty() && defines.empty()) {
    workspaceFolders = ctx.workspaceFolders;
    includePaths = ctx.includePaths;
    shaderExtensions = ctx.shaderExtensions;
    defines = ctx.preprocessorDefines;
  }

  DiagnosticsBuildOptions buildOptions = options;
  buildOptions.activeUnitUri = runtime.activeUnitSnapshot.uri;
  if (buildOptions.activeUnitUri.empty() && !runtime.activeUnitSnapshot.path.empty()) {
    buildOptions.activeUnitUri = pathToUri(runtime.activeUnitSnapshot.path);
  }
  if (!buildOptions.activeUnitUri.empty()) {
    auto it = ctx.documents.find(buildOptions.activeUnitUri);
    if (it != ctx.documents.end()) {
      buildOptions.activeUnitText = it->second.text;
    } else if (!runtime.activeUnitSnapshot.path.empty()) {
      readFileText(runtime.activeUnitSnapshot.path, buildOptions.activeUnitText);
    }
  }

  result = buildDiagnosticsWithOptions(uri, doc.text, workspaceFolders,
                                       includePaths, shaderExtensions, defines,
                                       buildOptions);
  auto deferred = getOrBuildDeferredDocSnapshot(uri, doc, ctx);
  if (!deferred)
    return result;
  auto writable = std::make_shared<DeferredDocSnapshot>(*deferred);
  writable->fullDiagnostics = result.diagnostics;
  writable->hasFullDiagnostics = true;
  writable->fullDiagnosticsFingerprint = fingerprint;
  writable->builtAtMs = currentTimeMs();
  documentOwnerStoreDeferredSnapshot(uri, writable);
  return result;
}

void deferredDocRuntimeInvalidateInlayHints(const std::string &uri) {
  if (uri.empty())
    return;
  DocumentRuntime runtime;
  if (!documentOwnerGetRuntime(uri, runtime) || !runtime.deferredDocSnapshot ||
      !runtime.deferredDocSnapshot->hasInlayHintsFull) {
    return;
  }
  auto writable = std::make_shared<DeferredDocSnapshot>(*runtime.deferredDocSnapshot);
  writable->inlayHintsFull = makeArray();
  writable->hasInlayHintsFull = false;
  writable->builtAtMs = currentTimeMs();
  documentOwnerStoreDeferredSnapshot(uri, writable);
}

void deferredDocRuntimeSchedule(const Document &doc,
                                const DeferredDocBuildContext &context) {
  ensureDeferredDocWorkerStarted();
  std::lock_guard<std::mutex> lock(gDeferredDocMutex);
  const bool mergedLatestOnly = gDeferredDocPendingByUri.find(doc.uri) !=
                                gDeferredDocPendingByUri.end();
  const auto now = std::chrono::steady_clock::now();
  gDeferredDocLatestVersionByUri[doc.uri] = doc.version;
  gDeferredDocPendingByUri[doc.uri] = DeferredDocJob{
      doc, context, now, now + kDeferredDocLatestOnlyCoalesceWindow};
  recordDeferredScheduled(mergedLatestOnly);
  if (!mergedLatestOnly)
    gDeferredDocQueue.push_back(doc.uri);
  gDeferredDocCv.notify_all();
}

DeferredDocRuntimeMetricsSnapshot takeDeferredDocRuntimeMetricsSnapshot() {
  std::lock_guard<std::mutex> lock(gDeferredDocMetricsMutex);
  DeferredDocRuntimeMetricsSnapshot snapshot = gDeferredDocMetrics;
  gDeferredDocMetrics = DeferredDocRuntimeMetricsSnapshot{};
  return snapshot;
}

void deferredDocRuntimeShutdown() {
  std::unique_lock<std::mutex> lock(gDeferredDocMutex);
  if (!gDeferredDocWorkerStarted)
    return;
  gDeferredDocStopping = true;
  gDeferredDocPendingByUri.clear();
  gDeferredDocQueue.clear();
  gDeferredDocLatestVersionByUri.clear();
  lock.unlock();
  gDeferredDocCv.notify_all();
  if (gDeferredDocWorker.joinable())
    gDeferredDocWorker.join();
  lock.lock();
  gDeferredDocWorkerStarted = false;
}
