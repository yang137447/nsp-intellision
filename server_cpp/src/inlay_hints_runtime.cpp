#include "inlay_hints_runtime.hpp"

#include "call_query.hpp"
#include "callsite_parser.hpp"
#include "deferred_doc_runtime.hpp"
#include "document_owner.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_markdown.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "server_request_handlers.hpp"
#include "text_utils.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct InlaySlowResolveTask {
  std::string uri;
  uint64_t epoch = 0;
  int documentVersion = 0;
  std::vector<std::string> cacheKeys;
  ServerRequestContext context;
};

std::mutex gInlayParameterCacheMutex;
std::unordered_map<std::string, std::vector<std::string>>
    gInlayParameterCache;
std::mutex gInlaySlowMutex;
std::condition_variable gInlaySlowCv;
std::deque<InlaySlowResolveTask> gInlaySlowQueue;
std::unordered_map<std::string, uint64_t> gInlaySlowLatestEpochByUri;
std::unordered_map<std::string, int> gInlaySlowLatestVersionByUri;
std::once_flag gInlaySlowWorkerInitFlag;

bool tryGetInlayParameterCache(const std::string &cacheKey,
                               std::vector<std::string> &paramsOut) {
  std::lock_guard<std::mutex> lock(gInlayParameterCacheMutex);
  auto it = gInlayParameterCache.find(cacheKey);
  if (it == gInlayParameterCache.end())
    return false;
  paramsOut = it->second;
  return true;
}

void updateInlayParameterCache(const std::string &cacheKey,
                               const std::vector<std::string> &params) {
  std::lock_guard<std::mutex> lock(gInlayParameterCacheMutex);
  gInlayParameterCache[cacheKey] = params;
}

std::string makeInlayParameterCacheKey(const std::string &functionName,
                                       bool isMemberCall) {
  return (isMemberCall ? "method:" : "func:") + functionName;
}

bool parseInlayParameterCacheKey(const std::string &cacheKey,
                                 bool &isMemberCallOut,
                                 std::string &nameOut) {
  isMemberCallOut = false;
  nameOut.clear();
  if (cacheKey.rfind("method:", 0) == 0) {
    isMemberCallOut = true;
    nameOut = cacheKey.substr(7);
    return !nameOut.empty();
  }
  if (cacheKey.rfind("func:", 0) == 0) {
    nameOut = cacheKey.substr(5);
    return !nameOut.empty();
  }
  return false;
}

bool resolveBuiltinMethodParametersForInlay(const std::string &methodName,
                                            std::vector<std::string> &outParams) {
  outParams.clear();
  if (methodName.empty())
    return false;
  std::vector<HlslBuiltinSignature> signatures;
  static const std::vector<std::string> kBaseTypeCandidates = {
      "Texture2D",        "RWTexture2D",       "Buffer",
      "RWBuffer",         "StructuredBuffer",  "RWStructuredBuffer",
      "ByteAddressBuffer","RWByteAddressBuffer","SamplerState",
      "SamplerComparisonState",
  };
  for (const auto &baseType : kBaseTypeCandidates) {
    if (lookupHlslBuiltinMethodSignatures(methodName, baseType, signatures) &&
        !signatures.empty()) {
      outParams = signatures.front().parameters;
      return !outParams.empty();
    }
  }
  return false;
}

uint64_t updateInlaySlowEpoch(const std::string &uri, int documentVersion) {
  std::lock_guard<std::mutex> lock(gInlaySlowMutex);
  gInlaySlowLatestVersionByUri[uri] = documentVersion;
  return ++gInlaySlowLatestEpochByUri[uri];
}

bool isInlaySlowEpochLatest(const std::string &uri, uint64_t epoch) {
  std::lock_guard<std::mutex> lock(gInlaySlowMutex);
  auto it = gInlaySlowLatestEpochByUri.find(uri);
  return it != gInlaySlowLatestEpochByUri.end() && it->second == epoch;
}

bool isInlaySlowDocumentVersionLatest(const std::string &uri,
                                      int documentVersion) {
  std::lock_guard<std::mutex> lock(gInlaySlowMutex);
  auto it = gInlaySlowLatestVersionByUri.find(uri);
  return it != gInlaySlowLatestVersionByUri.end() &&
         it->second == documentVersion;
}

