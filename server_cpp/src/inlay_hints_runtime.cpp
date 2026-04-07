#include "inlay_hints_runtime.hpp"

#include "call_query.hpp"
#include "callsite_parser.hpp"
#include "deferred_doc_runtime.hpp"
#include "document_owner.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_markdown.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "server_parse.hpp"
#include "server_request_handlers.hpp"
#include "text_utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
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

bool isStencilBinaryHintTarget(const std::string &name) {
  return name == "StencilRef" || name == "StencilWriteMask" ||
         name == "StencilReadMask";
}

bool parseStencilIntegerLiteral(const std::string &literal, uint64_t &valueOut,
                                size_t &bitWidthOut) {
  valueOut = 0;
  bitWidthOut = 0;

  std::string value = trimRightCopy(trimLeftCopy(literal));
  while (!value.empty()) {
    const char tail = value.back();
    if (tail == 'u' || tail == 'U' || tail == 'l' || tail == 'L') {
      value.pop_back();
      continue;
    }
    break;
  }
  if (value.empty())
    return false;

  try {
    size_t consumed = 0;
    valueOut = std::stoull(value, &consumed, 0);
    if (consumed != value.size())
      return false;
  } catch (...) {
    return false;
  }

  const bool isHex = value.size() > 2 && value[0] == '0' &&
                     (value[1] == 'x' || value[1] == 'X');
  if (isHex) {
    bitWidthOut = std::max<size_t>(1, (value.size() - 2) * 4);
    return true;
  }

  size_t bitsNeeded = 1;
  uint64_t remaining = valueOut;
  while (remaining > 1) {
    remaining >>= 1;
    bitsNeeded++;
  }
  if (bitsNeeded <= 8) {
    bitWidthOut = 8;
  } else if (bitsNeeded <= 16) {
    bitWidthOut = 16;
  } else if (bitsNeeded <= 32) {
    bitWidthOut = 32;
  } else {
    bitWidthOut = ((bitsNeeded + 7) / 8) * 8;
  }
  return true;
}

std::string formatStencilBinaryLiteral(uint64_t value, size_t bitWidth) {
  if (bitWidth == 0)
    return "0b0";
  std::string full = "0b";
  full.reserve(2 + bitWidth);
  for (size_t bit = 0; bit < bitWidth; bit++) {
    const size_t shift = bitWidth - bit - 1;
    const bool set = shift < 64 ? ((value >> shift) & 1ULL) != 0 : false;
    full.push_back(set ? '1' : '0');
  }
  return full;
}

bool tryExtractStencilAssignmentHint(const std::string &lineText,
                                     std::string &labelOut,
                                     int &characterOut) {
  labelOut.clear();
  characterOut = 0;

  std::string code = lineText;
  const size_t lineComment = code.find("//");
  if (lineComment != std::string::npos)
    code = code.substr(0, lineComment);

  const auto tokens = lexLineTokens(code);
  if (tokens.size() < 3 || tokens[0].kind != LexToken::Kind::Identifier)
    return false;
  if (!isStencilBinaryHintTarget(tokens[0].text))
    return false;

  const size_t equalsPos = code.find('=');
  const size_t semicolonPos =
      equalsPos == std::string::npos ? std::string::npos
                                     : code.find(';', equalsPos + 1);
  if (equalsPos == std::string::npos || semicolonPos == std::string::npos)
    return false;

  size_t valueStart = equalsPos + 1;
  while (valueStart < semicolonPos &&
         std::isspace(static_cast<unsigned char>(code[valueStart]))) {
    valueStart++;
  }
  size_t valueEnd = semicolonPos;
  while (valueEnd > valueStart &&
         std::isspace(static_cast<unsigned char>(code[valueEnd - 1]))) {
    valueEnd--;
  }
  if (valueEnd <= valueStart)
    return false;

  uint64_t parsedValue = 0;
  size_t bitWidth = 0;
  if (!parseStencilIntegerLiteral(code.substr(valueStart, valueEnd - valueStart),
                                  parsedValue, bitWidth)) {
    return false;
  }

  labelOut = formatStencilBinaryLiteral(parsedValue, bitWidth);
  characterOut =
      byteOffsetInLineToUtf16(lineText, static_cast<int>(valueEnd));
  return !labelOut.empty();
}

