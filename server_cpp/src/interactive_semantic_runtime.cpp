#include "interactive_semantic_runtime.hpp"

#include "document_owner.hpp"
#include "document_runtime.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_markdown.hpp"
#include "nsf_lexer.hpp"
#include "server_parse.hpp"
#include "semantic_snapshot.hpp"
#include "server_request_handlers.hpp"
#include "text_utils.hpp"
#include "type_desc.hpp"
#include "workspace_summary_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

uint64_t currentTimeMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

int lineIndexForOffset(const std::string &text, size_t offset) {
  offset = std::min(offset, text.size());
  int line = 0;
  for (size_t i = 0; i < offset; i++) {
    if (text[i] == '\n')
      line++;
  }
  return line;
}

bool isStaleEligible(const AnalysisSnapshotKey &candidate,
                     const AnalysisSnapshotKey &current) {
  return candidate.stableContextFingerprint == current.stableContextFingerprint;
}

struct RuntimeSemanticInputs {
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  std::unordered_map<std::string, int> defines;
};

struct InteractiveRuntimeMetricState {
  uint64_t snapshotRequests = 0;
  uint64_t analysisKeyHits = 0;
  uint64_t snapshotBuildAttempts = 0;
  uint64_t snapshotBuildSuccess = 0;
  uint64_t snapshotBuildFailed = 0;
  uint64_t lastGoodServed = 0;
  uint64_t incrementalPromoted = 0;
  uint64_t noSnapshotAvailable = 0;
  uint64_t mergeCurrentDocHits = 0;
  uint64_t mergeLastGoodHits = 0;
  uint64_t mergeDeferredDocHits = 0;
  uint64_t mergeWorkspaceSummaryHits = 0;
  uint64_t mergeMisses = 0;
  uint64_t snapshotWaitSamples = 0;
  double snapshotWaitTotalMs = 0.0;
  double snapshotWaitMaxMs = 0.0;
};

std::mutex gInteractiveMetricsMutex;
InteractiveRuntimeMetricState gInteractiveMetrics;

void recordInteractiveSnapshotWait(double waitMs) {
  std::lock_guard<std::mutex> lock(gInteractiveMetricsMutex);
  gInteractiveMetrics.snapshotWaitSamples++;
  gInteractiveMetrics.snapshotWaitTotalMs += waitMs;
  gInteractiveMetrics.snapshotWaitMaxMs =
      std::max(gInteractiveMetrics.snapshotWaitMaxMs, waitMs);
}

void recordInteractiveMetric(const std::function<void(InteractiveRuntimeMetricState &)> &fn) {
  std::lock_guard<std::mutex> lock(gInteractiveMetricsMutex);
  fn(gInteractiveMetrics);
}

RuntimeSemanticInputs getRuntimeSemanticInputs(const DocumentRuntime &runtime,
                                               const ServerRequestContext &ctx) {
  RuntimeSemanticInputs inputs;
  if (!runtime.activeUnitSnapshot.workspaceFolders.empty() ||
      !runtime.activeUnitSnapshot.includePaths.empty() ||
      !runtime.activeUnitSnapshot.shaderExtensions.empty() ||
      !runtime.activeUnitSnapshot.defines.empty()) {
    inputs.workspaceFolders = runtime.activeUnitSnapshot.workspaceFolders;
    inputs.includePaths = runtime.activeUnitSnapshot.includePaths;
    inputs.shaderExtensions = runtime.activeUnitSnapshot.shaderExtensions;
    inputs.defines = runtime.activeUnitSnapshot.defines;
    return inputs;
  }
  inputs.workspaceFolders = ctx.workspaceFolders;
  inputs.includePaths = ctx.includePaths;
  inputs.shaderExtensions = ctx.shaderExtensions;
  inputs.defines = ctx.preprocessorDefines;
  return inputs;
}

const SemanticSnapshot::FunctionInfo *
findBestFunctionContainingLine(const SemanticSnapshot &snapshot, int line);

bool lineLooksSemanticNeutral(const std::string &lineText) {
  const std::string trimmed = trimRightCopy(trimLeftCopy(lineText));
  if (trimmed.empty())
    return true;
  return trimmed.rfind("//", 0) == 0 || trimmed.rfind("/*", 0) == 0 ||
         trimmed.rfind("*", 0) == 0 || trimmed.rfind("*/", 0) == 0;
}

bool isLikelySemanticNeutralEdit(const DocumentRuntime &runtime,
                                 const Document &doc) {
  if (runtime.semanticNeutralEditHint)
    return true;
  if (runtime.changedRanges.empty() || runtime.changedRanges.size() > 4)
    return false;
  int inspectedLines = 0;
  for (const auto &range : runtime.changedRanges) {
    const int startLine = std::max(0, range.startLine);
    const int endLine = std::max(startLine, std::max(range.endLine, range.newEndLine));
    if (endLine - startLine > 6)
      return false;
    for (int line = startLine; line <= endLine; line++) {
      inspectedLines++;
      if (inspectedLines > 18)
        return false;
      if (!lineLooksSemanticNeutral(getLineAt(doc.text, line)))
        return false;
    }
  }
  return inspectedLines > 0;
}

bool isEligibleIncrementalPromoteEdit(const DocumentRuntime &runtime,
                                      const Document &doc) {
  if (runtime.syntaxOnlyEditHint)
    return true;
  return isLikelySemanticNeutralEdit(runtime, doc);
}