void ensureInlaySlowWorkerStarted() {
  std::call_once(gInlaySlowWorkerInitFlag, []() {
    std::thread([]() {
      while (true) {
        InlaySlowResolveTask task;
        {
          std::unique_lock<std::mutex> lock(gInlaySlowMutex);
          gInlaySlowCv.wait(lock, []() { return !gInlaySlowQueue.empty(); });
          task = std::move(gInlaySlowQueue.front());
          gInlaySlowQueue.pop_front();
        }
        if (!isInlaySlowEpochLatest(task.uri, task.epoch) ||
            !isInlaySlowDocumentVersionLatest(task.uri, task.documentVersion)) {
          continue;
        }
        bool changed = false;
        for (const auto &cacheKey : task.cacheKeys) {
          if (!isInlaySlowEpochLatest(task.uri, task.epoch) ||
              !isInlaySlowDocumentVersionLatest(task.uri,
                                                task.documentVersion)) {
            break;
          }
          bool isMemberCall = false;
          std::string name;
          if (!parseInlayParameterCacheKey(cacheKey, isMemberCall, name))
            continue;
          std::vector<std::string> paramsOut;
          const bool resolved =
              isMemberCall
                  ? resolveBuiltinMethodParametersForInlay(name, paramsOut)
                  : resolveFunctionParameters(task.uri, name, task.context,
                                              paramsOut);
          if (!resolved || paramsOut.empty())
            continue;
          updateInlayParameterCache(cacheKey, paramsOut);
          changed = true;
        }
        if (changed && isInlaySlowEpochLatest(task.uri, task.epoch) &&
            isInlaySlowDocumentVersionLatest(task.uri, task.documentVersion)) {
          deferredDocRuntimeInvalidateInlayHints(task.uri);
          Json params = makeObject();
          params.o["uri"] = makeString(task.uri);
          writeNotification("nsf/inlayHintsChanged", params);
        }
      }
    }).detach();
  });
}

void scheduleInlaySlowResolve(const std::string &uri, uint64_t epoch,
                              int documentVersion,
                              std::vector<std::string> cacheKeys,
                              ServerRequestContext context) {
  if (cacheKeys.empty())
    return;
  ensureInlaySlowWorkerStarted();
  InlaySlowResolveTask task;
  task.uri = uri;
  task.epoch = epoch;
  task.documentVersion = documentVersion;
  task.cacheKeys = std::move(cacheKeys);
  task.context = std::move(context);
  {
    std::lock_guard<std::mutex> lock(gInlaySlowMutex);
    gInlaySlowQueue.push_back(std::move(task));
  }
  gInlaySlowCv.notify_one();
}

void collectLineStarts(const std::string &text,
                       std::vector<size_t> &lineStarts) {
  lineStarts.clear();
  lineStarts.reserve(128);
  lineStarts.push_back(0);
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '\n' && i + 1 <= text.size())
      lineStarts.push_back(i + 1);
  }
}

bool offsetToPosition(const std::string &text,
                      const std::vector<size_t> &lineStarts, size_t offset,
                      int &lineOut, int &characterOut) {
  if (offset > text.size() || lineStarts.empty())
    return false;
  auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
  if (it == lineStarts.begin())
    return false;
  size_t lineIndex = static_cast<size_t>((it - lineStarts.begin()) - 1);
  size_t lineStart = lineStarts[lineIndex];
  size_t lineEnd = text.find('\n', lineStart);
  if (lineEnd == std::string::npos)
    lineEnd = text.size();
  if (offset > lineEnd)
    offset = lineEnd;
  std::string lineText = text.substr(lineStart, lineEnd - lineStart);
  lineOut = static_cast<int>(lineIndex);
  characterOut =
      byteOffsetInLineToUtf16(lineText, static_cast<int>(offset - lineStart));
  return true;
}

bool isPositionWithinRange(int line, int character, int startLine,
                           int startChar, int endLine, int endChar) {
  if (line < startLine || line > endLine)
    return false;
  if (line == startLine && character < startChar)
    return false;
  if (line == endLine && character > endChar)
    return false;
  return true;
}

} // namespace