void appendPassStencilBinaryInlayHints(const Document &doc, Json &hints,
                                       int maxHints, int minLine,
                                       int maxLineInclusive) {
  const std::vector<std::string> lines = splitLinesShared(doc.text);
  const std::vector<std::string> trimmedLines =
      buildTrimmedCodeLinesShared(doc.text);

  int braceDepth = 0;
  bool pendingPassBody = false;
  int activePassBraceDepth = -1;

  auto updateBraceDepth = [&](const std::string &line) {
    bool inString = false;
    bool inBlockComment = false;
    for (size_t i = 0; i < line.size(); i++) {
      const char ch = line[i];
      const char next = (i + 1 < line.size()) ? line[i + 1] : '\0';
      if (inBlockComment) {
        if (ch == '*' && next == '/') {
          inBlockComment = false;
          i++;
        }
        continue;
      }
      if (inString) {
        if (ch == '"' && (i == 0 || line[i - 1] != '\\'))
          inString = false;
        continue;
      }
      if (ch == '/' && next == '*') {
        inBlockComment = true;
        i++;
        continue;
      }
      if (ch == '/' && next == '/')
        break;
      if (ch == '"') {
        inString = true;
        continue;
      }
      if (ch == '{') {
        braceDepth++;
      } else if (ch == '}' && braceDepth > 0) {
        braceDepth--;
      }
    }
  };

  for (size_t lineIndex = 0; lineIndex < lines.size(); lineIndex++) {
    const int braceDepthBeforeLine = braceDepth;
    const std::string &trimmed =
        lineIndex < trimmedLines.size() ? trimmedLines[lineIndex] : std::string();

    if (pendingPassBody) {
      if (!trimmed.empty() && trimmed[0] == '{') {
        activePassBraceDepth = braceDepthBeforeLine + 1;
        pendingPassBody = false;
      } else if (!trimmed.empty() && trimmed[0] != '<' && trimmed[0] != '>') {
        pendingPassBody = false;
      }
    }

    std::string blockKind;
    std::string blockName;
    if (extractTechniquePassDeclarationHeaderShared(trimmed, blockKind,
                                                    blockName) &&
        blockKind == "pass") {
      if (trimmed.find('{') != std::string::npos) {
        activePassBraceDepth = braceDepthBeforeLine + 1;
      } else {
        pendingPassBody = true;
      }
    }

    if (activePassBraceDepth >= 0 &&
        braceDepthBeforeLine == activePassBraceDepth &&
        static_cast<int>(lineIndex) >= minLine &&
        static_cast<int>(lineIndex) <= maxLineInclusive &&
        static_cast<int>(hints.a.size()) < maxHints) {
      std::string binaryLabel;
      int character = 0;
      if (tryExtractStencilAssignmentHint(lines[lineIndex], binaryLabel,
                                          character)) {
        Json position = makeObject();
        position.o["line"] = makeNumber(static_cast<double>(lineIndex));
        position.o["character"] = makeNumber(character);

        Json hint = makeObject();
        hint.o["position"] = position;
        hint.o["label"] = makeString(binaryLabel);
        hint.o["kind"] = makeNumber(1);
        hint.o["paddingLeft"] = makeBool(true);
        hints.a.push_back(std::move(hint));
      }
    }

    updateBraceDepth(lines[lineIndex]);
    if (activePassBraceDepth >= 0 && braceDepth < activePassBraceDepth)
      activePassBraceDepth = -1;
  }
}

Json buildInlayHintsForOffsets(const std::string &uri, const Document &doc,
                               ServerRequestContext &ctx, size_t startOffset,
                               size_t endOffset, int minLine,
                               int maxLineInclusive, int maxHints) {
  std::vector<CallSiteArgument> arguments;
  collectCallArgumentsInRange(doc.text, startOffset, endOffset, arguments);
  const uint64_t slowEpoch = updateInlaySlowEpoch(uri, doc.version);
  std::unordered_map<std::string, std::vector<std::string>> parameterCache;
  std::unordered_set<std::string> slowResolveSet;
  std::vector<size_t> lineStarts;
  collectLineStarts(doc.text, lineStarts);
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
  appendPassStencilBinaryInlayHints(doc, hints, maxHints, minLine,
                                    maxLineInclusive);
  if (ctx.inlayHintsEnabled && ctx.inlayHintsParameterNamesEnabled &&
      !slowResolveSet.empty()) {
    std::vector<std::string> cacheKeys(slowResolveSet.begin(),
                                       slowResolveSet.end());
    scheduleInlaySlowResolve(uri, slowEpoch, doc.version, std::move(cacheKeys),
                             ctx);
  }
  return hints;
}

} // namespace

Json inlayHintsRuntimeBuildFullDocument(const std::string &uri,
                                        const Document &doc,
                                        ServerRequestContext &ctx) {
  static constexpr int kFullDocumentMaxHints = 4096;
  const int documentLineCount =
      static_cast<int>(splitLinesShared(doc.text).size());
  return buildInlayHintsForOffsets(uri, doc, ctx, 0, doc.text.size(), 0,
                                   std::max(0, documentLineCount - 1),
                                   kFullDocumentMaxHints);
}

Json inlayHintsRuntimeBuildRange(const std::string &uri, const Document &doc,
                                 ServerRequestContext &ctx, int startLine,
                                 int startChar, int endLine, int endChar) {
  static constexpr int kRangeMaxHints = 160;
  const std::string &text = doc.text;
  size_t startOffset = positionToOffsetUtf16(text, startLine, startChar);
  size_t endOffset = positionToOffsetUtf16(text, endLine, endChar);
  if (endOffset < startOffset)
    std::swap(startOffset, endOffset);

  const auto buildStart = std::chrono::steady_clock::now();
  Json hints = buildInlayHintsForOffsets(uri, doc, ctx, startOffset, endOffset,
                                         std::min(startLine, endLine),
                                         std::max(startLine, endLine),
                                         kRangeMaxHints);
  const double buildDurationMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                buildStart)
          .count();
  recordInlayRangeBuild(buildDurationMs);
  return hints;
}

Json inlayHintsRuntimeBuildOrGetDeferredFull(const std::string &uri,
                                             const Document &doc,
                                             ServerRequestContext &ctx) {
  auto deferred = getOrBuildDeferredDocSnapshot(uri, doc, ctx);
  if (deferred && deferred->hasInlayHintsFull) {
    recordInlayDeferredSnapshotHit();
    return deferred->inlayHintsFull;
  }

  recordInlayDeferredSnapshotMiss();
  const auto buildStart = std::chrono::steady_clock::now();
  Json hints = inlayHintsRuntimeBuildFullDocument(uri, doc, ctx);
  const double buildDurationMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                buildStart)
          .count();
  recordInlayFullBuild(buildDurationMs);
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
