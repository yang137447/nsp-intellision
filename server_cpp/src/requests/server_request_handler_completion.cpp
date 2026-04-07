#include "server_request_handler_completion.hpp"

#include "callsite_parser.hpp"
#include "completion_rendering.hpp"
#include "declaration_query.hpp"
#include "hlsl_builtin_docs.hpp"
#include "interactive_semantic_runtime.hpp"
#include "language_registry.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "member_query.hpp"
#include "nsf_lexer.hpp"
#include "server_parse.hpp"
#include "text_utils.hpp"
#include "type_model.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

struct CompletionMetricState {
  uint64_t interactiveCollectSamples = 0;
  double interactiveCollectTotalMs = 0.0;
  double interactiveCollectMaxMs = 0.0;
  uint64_t memberAccessDetectedCount = 0;
  uint64_t memberTypeResolvedCount = 0;
  uint64_t memberItemsReturnedCount = 0;
  uint64_t memberGenericFallbackCount = 0;
  uint64_t memberBaseResolveSamples = 0;
  double memberBaseResolveTotalMs = 0.0;
  double memberBaseResolveMaxMs = 0.0;
  uint64_t memberQuerySamples = 0;
  double memberQueryTotalMs = 0.0;
  double memberQueryMaxMs = 0.0;
  uint64_t workspaceSummaryQuerySamples = 0;
  double workspaceSummaryQueryTotalMs = 0.0;
  double workspaceSummaryQueryMaxMs = 0.0;
  uint64_t itemAssemblySamples = 0;
  double itemAssemblyTotalMs = 0.0;
  double itemAssemblyMaxMs = 0.0;
  uint64_t responseWriteSamples = 0;
  double responseWriteTotalMs = 0.0;
  double responseWriteMaxMs = 0.0;
};

std::mutex gCompletionMetricsMutex;
CompletionMetricState gCompletionMetrics;
std::mutex gCompletionDebugMutex;
CompletionDebugSnapshot gLastCompletionDebug;

void recordCompletionDuration(uint64_t &samples, double &totalMs,
                              double &maxMs, double durationMs) {
  samples++;
  totalMs += durationMs;
  maxMs = std::max(maxMs, durationMs);
}

} // namespace

void recordCompletionInteractiveCollect(double durationMs) {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  recordCompletionDuration(gCompletionMetrics.interactiveCollectSamples,
                           gCompletionMetrics.interactiveCollectTotalMs,
                           gCompletionMetrics.interactiveCollectMaxMs,
                           durationMs);
}

void recordCompletionWorkspaceSummaryQuery(double durationMs) {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  recordCompletionDuration(gCompletionMetrics.workspaceSummaryQuerySamples,
                           gCompletionMetrics.workspaceSummaryQueryTotalMs,
                           gCompletionMetrics.workspaceSummaryQueryMaxMs,
                           durationMs);
}

void recordCompletionItemAssembly(double durationMs) {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  recordCompletionDuration(gCompletionMetrics.itemAssemblySamples,
                           gCompletionMetrics.itemAssemblyTotalMs,
                           gCompletionMetrics.itemAssemblyMaxMs, durationMs);
}

void recordCompletionResponseWrite(double durationMs) {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  recordCompletionDuration(gCompletionMetrics.responseWriteSamples,
                           gCompletionMetrics.responseWriteTotalMs,
                           gCompletionMetrics.responseWriteMaxMs, durationMs);
}

void recordCompletionMemberAccessDetected() {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  gCompletionMetrics.memberAccessDetectedCount++;
}

void recordCompletionMemberTypeResolved() {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  gCompletionMetrics.memberTypeResolvedCount++;
}

void recordCompletionMemberItemsReturned() {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  gCompletionMetrics.memberItemsReturnedCount++;
}

void recordCompletionMemberGenericFallback() {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  gCompletionMetrics.memberGenericFallbackCount++;
}