std::shared_ptr<const InteractiveSnapshot> tryPromoteIncrementalSnapshot(
    const std::string &uri, const Document &doc, const DocumentRuntime &runtime) {
  if (!runtime.lastGoodInteractiveSnapshot)
    return nullptr;
  if (!isStaleEligible(runtime.lastGoodInteractiveSnapshot->key,
                       runtime.analysisSnapshotKey)) {
    return nullptr;
  }
  if (!isEligibleIncrementalPromoteEdit(runtime, doc))
    return nullptr;

  auto promoted =
      std::make_shared<InteractiveSnapshot>(*runtime.lastGoodInteractiveSnapshot);
  promoted->key = runtime.analysisSnapshotKey;
  promoted->documentEpoch = doc.epoch;
  promoted->documentVersion = doc.version;
  promoted->builtAtMs = currentTimeMs();
  return promoted;
}

const SemanticSnapshot::FunctionInfo *
findBestFunctionForLocation(const SemanticSnapshot &snapshot,
                            const std::string &name, int lineIndex,
                            int nameCharacter) {
  auto it = snapshot.functionsByName.find(name);
  if (it == snapshot.functionsByName.end() || it->second.empty())
    return nullptr;
  for (size_t index : it->second) {
    if (index >= snapshot.functions.size())
      continue;
    const auto &function = snapshot.functions[index];
    if (function.line == lineIndex && function.character == nameCharacter)
      return &function;
  }
  for (size_t index : it->second) {
    if (index >= snapshot.functions.size())
      continue;
    const auto &function = snapshot.functions[index];
    if (function.line == lineIndex)
      return &function;
  }
  return &snapshot.functions[it->second.front()];
}

bool lookupTypeAtOffsetInSnapshot(const SemanticSnapshot &snapshot,
                                  const std::string &docText,
                                  const std::string &symbol, size_t offset,
                                  std::string &typeOut, bool &isParamOut) {
  typeOut.clear();
  isParamOut = false;
  if (symbol.empty())
    return false;

  const int line = lineIndexForOffset(docText, offset);
  const SemanticSnapshot::FunctionInfo *bestFunction = nullptr;
  int bestSpan = 0;
  for (const auto &function : snapshot.functions) {
    const int visibleEndLine =
        function.hasBody && function.bodyEndLine >= 0
            ? function.bodyEndLine
            : (function.signatureEndLine >= 0 ? function.signatureEndLine
                                              : function.line);
    if (line < function.line || line > visibleEndLine)
      continue;
    const int span = visibleEndLine - function.line;
    if (!bestFunction || span < bestSpan) {
      bestFunction = &function;
      bestSpan = span;
    }
  }

  if (bestFunction) {
    const SemanticSnapshot::FunctionInfo::LocalInfo *bestLocal = nullptr;
    for (const auto &local : bestFunction->locals) {
      if (local.name != symbol || local.offset > offset)
        continue;
      if (!bestLocal || local.depth > bestLocal->depth ||
          (local.depth == bestLocal->depth && local.offset >= bestLocal->offset)) {
        bestLocal = &local;
      }
    }
    if (bestLocal) {
      typeOut = bestLocal->type;
      return !typeOut.empty();
    }
    for (const auto &parameter : bestFunction->parameterInfos) {
      if (parameter.first != symbol)
        continue;
      typeOut = parameter.second;
      isParamOut = true;
      return !typeOut.empty();
    }
  }

  auto globalIt = snapshot.globalByName.find(symbol);
  if (globalIt != snapshot.globalByName.end() &&
      globalIt->second < snapshot.globals.size()) {
    typeOut = snapshot.globals[globalIt->second].type;
    return !typeOut.empty();
  }
  return false;
}

bool appendOverloadsFromSnapshot(
    const SemanticSnapshot &snapshot, const std::string &name,
    std::vector<SemanticSnapshotFunctionOverloadInfo> &overloadsOut) {
  auto it = snapshot.functionsByName.find(name);
  if (it == snapshot.functionsByName.end() || it->second.empty())
    return false;
  for (size_t index : it->second) {
    if (index >= snapshot.functions.size())
      continue;
    const auto &function = snapshot.functions[index];
    SemanticSnapshotFunctionOverloadInfo info;
    info.label = function.label;
    info.parameters = function.parameters;
    info.returnType = function.returnType;
    info.line = function.line;
    info.character = function.character;
    info.hasBody = function.hasBody;
    overloadsOut.push_back(std::move(info));
  }
  return !overloadsOut.empty();
}

bool resolveFunctionSignatureFromWorkspaceSummary(
    const std::string &name, const ServerRequestContext &ctx,
    std::string &labelOut, std::vector<std::string> &parametersOut) {
  std::vector<IndexedDefinition> defs;
  if (!workspaceSummaryRuntimeFindDefinitions(name, defs, 64))
    return false;

  for (const auto &def : defs) {
    if (def.kind != 12 || def.uri.empty())
      continue;
    std::string candidateText;
    if (!ctx.readDocumentText(def.uri, candidateText))
      continue;
    uint64_t candidateEpoch = 0;
    if (const Document *candidateDoc = ctx.findDocument(def.uri))
      candidateEpoch = candidateDoc->epoch;
    if (querySemanticSnapshotFunctionSignature(
            def.uri, candidateText, candidateEpoch, ctx.workspaceFolders,
            ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
            name, def.line, def.start, labelOut, parametersOut) &&
        !labelOut.empty()) {
      return true;
    }
  }
  return false;
}