Json inlayHintsRuntimeBuildFullDocument(const std::string &uri,
                                        const Document &doc,
                                        ServerRequestContext &ctx) {
  std::vector<CallSiteArgument> arguments;
  collectCallArgumentsInRange(doc.text, 0, doc.text.size(), arguments);
  const uint64_t slowEpoch = updateInlaySlowEpoch(uri, doc.version);
  std::unordered_map<std::string, std::vector<std::string>> parameterCache;
  std::unordered_set<std::string> slowResolveSet;
  std::vector<size_t> lineStarts;
  collectLineStarts(doc.text, lineStarts);
  const int maxHints = 160;
  Json hints = makeArray();
  for (const auto &argument : arguments) {
    if (static_cast<int>(hints.a.size()) >= maxHints)
      break;
    const std::string cacheKey =
        makeInlayParameterCacheKey(argument.functionName, argument.isMemberCall);
    auto cacheIt = parameterCache.find(cacheKey);
    if (cacheIt == parameterCache.end()) {
      std::vector<std::string> paramsOut;
      if (argument.isMemberCall) {
        if (tryGetInlayParameterCache(cacheKey, paramsOut)) {
          cacheIt = parameterCache.emplace(cacheKey, std::move(paramsOut)).first;
        } else if (resolveBuiltinMethodParametersForInlay(argument.functionName,
                                                          paramsOut) &&
                   !paramsOut.empty()) {
          updateInlayParameterCache(cacheKey, paramsOut);
          cacheIt = parameterCache.emplace(cacheKey, std::move(paramsOut)).first;
        } else {
          cacheIt =
              parameterCache.emplace(cacheKey, std::vector<std::string>{}).first;
        }
      } else {
        const HlslBuiltinSignature *builtinSig =
            lookupHlslBuiltinSignature(argument.functionName);
        if (builtinSig) {
          paramsOut = builtinSig->parameters;
          cacheIt = parameterCache.emplace(cacheKey, std::move(paramsOut)).first;
        } else if (tryGetInlayParameterCache(cacheKey, paramsOut)) {
          cacheIt = parameterCache.emplace(cacheKey, std::move(paramsOut)).first;
        } else {
          if (resolveFunctionParameters(uri, argument.functionName, ctx,
                                        paramsOut) &&
              !paramsOut.empty()) {
            updateInlayParameterCache(cacheKey, paramsOut);
            cacheIt = parameterCache.emplace(cacheKey, std::move(paramsOut)).first;
          } else {
            cacheIt =
                parameterCache.emplace(cacheKey, std::vector<std::string>{}).first;
            slowResolveSet.insert(cacheKey);
          }
        }
      }
    }
    const auto &paramsOut = cacheIt->second;
    if (argument.argumentIndex < 0 ||
        argument.argumentIndex >= static_cast<int>(paramsOut.size())) {
      continue;
    }
    std::string parameterName = extractParameterName(
        paramsOut[static_cast<size_t>(argument.argumentIndex)]);
    if (parameterName.empty())
      continue;

    int line = 0;
    int character = 0;
    if (!offsetToPosition(doc.text, lineStarts, argument.argumentStartOffset,
                          line, character)) {
      continue;
    }

    Json position = makeObject();
    position.o["line"] = makeNumber(line);
    position.o["character"] = makeNumber(character);

    Json hint = makeObject();
    hint.o["position"] = position;
    hint.o["label"] = makeString(parameterName + ":");
    hint.o["kind"] = makeNumber(2);
    hint.o["paddingRight"] = makeBool(true);
    hints.a.push_back(std::move(hint));
  }
  if (ctx.inlayHintsEnabled && ctx.inlayHintsParameterNamesEnabled &&
      !slowResolveSet.empty()) {
    std::vector<std::string> cacheKeys(slowResolveSet.begin(),
                                       slowResolveSet.end());
    scheduleInlaySlowResolve(uri, slowEpoch, doc.version, std::move(cacheKeys),
                             ctx);
  }
  return hints;
}

Json inlayHintsRuntimeBuildOrGetDeferredFull(const std::string &uri,
                                             const Document &doc,
                                             ServerRequestContext &ctx) {
  auto deferred = getOrBuildDeferredDocSnapshot(uri, doc, ctx);
  if (deferred && deferred->hasInlayHintsFull)
    return deferred->inlayHintsFull;

  Json hints = inlayHintsRuntimeBuildFullDocument(uri, doc, ctx);
  if (!deferred)
    return hints;

  auto writable = std::make_shared<DeferredDocSnapshot>(*deferred);
  writable->inlayHintsFull = hints;
  writable->hasInlayHintsFull = true;
  documentOwnerStoreDeferredSnapshot(uri, writable);
  return hints;
}

Json inlayHintsRuntimeFilterRange(const Json &fullHints, int startLine,
                                  int startChar, int endLine, int endChar) {
  Json filtered = makeArray();
  if (fullHints.type != Json::Type::Array)
    return filtered;
  for (const auto &hint : fullHints.a) {
    const Json *position = getObjectValue(hint, "position");
    const Json *lineValue =
        position ? getObjectValue(*position, "line") : nullptr;
    const Json *charValue =
        position ? getObjectValue(*position, "character") : nullptr;
    if (!lineValue || !charValue || lineValue->type != Json::Type::Number ||
        charValue->type != Json::Type::Number) {
      continue;
    }
    const int line = static_cast<int>(getNumberValue(*lineValue));
    const int character = static_cast<int>(getNumberValue(*charValue));
    if (isPositionWithinRange(line, character, startLine, startChar, endLine,
                              endChar)) {
      filtered.a.push_back(hint);
    }
  }
  return filtered;
}