void recordCompletionMemberBaseResolve(double durationMs) {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  recordCompletionDuration(gCompletionMetrics.memberBaseResolveSamples,
                           gCompletionMetrics.memberBaseResolveTotalMs,
                           gCompletionMetrics.memberBaseResolveMaxMs,
                           durationMs);
}

void recordCompletionMemberQuery(double durationMs) {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  recordCompletionDuration(gCompletionMetrics.memberQuerySamples,
                           gCompletionMetrics.memberQueryTotalMs,
                           gCompletionMetrics.memberQueryMaxMs, durationMs);
}

void updateLastCompletionDebugSnapshot(const CompletionDebugSnapshot &snapshot) {
  std::lock_guard<std::mutex> lock(gCompletionDebugMutex);
  gLastCompletionDebug = snapshot;
}

CompletionDebugSnapshot getLastCompletionDebugSnapshot() {
  std::lock_guard<std::mutex> lock(gCompletionDebugMutex);
  return gLastCompletionDebug;
}

CompletionMetricsSnapshot takeCompletionMetricsSnapshot() {
  std::lock_guard<std::mutex> lock(gCompletionMetricsMutex);
  CompletionMetricsSnapshot snapshot;
  snapshot.interactiveCollectSamples =
      gCompletionMetrics.interactiveCollectSamples;
  snapshot.interactiveCollectTotalMs =
      gCompletionMetrics.interactiveCollectTotalMs;
  snapshot.interactiveCollectMaxMs = gCompletionMetrics.interactiveCollectMaxMs;
  snapshot.memberAccessDetectedCount =
      gCompletionMetrics.memberAccessDetectedCount;
  snapshot.memberTypeResolvedCount =
      gCompletionMetrics.memberTypeResolvedCount;
  snapshot.memberItemsReturnedCount =
      gCompletionMetrics.memberItemsReturnedCount;
  snapshot.memberGenericFallbackCount =
      gCompletionMetrics.memberGenericFallbackCount;
  snapshot.memberBaseResolveSamples =
      gCompletionMetrics.memberBaseResolveSamples;
  snapshot.memberBaseResolveTotalMs =
      gCompletionMetrics.memberBaseResolveTotalMs;
  snapshot.memberBaseResolveMaxMs = gCompletionMetrics.memberBaseResolveMaxMs;
  snapshot.memberQuerySamples = gCompletionMetrics.memberQuerySamples;
  snapshot.memberQueryTotalMs = gCompletionMetrics.memberQueryTotalMs;
  snapshot.memberQueryMaxMs = gCompletionMetrics.memberQueryMaxMs;
  snapshot.workspaceSummaryQuerySamples =
      gCompletionMetrics.workspaceSummaryQuerySamples;
  snapshot.workspaceSummaryQueryTotalMs =
      gCompletionMetrics.workspaceSummaryQueryTotalMs;
  snapshot.workspaceSummaryQueryMaxMs =
      gCompletionMetrics.workspaceSummaryQueryMaxMs;
  snapshot.itemAssemblySamples = gCompletionMetrics.itemAssemblySamples;
  snapshot.itemAssemblyTotalMs = gCompletionMetrics.itemAssemblyTotalMs;
  snapshot.itemAssemblyMaxMs = gCompletionMetrics.itemAssemblyMaxMs;
  snapshot.responseWriteSamples = gCompletionMetrics.responseWriteSamples;
  snapshot.responseWriteTotalMs = gCompletionMetrics.responseWriteTotalMs;
  snapshot.responseWriteMaxMs = gCompletionMetrics.responseWriteMaxMs;
  gCompletionMetrics = CompletionMetricState{};
  return snapshot;
}