bool appendWorkspaceSummaryOverloads(
    const std::string &name, const ServerRequestContext &ctx,
    std::vector<SemanticSnapshotFunctionOverloadInfo> &overloadsOut) {
  std::vector<IndexedDefinition> defs;
  if (!workspaceSummaryRuntimeFindDefinitions(name, defs, 64))
    return false;

  std::unordered_set<std::string> seen;
  const size_t before = overloadsOut.size();
  for (const auto &def : defs) {
    if (def.kind != 12 || def.uri.empty())
      continue;
    std::string candidateText;
    if (!ctx.readDocumentText(def.uri, candidateText))
      continue;
    uint64_t candidateEpoch = 0;
    if (const Document *candidateDoc = ctx.findDocument(def.uri))
      candidateEpoch = candidateDoc->epoch;
    std::vector<SemanticSnapshotFunctionOverloadInfo> localOverloads;
    if (!querySemanticSnapshotFunctionOverloads(
            def.uri, candidateText, candidateEpoch, ctx.workspaceFolders,
            ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
            name, localOverloads)) {
      continue;
    }
    for (const auto &overload : localOverloads) {
      const std::string key =
          overload.label + "|" + std::to_string(overload.line) + "|" +
          std::to_string(overload.character);
      if (!seen.insert(key).second)
        continue;
      overloadsOut.push_back(overload);
    }
  }
  return overloadsOut.size() > before;
}

bool lookupStructFieldInfosFromSnapshot(const SemanticSnapshot &snapshot,
                                        const std::string &ownerType,
                                        std::vector<MemberCompletionField> &fieldsOut) {
  auto it = snapshot.structByName.find(ownerType);
  if (it == snapshot.structByName.end() || it->second >= snapshot.structs.size())
    return false;
  fieldsOut.clear();
  fieldsOut.reserve(snapshot.structs[it->second].fields.size());
  for (const auto &field : snapshot.structs[it->second].fields) {
    MemberCompletionField item;
    item.name = field.name;
    item.type = field.type;
    fieldsOut.push_back(std::move(item));
  }
  return !fieldsOut.empty();
}

bool findSymbolRangeOnLine(const std::string &lineText, const std::string &symbol,
                           int &startOut, int &endOut,
                           size_t minByteOffset = 0) {
  startOut = 0;
  endOut = 0;
  if (lineText.empty() || symbol.empty())
    return false;

  size_t searchFrom = std::min(minByteOffset, lineText.size());
  while (searchFrom <= lineText.size()) {
    size_t pos = lineText.find(symbol, searchFrom);
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

bool makeDefinitionLocationFromLine(const std::string &uri,
                                    const std::string &docText, int line,
                                    const std::string &symbol,
                                    DefinitionLocation &outLocation,
                                    size_t minByteOffset = 0) {
  if (line < 0)
    return false;
  const std::string lineText = getLineAt(docText, line);
  int start = 0;
  int end = 0;
  if (!findSymbolRangeOnLine(lineText, symbol, start, end, minByteOffset))
    return false;
  outLocation.uri = uri;
  outLocation.line = line;
  outLocation.start = start;
  outLocation.end = end;
  return true;
}

bool makeDefinitionLocationFromOffset(const std::string &uri,
                                      const std::string &docText, size_t offset,
                                      const std::string &symbol,
                                      DefinitionLocation &outLocation) {
  offset = std::min(offset, docText.size());
  size_t lineStart = 0;
  int line = 0;
  for (size_t index = 0; index < offset; index++) {
    if (docText[index] == '\n') {
      line++;
      lineStart = index + 1;
    }
  }
  size_t lineEnd = docText.find('\n', lineStart);
  if (lineEnd == std::string::npos)
    lineEnd = docText.size();
  const std::string lineText = docText.substr(lineStart, lineEnd - lineStart);
  const int start = byteOffsetInLineToUtf16(
      lineText, static_cast<int>(offset - lineStart));
  const int end = byteOffsetInLineToUtf16(
      lineText, static_cast<int>(std::min(lineText.size(),
                                          (offset - lineStart) +
                                              symbol.size())));
  outLocation.uri = uri;
  outLocation.line = line;
  outLocation.start = start;
  outLocation.end = end;
  return true;
}

bool findFunctionParameterDefinition(const std::string &uri,
                                     const std::string &docText,
                                     const SemanticSnapshot::FunctionInfo &fn,
                                     const std::string &symbol,
                                     DefinitionLocation &outLocation) {
  if (fn.line < 0)
    return false;
  const int endLine =
      fn.signatureEndLine >= fn.line ? fn.signatureEndLine : fn.line;
  for (int line = fn.line; line <= endLine; line++) {
    size_t minByteOffset = 0;
    if (line == fn.line && fn.character >= 0) {
      const std::string lineText = getLineAt(docText, line);
      minByteOffset = static_cast<size_t>(std::max(
          0, utf16ToByteOffsetInLine(lineText, fn.character)));
    }
    if (makeDefinitionLocationFromLine(uri, docText, line, symbol, outLocation,
                                       minByteOffset)) {
      return true;
    }
  }
  return false;
}

const SemanticSnapshot::FunctionInfo *
findBestFunctionDefinitionCandidate(const SemanticSnapshot &snapshot,
                                    const std::string &name,
                                    size_t cursorOffset,
                                    const std::string &docText) {
  auto byNameIt = snapshot.functionsByName.find(name);
  if (byNameIt == snapshot.functionsByName.end() || byNameIt->second.empty())
    return nullptr;

  const int line = lineIndexForOffset(docText, cursorOffset);
  const SemanticSnapshot::FunctionInfo *best = nullptr;
  int bestDistance = std::numeric_limits<int>::max();
  for (size_t index : byNameIt->second) {
    if (index >= snapshot.functions.size())
      continue;
    const auto &fn = snapshot.functions[index];
    if (fn.line < 0)
      continue;
    int distance = std::abs(fn.line - line);
    if (fn.line <= line)
      distance /= 2;
    if (!best || distance < bestDistance) {
      best = &fn;
      bestDistance = distance;
    }
  }
  return best ? best : &snapshot.functions[byNameIt->second.front()];
}

bool tryResolveDefinitionFromSnapshot(const SemanticSnapshot &snapshot,
                                      const std::string &uri,
                                      const std::string &docText,
                                      const std::string &symbol,
                                      size_t cursorOffset,
                                      DefinitionLocation &outLocation) {
  const int line = lineIndexForOffset(docText, cursorOffset);
  const auto *bestFunction = findBestFunctionContainingLine(snapshot, line);
  if (bestFunction) {
    const SemanticSnapshot::FunctionInfo::LocalInfo *bestLocal = nullptr;
    for (const auto &local : bestFunction->locals) {
      if (local.name != symbol || local.offset > cursorOffset)
        continue;
      if (!bestLocal || local.depth > bestLocal->depth ||
          (local.depth == bestLocal->depth &&
           local.offset >= bestLocal->offset)) {
        bestLocal = &local;
      }
    }
    if (bestLocal &&
        makeDefinitionLocationFromOffset(uri, docText, bestLocal->offset, symbol,
                                         outLocation)) {
      return true;
    }

    for (const auto &parameter : bestFunction->parameterInfos) {
      if (parameter.first == symbol &&
          findFunctionParameterDefinition(uri, docText, *bestFunction, symbol,
                                          outLocation)) {
        return true;
      }
    }
  }

  const auto *functionCandidate =
      findBestFunctionDefinitionCandidate(snapshot, symbol, cursorOffset, docText);
  if (functionCandidate &&
      makeDefinitionLocationFromLine(
          uri, docText, functionCandidate->line, symbol, outLocation,
          functionCandidate->character >= 0
              ? static_cast<size_t>(std::max(
                    0, utf16ToByteOffsetInLine(getLineAt(docText,
                                                         functionCandidate->line),
                                               functionCandidate->character)))
              : 0)) {
    return true;
  }

  auto structIt = snapshot.structByName.find(symbol);
  if (structIt != snapshot.structByName.end() &&
      structIt->second < snapshot.structs.size() &&
      makeDefinitionLocationFromLine(uri, docText,
                                     snapshot.structs[structIt->second].line,
                                     symbol, outLocation)) {
    return true;
  }

  auto globalIt = snapshot.globalByName.find(symbol);
  if (globalIt != snapshot.globalByName.end() &&
      globalIt->second < snapshot.globals.size() &&
      makeDefinitionLocationFromLine(uri, docText,
                                     snapshot.globals[globalIt->second].line,
                                     symbol, outLocation)) {
    return true;
  }

  return false;
}

bool matchesCompletionPrefix(const std::string &label,
                             const std::string &prefix) {
  if (prefix.empty())
    return true;
  if (label.size() < prefix.size())
    return false;
  for (size_t i = 0; i < prefix.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(label[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

const SemanticSnapshot::FunctionInfo *
findBestFunctionContainingLine(const SemanticSnapshot &snapshot, int line) {
  const SemanticSnapshot::FunctionInfo *bestFunction = nullptr;
  int bestSpan = 0;
  for (const auto &function : snapshot.functions) {
    const int visibleEndLine =
        function.hasBody && function.bodyEndLine >= 0
            ? function.bodyEndLine
            : (function.signatureEndLine >= 0 ? function.signatureEndLine
                                              : function.line);
    if (line < function.line || line > visibleEndLine)
      continue;
    const int span = visibleEndLine - function.line;
    if (!bestFunction || span < bestSpan) {
      bestFunction = &function;
      bestSpan = span;
    }
  }
  return bestFunction;
}

void appendInteractiveCompletionCandidate(
    std::vector<InteractiveCompletionItem> &outItems,
    std::unordered_set<std::string> &seen, const std::string &label, int kind,
    const std::string &detail, const std::string &prefix) {
  if (label.empty() || !matchesCompletionPrefix(label, prefix) ||
      !seen.insert(label).second) {
    return;
  }
  InteractiveCompletionItem item;
  item.label = label;
  item.kind = kind;
  item.detail = detail;
  outItems.push_back(std::move(item));
}

bool appendCompletionItemsFromSnapshot(
    const SemanticSnapshot &snapshot, const std::string &docText,
    size_t cursorOffset, const std::string &prefix,
    std::vector<InteractiveCompletionItem> &outItems,
    std::unordered_set<std::string> &seen) {
  const size_t beforeCount = outItems.size();
  const int line = lineIndexForOffset(docText, cursorOffset);
  const auto *bestFunction = findBestFunctionContainingLine(snapshot, line);
  if (bestFunction) {
    for (const auto &parameter : bestFunction->parameterInfos) {
      appendInteractiveCompletionCandidate(outItems, seen, parameter.first, 6,
                                           parameter.second, prefix);
    }

    std::unordered_map<std::string,
                       const SemanticSnapshot::FunctionInfo::LocalInfo *>
        bestLocals;
    for (const auto &local : bestFunction->locals) {
      if (local.offset > cursorOffset)
        continue;
      auto it = bestLocals.find(local.name);
      if (it == bestLocals.end() || local.depth > it->second->depth ||
          (local.depth == it->second->depth &&
           local.offset >= it->second->offset)) {
        bestLocals[local.name] = &local;
      }
    }
    for (const auto &entry : bestLocals) {
      appendInteractiveCompletionCandidate(outItems, seen, entry.first, 6,
                                           entry.second->type, prefix);
    }
  }

  for (const auto &function : snapshot.functions) {
    appendInteractiveCompletionCandidate(outItems, seen, function.name, 3,
                                         function.returnType, prefix);
  }
  for (const auto &global : snapshot.globals) {
    appendInteractiveCompletionCandidate(outItems, seen, global.name, 6,
                                         global.type, prefix);
  }
  for (const auto &structInfo : snapshot.structs) {
    appendInteractiveCompletionCandidate(outItems, seen, structInfo.name, 7,
                                         "struct", prefix);
  }
  return outItems.size() > beforeCount;
}

int completionKindForWorkspaceDefinition(const IndexedDefinition &def) {
  if (def.kind == 12)
    return 3;
  if (def.kind == 23)
    return 7;
  return 6;
}

std::shared_ptr<const InteractiveSnapshot>
buildCurrentInteractiveSnapshot(const std::string &uri, const Document &doc,
                                const ServerRequestContext &ctx,
                                const DocumentRuntime &runtime) {
  const auto buildStartedAt = std::chrono::steady_clock::now();
  const RuntimeSemanticInputs semanticInputs =
      getRuntimeSemanticInputs(runtime, ctx);
  auto semanticSnapshot = getSemanticSnapshotView(
      uri, doc.text, doc.epoch, semanticInputs.workspaceFolders,
      semanticInputs.includePaths, semanticInputs.shaderExtensions,
      semanticInputs.defines);
  if (!semanticSnapshot)
    return nullptr;

  auto interactive = std::make_shared<InteractiveSnapshot>();
  interactive->key = runtime.analysisSnapshotKey;
  interactive->documentEpoch = doc.epoch;
  interactive->documentVersion = doc.version;
  interactive->semanticSnapshot = std::move(semanticSnapshot);
  interactive->builtAtMs = currentTimeMs();
  const double buildMs = static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - buildStartedAt)
          .count());
  recordInteractiveSnapshotWait(buildMs);
  return interactive;
}

void collectEligibleSnapshots(
    const DocumentRuntime &runtime,
    std::shared_ptr<const InteractiveSnapshot> &lastGoodOut,
    std::shared_ptr<const DeferredDocSnapshot> &deferredOut) {
  lastGoodOut.reset();
  deferredOut.reset();
  if (runtime.lastGoodInteractiveSnapshot &&
      isStaleEligible(runtime.lastGoodInteractiveSnapshot->key,
                      runtime.analysisSnapshotKey)) {
    lastGoodOut = runtime.lastGoodInteractiveSnapshot;
  }
  if (runtime.deferredDocSnapshot &&
      isStaleEligible(runtime.deferredDocSnapshot->key,
                      runtime.analysisSnapshotKey)) {
    deferredOut = runtime.deferredDocSnapshot;
  }
}

} // namespace

std::shared_ptr<const InteractiveSnapshot> getOrBuildInteractiveSnapshot(
    const std::string &uri, const Document &doc, const ServerRequestContext &ctx,
    bool *usedLastGoodOut) {
  if (usedLastGoodOut)
    *usedLastGoodOut = false;
  recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
    state.snapshotRequests++;
  });
  DocumentRuntime runtime;
  if (!documentOwnerGetRuntime(uri, runtime)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.noSnapshotAvailable++;
    });
    return nullptr;
  }
  if (runtime.interactiveSnapshot &&
      runtime.interactiveSnapshot->key.fullFingerprint ==
          runtime.analysisSnapshotKey.fullFingerprint &&
      runtime.interactiveSnapshot->documentEpoch == doc.epoch &&
      runtime.interactiveSnapshot->documentVersion == doc.version) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.analysisKeyHits++;
      state.mergeCurrentDocHits++;
    });
    return runtime.interactiveSnapshot;
  }

  if (runtime.lastGoodInteractiveSnapshot &&
      isStaleEligible(runtime.lastGoodInteractiveSnapshot->key,
                      runtime.analysisSnapshotKey)) {
    if (usedLastGoodOut)
      *usedLastGoodOut = true;
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.lastGoodServed++;
      state.mergeLastGoodHits++;
    });
    return runtime.lastGoodInteractiveSnapshot;
  }
  recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
    state.noSnapshotAvailable++;
    state.mergeMisses++;
  });
  return nullptr;
}