static const std::vector<std::string> &getHlslScalarVectorMatrixTypeNames() {
  static const std::vector<std::string> names = []() {
    const std::vector<std::string> scalarTypes = {
        "void",       "bool",       "int",       "uint",      "dword",
        "half",       "float",      "double",    "min16float","min10float",
        "min16int",   "min12int",   "min16uint",
    };
    const std::vector<std::string> numericBases = {
        "bool",       "int",        "uint",      "half",      "float",
        "double",     "min16float", "min10float","min16int",  "min12int",
        "min16uint",
    };

    std::vector<std::string> generated = scalarTypes;
    generated.reserve(scalarTypes.size() + numericBases.size() * 12);
    for (const auto &base : numericBases) {
      for (int dim = 2; dim <= 4; ++dim)
        generated.push_back(base + std::to_string(dim));
      for (int rows = 2; rows <= 4; ++rows) {
        for (int cols = 2; cols <= 4; ++cols)
          generated.push_back(base + std::to_string(rows) + "x" +
                              std::to_string(cols));
      }
    }
    return generated;
  }();
  return names;
}

static std::string extractCompletionPrefix(const std::string &lineText,
                                           int cursorCharacter) {
  if (cursorCharacter < 0)
    return std::string();
  size_t cursor =
      static_cast<size_t>(utf16ToByteOffsetInLine(lineText, cursorCharacter));
  if (cursor > lineText.size())
    cursor = lineText.size();
  size_t start = cursor;
  while (start > 0 && isIdentifierChar(lineText[start - 1]))
    start--;
  return lineText.substr(start, cursor - start);
}

static bool matchesStaticCompletionPrefix(const std::string &label,
                                          const std::string &prefix,
                                          const std::string &filterText) {
  if (prefix.empty())
    return true;
  const std::string &candidate = filterText.empty() ? label : filterText;
  if (candidate.size() < prefix.size())
    return false;
  for (size_t index = 0; index < prefix.size(); ++index) {
    const unsigned char lhs = static_cast<unsigned char>(candidate[index]);
    const unsigned char rhs = static_cast<unsigned char>(prefix[index]);
    if (std::tolower(lhs) != std::tolower(rhs))
      return false;
  }
  return true;
}