void interactiveSemanticRuntimePrewarm(const std::string &uri,
                                       const Document &doc,
                                       const ServerRequestContext &ctx) {
  DocumentRuntime runtime;
  if (!documentRuntimeGet(uri, runtime))
    return;
  if (runtime.interactiveSnapshot &&
      runtime.interactiveSnapshot->key.fullFingerprint ==
          runtime.analysisSnapshotKey.fullFingerprint &&
      runtime.interactiveSnapshot->documentEpoch == doc.epoch &&
      runtime.interactiveSnapshot->documentVersion == doc.version) {
    return;
  }
  if (auto promoted = tryPromoteIncrementalSnapshot(uri, doc, runtime)) {
    documentRuntimeStoreInteractiveSnapshot(uri, promoted);
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.incrementalPromoted++;
      state.snapshotBuildSuccess++;
    });
    return;
  }
  recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
    state.snapshotBuildAttempts++;
  });
  if (auto built = buildCurrentInteractiveSnapshot(uri, doc, ctx, runtime)) {
    documentRuntimeStoreInteractiveSnapshot(uri, built);
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.snapshotBuildSuccess++;
    });
    return;
  }
  recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
    state.snapshotBuildFailed++;
  });
}

void interactiveCollectCompletionItems(
    const std::string &uri, const Document &doc, size_t cursorOffset,
    const std::string &prefix, const ServerRequestContext &ctx,
    std::vector<InteractiveCompletionItem> &outItems) {
  outItems.clear();
  DocumentRuntime runtime;
  const bool hasRuntime = documentOwnerGetRuntime(uri, runtime);
  bool usedLastGood = false;
  std::shared_ptr<const InteractiveSnapshot> current =
      getOrBuildInteractiveSnapshot(uri, doc, ctx, &usedLastGood);
  std::shared_ptr<const InteractiveSnapshot> lastGood;
  std::shared_ptr<const DeferredDocSnapshot> deferred;
  if (hasRuntime)
    collectEligibleSnapshots(runtime, lastGood, deferred);
  if (usedLastGood && current && lastGood &&
      current.get() == lastGood.get()) {
    lastGood.reset();
  }

  std::unordered_set<std::string> seen;
  if (current && current->semanticSnapshot &&
      appendCompletionItemsFromSnapshot(*current->semanticSnapshot, doc.text,
                                        cursorOffset, prefix, outItems, seen)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeCurrentDocHits++;
    });
  }
  if (lastGood && lastGood->semanticSnapshot &&
      appendCompletionItemsFromSnapshot(*lastGood->semanticSnapshot, doc.text,
                                        cursorOffset, prefix, outItems, seen)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeLastGoodHits++;
    });
  }
  if (deferred && deferred->semanticSnapshot &&
      appendCompletionItemsFromSnapshot(*deferred->semanticSnapshot, doc.text,
                                        cursorOffset, prefix, outItems, seen)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeDeferredDocHits++;
    });
  }

  if (!prefix.empty()) {
    std::vector<IndexedDefinition> defs;
    if (workspaceSummaryRuntimeQuerySymbols(prefix, defs, 64)) {
      bool addedWorkspace = false;
      for (const auto &def : defs) {
        const size_t before = outItems.size();
        appendInteractiveCompletionCandidate(
            outItems, seen, def.name, completionKindForWorkspaceDefinition(def),
            def.type, prefix);
        addedWorkspace = addedWorkspace || outItems.size() > before;
      }
      if (addedWorkspace) {
        recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
          state.mergeWorkspaceSummaryHits++;
        });
      }
    }
  }
}

bool interactiveResolveDefinitionLocation(const std::string &uri,
                                          const Document &doc,
                                          const std::string &symbol,
                                          size_t cursorOffset,
                                          const ServerRequestContext &ctx,
                                          DefinitionLocation &outLocation) {
  outLocation = DefinitionLocation{};
  DocumentRuntime runtime;
  const bool hasRuntime = documentOwnerGetRuntime(uri, runtime);
  bool usedLastGood = false;
  std::shared_ptr<const InteractiveSnapshot> current =
      getOrBuildInteractiveSnapshot(uri, doc, ctx, &usedLastGood);
  std::shared_ptr<const InteractiveSnapshot> lastGood;
  std::shared_ptr<const DeferredDocSnapshot> deferred;
  if (hasRuntime)
    collectEligibleSnapshots(runtime, lastGood, deferred);
  if (usedLastGood && current && lastGood &&
      current.get() == lastGood.get()) {
    lastGood.reset();
  }

  if (current && current->semanticSnapshot &&
      tryResolveDefinitionFromSnapshot(*current->semanticSnapshot, uri, doc.text,
                                       symbol, cursorOffset, outLocation)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeCurrentDocHits++;
    });
    return true;
  }
  if (lastGood && lastGood->semanticSnapshot &&
      tryResolveDefinitionFromSnapshot(*lastGood->semanticSnapshot, uri,
                                       doc.text, symbol, cursorOffset,
                                       outLocation)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeLastGoodHits++;
    });
    return true;
  }
  if (deferred && deferred->semanticSnapshot &&
      tryResolveDefinitionFromSnapshot(*deferred->semanticSnapshot, uri,
                                       doc.text, symbol, cursorOffset,
                                       outLocation)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeDeferredDocHits++;
    });
    return true;
  }
  recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
    state.mergeMisses++;
  });
  return false;
}