bool request_completion_handlers::handleCompletionRequest(
    const std::string &method, const Json &id, const Json *params,
    ServerRequestContext &ctx, const std::vector<std::string> &keywords,
    const std::vector<std::string> &directives) {
  if (method != "textDocument/completion" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  std::string uri;
  int line = -1;
  int character = -1;
  const Document *doc = nullptr;
  if (textDocument && position) {
    const Json *uriValue = getObjectValue(*textDocument, "uri");
    const Json *lineValue = getObjectValue(*position, "line");
    const Json *charValue = getObjectValue(*position, "character");
    if (uriValue && lineValue && charValue) {
      uri = getStringValue(*uriValue);
      line = static_cast<int>(getNumberValue(*lineValue));
      character = static_cast<int>(getNumberValue(*charValue));
      doc = ctx.findDocument(uri);
    }
  }

  static const std::vector<std::string> attributeKeywords = {
      "unroll", "loop",    "fastopt", "allow_uav_condition",
      "branch", "flatten", "call"};
  std::vector<InteractiveCompletionItem> interactiveItems;
  std::string completionPrefix;
  CompletionDebugSnapshot completionDebug;
  completionDebug.line = line;
  completionDebug.character = character;
  auto writeCompletionResponse = [&](const Json &response) {
    const auto responseWriteStartedAt = std::chrono::steady_clock::now();
    writeResponse(id, response);
    recordCompletionResponseWrite(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - responseWriteStartedAt)
            .count());
  };

  auto isAttributeCompletionContext = [&](const std::string &lineText,
                                          int cursorChar) {
    if (cursorChar < 0)
      return false;
    size_t cursor =
        static_cast<size_t>(utf16ToByteOffsetInLine(lineText, cursorChar));
    if (cursor > lineText.size())
      cursor = lineText.size();
    size_t open = lineText.rfind('[', cursor == 0 ? 0 : cursor - 1);
    if (open == std::string::npos)
      return false;
    size_t close = lineText.find(']', open);
    if (close != std::string::npos && close < cursor)
      return false;
    size_t firstNonSpace = 0;
    while (firstNonSpace < lineText.size() &&
           std::isspace(static_cast<unsigned char>(lineText[firstNonSpace]))) {
      firstNonSpace++;
    }
    if (firstNonSpace >= lineText.size())
      return false;
    if (firstNonSpace == open)
      return true;
    size_t prev = open;
    while (prev > 0) {
      prev--;
      if (!std::isspace(static_cast<unsigned char>(lineText[prev])))
        break;
    }
    char prevCh = prev < lineText.size() ? lineText[prev] : '\0';
    return prevCh == '{' || prevCh == ';';
  };

  if (doc && line >= 0 && character >= 0) {
    std::string lineText = getLineAt(doc->text, line);
    completionDebug.lineText = lineText;
    completionPrefix = extractCompletionPrefix(lineText, character);
    if (isAttributeCompletionContext(lineText, character)) {
      Json items = makeArray();
      for (const auto &label : attributeKeywords) {
        if (!matchesStaticCompletionPrefix(label, completionPrefix,
                                           std::string())) {
          continue;
        }
        appendCompletionItem(items, label, 14);
      }
      writeCompletionResponse(items);
      return true;
    }
    size_t cursorOffset = positionToOffsetUtf16(doc->text, line, character);
    std::string base;
    std::string member;
    bool memberAccessDetected =
        extractMemberAccessAtOffset(doc->text, cursorOffset, base, member);
    if (!memberAccessDetected) {
      const std::string lineText = getLineAt(doc->text, line);
      for (const int candidateChar : {character, character + 1,
                                      std::max(0, character - 1)}) {
        if (extractMemberAccessBase(lineText, candidateChar, base)) {
          memberAccessDetected = true;
          member.clear();
          break;
        }
      }
    }
    if (memberAccessDetected) {
      recordCompletionMemberAccessDetected();
      completionDebug.memberAccessDetected = true;
      completionDebug.base = base;
      completionDebug.member = member;
      completionDebug.path = "member_access_detected";
      MemberAccessBaseTypeOptions baseOptions;
      baseOptions.includeWorkspaceIndexFallback = true;
      const auto memberBaseResolveStartedAt = std::chrono::steady_clock::now();
      MemberAccessBaseTypeResult baseResolution =
          resolveMemberAccessBaseType(uri, *doc, base, cursorOffset, ctx,
                                      baseOptions);
      recordCompletionMemberBaseResolve(
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - memberBaseResolveStartedAt)
              .count());
      if (!baseResolution.resolved) {
        DeclCandidate decl;
        if (findBestCurrentDocDeclarationUpTo(doc->text, base, cursorOffset,
                                              ctx.preprocessorDefines, decl) &&
            decl.found &&
            findTypeOfIdentifierInDeclarationLineShared(decl.lineText, base,
                                                        baseResolution.typeName) &&
            !baseResolution.typeName.empty()) {
          baseResolution.resolved = true;
          completionDebug.path = "member_decl_fallback";
        }
      }
      if (baseResolution.resolved) {
        recordCompletionMemberTypeResolved();
        completionDebug.memberTypeResolved = true;
        completionDebug.resolvedType = baseResolution.typeName;
        MemberCompletionQuery query;
        const auto memberQueryStartedAt = std::chrono::steady_clock::now();
        if (collectMemberCompletionQuery(uri, baseResolution.typeName, ctx,
                                         query)) {
          recordCompletionMemberQuery(
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - memberQueryStartedAt)
                  .count());
          completionDebug.fieldCount = query.fields.size();
          completionDebug.methodCount = query.methods.size();
          Json items = buildMemberCompletionItems(query);
          if (!items.a.empty()) {
            recordCompletionMemberItemsReturned();
            completionDebug.memberItemsReturned = true;
            completionDebug.path = "member_items_returned";
            updateLastCompletionDebugSnapshot(completionDebug);
            writeCompletionResponse(items);
            return true;
          }
          completionDebug.path = "member_query_empty_items";
        } else {
          recordCompletionMemberQuery(
              std::chrono::duration<double, std::milli>(
                  std::chrono::steady_clock::now() - memberQueryStartedAt)
                  .count());
        }
      } else {
        completionDebug.path = "member_type_unresolved";
      }
      recordCompletionMemberGenericFallback();
      updateLastCompletionDebugSnapshot(completionDebug);
    }

    const auto interactiveCollectStartedAt = std::chrono::steady_clock::now();
    interactiveCollectCompletionItems(uri, *doc, cursorOffset,
                                      completionPrefix, ctx, interactiveItems);
    recordCompletionInteractiveCollect(
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - interactiveCollectStartedAt)
            .count());
  }

  const auto &directiveItems = getHlslDirectiveCompletionItems();
  const auto &keywordNames = getHlslKeywordNames();
  const auto &scalarVectorMatrixTypeNames =
      getHlslScalarVectorMatrixTypeNames();
  const auto &objectTypeNames = getTypeModelObjectTypeNames();
  const auto &systemSemanticNames = getHlslSystemSemanticNames();
  const auto &builtinNames = getHlslBuiltinNames();
  bool appendAttributeKeywords = false;
  if (doc && line >= 0 && character >= 0) {
    std::string lineText = getLineAt(doc->text, line);
    appendAttributeKeywords = isAttributeCompletionContext(lineText, character);
  }

  const size_t estimatedItemCount =
      interactiveItems.size() + directiveItems.size() + keywordNames.size() +
      scalarVectorMatrixTypeNames.size() + objectTypeNames.size() +
      systemSemanticNames.size() + builtinNames.size() +
      (appendAttributeKeywords ? attributeKeywords.size() : 0);

  const auto itemAssemblyStartedAt = std::chrono::steady_clock::now();
  Json items = makeArray();
  items.a.reserve(estimatedItemCount);
  std::unordered_set<std::string> seen;
  seen.reserve(std::max<size_t>(estimatedItemCount * 2, 32));
  auto appendUniqueItems = [&](const std::vector<std::string> &labels,
                               int kind) {
    for (const auto &label : labels) {
      if (!label.empty() && label[0] == '#') {
        const std::string filterText = label.substr(1);
        if (!matchesStaticCompletionPrefix(label, completionPrefix,
                                           filterText)) {
          continue;
        }
        appendUniqueCompletionItem(items, seen, label, kind, std::string(),
                                   filterText);
        continue;
      }
      if (!matchesStaticCompletionPrefix(label, completionPrefix,
                                         std::string())) {
        continue;
      }
      appendUniqueCompletionItem(items, seen, label, kind);
    }
  };
  for (const auto &item : interactiveItems) {
    appendUniqueCompletionItem(items, seen, item.label, item.kind, item.detail);
  }
  appendUniqueItems(directiveItems, 14);
  appendUniqueItems(keywordNames, 14);
  appendUniqueItems(scalarVectorMatrixTypeNames, 7);
  appendUniqueItems(objectTypeNames, 7);
  appendUniqueItems(systemSemanticNames, 13);
  appendUniqueItems(builtinNames, 3);
  if (appendAttributeKeywords)
    appendUniqueItems(attributeKeywords, 14);
  recordCompletionItemAssembly(
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - itemAssemblyStartedAt)
          .count());
  if (!completionDebug.memberItemsReturned) {
    if (completionDebug.path.empty()) {
      completionDebug.path = "generic_completion_only";
    } else {
      completionDebug.path = "generic_completion_fallback";
    }
  }
  updateLastCompletionDebugSnapshot(completionDebug);
  writeCompletionResponse(items);
  return true;
}