TypeEvalResult interactiveResolveHoverTypeAtDeclaration(
    const std::string &uri, const Document &doc, const std::string &symbol,
    size_t cursorOffset, const ServerRequestContext &ctx, bool &isParamOut) {
  TypeEvalResult result;
  isParamOut = false;

  DocumentRuntime runtime;
  const bool hasRuntime = documentOwnerGetRuntime(uri, runtime);
  bool usedLastGood = false;
  std::shared_ptr<const InteractiveSnapshot> current =
      getOrBuildInteractiveSnapshot(uri, doc, ctx, &usedLastGood);
  std::shared_ptr<const InteractiveSnapshot> lastGood;
  std::shared_ptr<const DeferredDocSnapshot> deferred;
  if (hasRuntime)
    collectEligibleSnapshots(runtime, lastGood, deferred);
  if (usedLastGood && current && lastGood &&
      current.get() == lastGood.get()) {
    lastGood.reset();
  }

  std::string typeName;
  if (current && current->semanticSnapshot &&
      lookupTypeAtOffsetInSnapshot(*current->semanticSnapshot, doc.text, symbol,
                                   cursorOffset, typeName, isParamOut)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeCurrentDocHits++;
    });
    result.type = typeName;
    result.confidence = TypeEvalConfidence::L2;
    return result;
  }

  bool lastGoodIsParam = false;
  if (lastGood && lastGood->semanticSnapshot &&
      lookupTypeAtOffsetInSnapshot(*lastGood->semanticSnapshot, doc.text, symbol,
                                   cursorOffset, typeName, lastGoodIsParam)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeLastGoodHits++;
    });
    isParamOut = lastGoodIsParam;
    result.type = typeName;
    result.confidence = TypeEvalConfidence::L2;
    result.reasonCode = "last_good_snapshot";
    return result;
  }

  bool deferredIsParam = false;
  if (deferred && deferred->semanticSnapshot &&
      lookupTypeAtOffsetInSnapshot(*deferred->semanticSnapshot, doc.text, symbol,
                                   cursorOffset, typeName, deferredIsParam)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeDeferredDocHits++;
    });
    isParamOut = deferredIsParam;
    result.type = typeName;
    result.confidence = TypeEvalConfidence::L2;
    result.reasonCode = "deferred_doc_snapshot";
    return result;
  }

  workspaceSummaryRuntimeGetSymbolType(symbol, typeName);
  if (!typeName.empty()) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeWorkspaceSummaryHits++;
    });
    result.type = typeName;
    result.confidence = TypeEvalConfidence::L1;
    return result;
  }

  recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
    state.mergeMisses++;
  });
  result.confidence = TypeEvalConfidence::L3;
  result.reasonCode = "unknown_symbol";
  return result;
}

MemberAccessBaseTypeResult interactiveResolveMemberAccessBaseType(
    const std::string &uri, const Document &doc, const std::string &base,
    size_t cursorOffset, const ServerRequestContext &ctx,
    const MemberAccessBaseTypeOptions &options) {
  MemberAccessBaseTypeResult result;
  bool isParam = false;
  TypeEvalResult typeEval = interactiveResolveHoverTypeAtDeclaration(
      uri, doc, base, cursorOffset, ctx, isParam);
  result.typeName = typeEval.type;
  result.resolved = !result.typeName.empty();
  if (result.resolved)
    return result;

  if (options.includeWorkspaceIndexFallback) {
    workspaceSummaryRuntimeGetSymbolType(base, result.typeName);
    result.resolved = !result.typeName.empty();
    if (result.resolved) {
      recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
        state.mergeWorkspaceSummaryHits++;
      });
      return result;
    }
  }
  if (!result.resolved) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeMisses++;
    });
  }
  return result;
}

bool interactiveResolveFunctionSignature(
    const std::string &uri, const Document &doc, const std::string &name,
    int lineIndex, int nameCharacter, const ServerRequestContext &ctx,
    std::string &labelOut, std::vector<std::string> &parametersOut) {
  labelOut.clear();
  parametersOut.clear();

  DocumentRuntime runtime;
  const bool hasRuntime = documentOwnerGetRuntime(uri, runtime);
  bool usedLastGood = false;
  std::shared_ptr<const InteractiveSnapshot> current =
      getOrBuildInteractiveSnapshot(uri, doc, ctx, &usedLastGood);
  std::shared_ptr<const InteractiveSnapshot> lastGood;
  std::shared_ptr<const DeferredDocSnapshot> deferred;
  if (hasRuntime)
    collectEligibleSnapshots(runtime, lastGood, deferred);
  if (usedLastGood && current && lastGood &&
      current.get() == lastGood.get()) {
    lastGood.reset();
  }

  auto trySnapshot = [&](const SemanticSnapshot &snapshot) {
    const auto *function =
        findBestFunctionForLocation(snapshot, name, lineIndex, nameCharacter);
    if (!function)
      return false;
    labelOut = function->label;
    parametersOut = function->parameters;
    return !labelOut.empty();
  };

  if (current && current->semanticSnapshot &&
      trySnapshot(*current->semanticSnapshot)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeCurrentDocHits++;
    });
    return true;
  }
  if (lastGood && lastGood->semanticSnapshot &&
      trySnapshot(*lastGood->semanticSnapshot)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeLastGoodHits++;
    });
    return true;
  }
  if (deferred && deferred->semanticSnapshot &&
      trySnapshot(*deferred->semanticSnapshot)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeDeferredDocHits++;
    });
    return true;
  }
  if (resolveFunctionSignatureFromWorkspaceSummary(name, ctx, labelOut,
                                                   parametersOut)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeWorkspaceSummaryHits++;
    });
    return true;
  }
  recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
    state.mergeMisses++;
  });
  return false;
}

bool interactiveResolveFunctionOverloads(
    const std::string &uri, const Document &doc, const std::string &name,
    const ServerRequestContext &ctx,
    std::vector<SemanticSnapshotFunctionOverloadInfo> &overloadsOut) {
  overloadsOut.clear();

  DocumentRuntime runtime;
  const bool hasRuntime = documentOwnerGetRuntime(uri, runtime);
  bool usedLastGood = false;
  std::shared_ptr<const InteractiveSnapshot> current =
      getOrBuildInteractiveSnapshot(uri, doc, ctx, &usedLastGood);
  std::shared_ptr<const InteractiveSnapshot> lastGood;
  std::shared_ptr<const DeferredDocSnapshot> deferred;
  if (hasRuntime)
    collectEligibleSnapshots(runtime, lastGood, deferred);
  if (usedLastGood && current && lastGood &&
      current.get() == lastGood.get()) {
    lastGood.reset();
  }

  if (current && current->semanticSnapshot &&
      appendOverloadsFromSnapshot(*current->semanticSnapshot, name,
                                  overloadsOut)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeCurrentDocHits++;
    });
    return true;
  }
  if (lastGood && lastGood->semanticSnapshot &&
      appendOverloadsFromSnapshot(*lastGood->semanticSnapshot, name,
                                  overloadsOut)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeLastGoodHits++;
    });
    return true;
  }
  if (deferred && deferred->semanticSnapshot &&
      appendOverloadsFromSnapshot(*deferred->semanticSnapshot, name,
                                  overloadsOut)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeDeferredDocHits++;
    });
    return true;
  }
  if (appendWorkspaceSummaryOverloads(name, ctx, overloadsOut)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeWorkspaceSummaryHits++;
    });
    return true;
  }
  recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
    state.mergeMisses++;
  });
  return false;
}

bool interactiveCollectMemberCompletionQuery(const std::string &uri,
                                             const std::string &ownerType,
                                             ServerRequestContext &ctx,
                                             MemberCompletionQuery &out) {
  out = MemberCompletionQuery{};
  out.ownerType = ownerType;
  if (ownerType.empty())
    return false;

  listHlslBuiltinMethodsForType(ownerType, out.methods);

  const Document *doc = ctx.findDocument(uri);
  if (!doc)
    return !out.methods.empty();

  DocumentRuntime runtime;
  const bool hasRuntime = documentOwnerGetRuntime(uri, runtime);
  bool usedLastGood = false;
  std::shared_ptr<const InteractiveSnapshot> current =
      getOrBuildInteractiveSnapshot(uri, *doc, ctx, &usedLastGood);
  std::shared_ptr<const InteractiveSnapshot> lastGood;
  std::shared_ptr<const DeferredDocSnapshot> deferred;
  if (hasRuntime)
    collectEligibleSnapshots(runtime, lastGood, deferred);
  if (usedLastGood && current && lastGood &&
      current.get() == lastGood.get()) {
    lastGood.reset();
  }

  if (current && current->semanticSnapshot &&
      lookupStructFieldInfosFromSnapshot(*current->semanticSnapshot, ownerType,
                                         out.fields)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeCurrentDocHits++;
    });
    return true;
  }
  if (lastGood && lastGood->semanticSnapshot &&
      lookupStructFieldInfosFromSnapshot(*lastGood->semanticSnapshot, ownerType,
                                         out.fields)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeLastGoodHits++;
    });
    return true;
  }
  if (deferred && deferred->semanticSnapshot &&
      lookupStructFieldInfosFromSnapshot(*deferred->semanticSnapshot, ownerType,
                                         out.fields)) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeDeferredDocHits++;
    });
    return true;
  }
  std::vector<std::string> fieldNames;
  if (workspaceSummaryRuntimeGetStructFields(ownerType, fieldNames) &&
      !fieldNames.empty()) {
    out.fields.clear();
    out.fields.reserve(fieldNames.size());
    for (const auto &fieldName : fieldNames) {
      MemberCompletionField item;
      item.name = fieldName;
      workspaceSummaryRuntimeGetStructMemberType(ownerType, fieldName,
                                                 item.type);
      out.fields.push_back(std::move(item));
    }
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeWorkspaceSummaryHits++;
    });
    return true;
  }
  if (out.methods.empty()) {
    recordInteractiveMetric([](InteractiveRuntimeMetricState &state) {
      state.mergeMisses++;
    });
  }
  return !out.methods.empty();
}

InteractiveRuntimeMetricsSnapshot takeInteractiveRuntimeMetricsSnapshot() {
  std::lock_guard<std::mutex> lock(gInteractiveMetricsMutex);
  InteractiveRuntimeMetricsSnapshot snapshot;
  snapshot.snapshotRequests = gInteractiveMetrics.snapshotRequests;
  snapshot.analysisKeyHits = gInteractiveMetrics.analysisKeyHits;
  snapshot.snapshotBuildAttempts = gInteractiveMetrics.snapshotBuildAttempts;
  snapshot.snapshotBuildSuccess = gInteractiveMetrics.snapshotBuildSuccess;
  snapshot.snapshotBuildFailed = gInteractiveMetrics.snapshotBuildFailed;
  snapshot.lastGoodServed = gInteractiveMetrics.lastGoodServed;
  snapshot.incrementalPromoted = gInteractiveMetrics.incrementalPromoted;
  snapshot.noSnapshotAvailable = gInteractiveMetrics.noSnapshotAvailable;
  snapshot.mergeCurrentDocHits = gInteractiveMetrics.mergeCurrentDocHits;
  snapshot.mergeLastGoodHits = gInteractiveMetrics.mergeLastGoodHits;
  snapshot.mergeDeferredDocHits = gInteractiveMetrics.mergeDeferredDocHits;
  snapshot.mergeWorkspaceSummaryHits =
      gInteractiveMetrics.mergeWorkspaceSummaryHits;
  snapshot.mergeMisses = gInteractiveMetrics.mergeMisses;
  snapshot.snapshotWaitSamples = gInteractiveMetrics.snapshotWaitSamples;
  snapshot.snapshotWaitTotalMs = gInteractiveMetrics.snapshotWaitTotalMs;
  snapshot.snapshotWaitMaxMs = gInteractiveMetrics.snapshotWaitMaxMs;
  gInteractiveMetrics = InteractiveRuntimeMetricState{};
  return snapshot;
}
