#include "server_request_handlers.hpp"

#include "callsite_parser.hpp"
#include "call_query.hpp"
#include "completion_rendering.hpp"
#include "declaration_query.hpp"
#include "definition_fallback.hpp"
#include "definition_location.hpp"
#include "fast_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_docs.hpp"
#include "hover_markdown.hpp"
#include "hover_rendering.hpp"
#include "include_resolver.hpp"
#include "indeterminate_reasons.hpp"
#include "language_registry.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "macro_generated_functions.hpp"
#include "member_query.hpp"
#include "nsf_lexer.hpp"
#include "overload_resolver.hpp"
#include "semantic_tokens.hpp"
#include "server_occurrences.hpp"
#include "server_parse.hpp"
#include "signature_help.hpp"
#include "symbol_query.hpp"
#include "text_utils.hpp"
#include "type_desc.hpp"
#include "type_eval.hpp"
#include "type_model.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"
#include "workspace_scan_plan.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <sys/stat.h>
#endif

bool findDefinitionInIncludeGraph(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited, DefinitionLocation &outLocation);
bool findDefinitionInIncludeGraphLegacy(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited, DefinitionLocation &outLocation);
static bool definitionLocationEquals(const DefinitionLocation &a,
                                     const DefinitionLocation &b);
static std::string
definitionLocationToString(const DefinitionLocation &location);

bool findTypeOfIdentifierInTextUpTo(const std::string &text,
                                    const std::string &identifier,
                                    size_t maxOffset, std::string &typeNameOut);

bool findParameterTypeInTextUpTo(const std::string &text,
                                 const std::string &identifier,
                                 size_t maxOffset, std::string &typeNameOut);

bool findTypeOfIdentifierInIncludeGraph(
    const std::string &uri, const std::string &identifier,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited, std::string &typeNameOut);

bool collectStructFieldsInIncludeGraph(
    const std::string &uri, const std::string &structName,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited,
    std::vector<std::string> &fieldsOut);

bool collectStructFieldsFromText(const std::string &text,
                                 const std::string &structName,
                                 std::vector<std::string> &fieldsOut);

size_t positionToOffset(const std::string &text, int line, int character);

std::vector<LocatedOccurrence> collectOccurrencesForSymbol(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions);

bool findMacroDefinitionInLine(const std::string &line, const std::string &word,
                               size_t &posOut);

bool findDeclaredIdentifierInDeclarationLine(const std::string &line,
                                             const std::string &word,
                                             size_t &posOut);

namespace {

constexpr bool kEnableInlayHintsSlowPath = true;
constexpr bool kEnableSemanticCacheShadowCompare = false;
constexpr bool kEnableOverloadResolver = false;
constexpr bool kEnableOverloadResolverShadowCompare = false;

} // namespace

using CoreRequestHandler = bool (*)(const std::string &, const Json &,
                                    const Json *, ServerRequestContext &,
                                    const std::vector<std::string> &,
                                    const std::vector<std::string> &);

static bool handleDefinitionRequest(const std::string &, const Json &,
                                    const Json *, ServerRequestContext &,
                                    const std::vector<std::string> &,
                                    const std::vector<std::string> &);
static bool handleHoverRequest(const std::string &, const Json &, const Json *,
                               ServerRequestContext &,
                               const std::vector<std::string> &,
                               const std::vector<std::string> &);
static bool handleCompletionRequest(const std::string &, const Json &,
                                    const Json *, ServerRequestContext &,
                                    const std::vector<std::string> &,
                                    const std::vector<std::string> &);
static bool handleSignatureHelpRequest(const std::string &, const Json &,
                                       const Json *, ServerRequestContext &,
                                       const std::vector<std::string> &,
                                       const std::vector<std::string> &);
static bool handleInlayHintRequest(const std::string &, const Json &,
                                   const Json *, ServerRequestContext &,
                                   const std::vector<std::string> &,
                                   const std::vector<std::string> &);
static bool handleSemanticTokensFullRequest(const std::string &, const Json &,
                                            const Json *,
                                            ServerRequestContext &,
                                            const std::vector<std::string> &,
                                            const std::vector<std::string> &);
static bool handleSemanticTokensRangeRequest(const std::string &, const Json &,
                                             const Json *,
                                             ServerRequestContext &,
                                             const std::vector<std::string> &,
                                             const std::vector<std::string> &);
static bool handleReferencesRequest(const std::string &, const Json &,
                                    const Json *, ServerRequestContext &,
                                    const std::vector<std::string> &,
                                    const std::vector<std::string> &);

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
static bool handlePrepareRenameRequest(const std::string &, const Json &,
                                       const Json *, ServerRequestContext &,
                                       const std::vector<std::string> &,
                                       const std::vector<std::string> &);
static bool handleRenameRequest(const std::string &, const Json &, const Json *,
                                ServerRequestContext &,
                                const std::vector<std::string> &,
                                const std::vector<std::string> &);
static bool handleDocumentSymbolRequest(const std::string &, const Json &,
                                        const Json *, ServerRequestContext &,
                                        const std::vector<std::string> &,
                                        const std::vector<std::string> &);

static const CoreRequestHandler kCoreRequestHandlers[] = {
    handleDefinitionRequest,
    handleHoverRequest,
    handleCompletionRequest,
    handleSignatureHelpRequest,
    handleInlayHintRequest,

    handleSemanticTokensFullRequest,
    handleSemanticTokensRangeRequest,

    handleReferencesRequest,
    handlePrepareRenameRequest,
    handleRenameRequest,

    handleDocumentSymbolRequest,
};

struct InlayCallArgument {
  std::string functionName;
  int argumentIndex = 0;
  size_t argumentStartOffset = 0;
};

struct InlayCallFrame {
  bool isCallable = false;
  std::string functionName;
  int argumentIndex = 0;
  bool expectingArgument = true;
  int angleDepth = 0;
  int bracketDepth = 0;
  int braceDepth = 0;
};

struct InlaySlowResolveTask {
  std::string uri;
  uint64_t epoch = 0;
  int documentVersion = 0;
  std::vector<std::string> cacheKeys;
  ServerRequestContext context;
};

static std::mutex gInlayParameterCacheMutex;
static std::unordered_map<std::string, std::vector<std::string>>
    gInlayParameterCache;
static std::atomic<uint64_t> gSignatureHelpIndeterminateTotal{0};
static std::atomic<uint64_t> gSignatureHelpIndeterminateReasonCallTargetUnknown{
    0};
static std::atomic<uint64_t>
    gSignatureHelpIndeterminateReasonDefinitionTextUnavailable{0};
static std::atomic<uint64_t>
    gSignatureHelpIndeterminateReasonSignatureExtractFailed{0};
static std::atomic<uint64_t> gSignatureHelpIndeterminateReasonOther{0};
static std::atomic<uint64_t> gOverloadResolverAttempts{0};
static std::atomic<uint64_t> gOverloadResolverResolved{0};
static std::atomic<uint64_t> gOverloadResolverAmbiguous{0};
static std::atomic<uint64_t> gOverloadResolverNoViable{0};
static std::atomic<uint64_t> gOverloadResolverShadowMismatch{0};
static std::mutex gInlaySlowMutex;
static std::condition_variable gInlaySlowCv;
static std::deque<InlaySlowResolveTask> gInlaySlowQueue;
static std::unordered_map<std::string, uint64_t> gInlaySlowLatestEpochByUri;
static std::unordered_map<std::string, int> gInlaySlowLatestVersionByUri;
static std::once_flag gInlaySlowWorkerInitFlag;

static bool tryGetInlayParameterCache(const std::string &cacheKey,
                                      std::vector<std::string> &paramsOut) {
  std::lock_guard<std::mutex> lock(gInlayParameterCacheMutex);
  auto it = gInlayParameterCache.find(cacheKey);
  if (it == gInlayParameterCache.end())
    return false;
  paramsOut = it->second;
  return true;
}

static void updateInlayParameterCache(const std::string &cacheKey,
                                      const std::vector<std::string> &params) {
  std::lock_guard<std::mutex> lock(gInlayParameterCacheMutex);
  gInlayParameterCache[cacheKey] = params;
}

static std::string makeInlayParameterCacheKey(const std::string &functionName,
                                              bool isMemberCall) {
  return (isMemberCall ? "method:" : "func:") + functionName;
}

static bool parseInlayParameterCacheKey(const std::string &cacheKey,
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
    isMemberCallOut = false;
    nameOut = cacheKey.substr(5);
    return !nameOut.empty();
  }
  return false;
}

static bool resolveBuiltinMethodParametersForInlay(
    const std::string &methodName, std::vector<std::string> &outParams) {
  outParams.clear();
  if (methodName.empty())
    return false;
  std::vector<HlslBuiltinSignature> signatures;
  static const std::vector<std::string> kBaseTypeCandidates = {
      "Texture2D",
      "RWTexture2D",
      "Buffer",
      "RWBuffer",
      "StructuredBuffer",
      "RWStructuredBuffer",
      "ByteAddressBuffer",
      "RWByteAddressBuffer",
      "SamplerState",
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

static uint64_t updateInlaySlowEpoch(const std::string &uri,
                                     int documentVersion) {
  std::lock_guard<std::mutex> lock(gInlaySlowMutex);
  gInlaySlowLatestVersionByUri[uri] = documentVersion;
  return ++gInlaySlowLatestEpochByUri[uri];
}

static bool isInlaySlowEpochLatest(const std::string &uri, uint64_t epoch) {
  std::lock_guard<std::mutex> lock(gInlaySlowMutex);
  auto it = gInlaySlowLatestEpochByUri.find(uri);
  return it != gInlaySlowLatestEpochByUri.end() && it->second == epoch;
}

static bool isInlaySlowDocumentVersionLatest(const std::string &uri,
                                             int documentVersion) {
  std::lock_guard<std::mutex> lock(gInlaySlowMutex);
  auto it = gInlaySlowLatestVersionByUri.find(uri);
  return it != gInlaySlowLatestVersionByUri.end() &&
         it->second == documentVersion;
}

static void ensureInlaySlowWorkerStarted() {
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
            !isInlaySlowDocumentVersionLatest(task.uri, task.documentVersion))
          continue;
        bool changed = false;
        for (const auto &cacheKey : task.cacheKeys) {
          if (!isInlaySlowEpochLatest(task.uri, task.epoch) ||
              !isInlaySlowDocumentVersionLatest(task.uri, task.documentVersion))
            break;
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
          if (!resolved ||
              paramsOut.empty()) {
            continue;
          }
          updateInlayParameterCache(cacheKey, paramsOut);
          changed = true;
        }
        if (changed && isInlaySlowEpochLatest(task.uri, task.epoch) &&
            isInlaySlowDocumentVersionLatest(task.uri, task.documentVersion)) {
          Json params = makeObject();
          params.o["uri"] = makeString(task.uri);
          writeNotification("nsf/inlayHintsChanged", params);
        }
      }
    }).detach();
  });
}

static void scheduleInlaySlowResolve(const std::string &uri, uint64_t epoch,
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

static bool isRequestCancelled(ServerRequestContext &ctx) {
  if (!ctx.isCancellationRequested)
    return false;
  return ctx.isCancellationRequested();
}

static bool isSpace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

static bool isLikelyStringPrefixChar(char ch) {
  unsigned char c = static_cast<unsigned char>(ch);
  return std::isalpha(c) != 0 || ch == '_';
}

static std::string
signatureHelpTargetToString(const SignatureHelpTargetResult &value) {
  if (value.builtinSigs != nullptr && !value.builtinSigs->empty())
    return "builtin:" + value.builtinSigs->front().label;
  if (value.hasDefinition)
    return "definition:" + definitionLocationToString(value.definition);
  if (!value.typeEval.reasonCode.empty())
    return "indeterminate:" + value.typeEval.reasonCode;
  return "empty";
}

static void emitSignatureHelpShadowCompareMismatch(
    const std::string &uri, const std::string &functionName,
    const SignatureHelpTargetResult &newPath,
    const SignatureHelpTargetResult &oldPath) {
  Json params = makeObject();
  params.o["type"] = makeNumber(4);
  params.o["message"] =
      makeString("nsf signatureHelp shadowCompare mismatch: uri=" + uri +
                 " function=" + functionName +
                 " newPath=" + signatureHelpTargetToString(newPath) +
                 " oldPath=" + signatureHelpTargetToString(oldPath));
  writeNotification("window/logMessage", params);
}

static void
emitSignatureHelpIndeterminateTrace(const std::string &functionName,
                                    const TypeEvalResult &typeEval) {
  if (!typeEval.type.empty() || typeEval.reasonCode.empty())
    return;
  gSignatureHelpIndeterminateTotal.fetch_add(1, std::memory_order_relaxed);
  if (typeEval.reasonCode ==
      IndeterminateReason::SignatureHelpCallTargetUnknown) {
    gSignatureHelpIndeterminateReasonCallTargetUnknown.fetch_add(
        1, std::memory_order_relaxed);
  } else if (typeEval.reasonCode ==
             IndeterminateReason::SignatureHelpDefinitionTextUnavailable) {
    gSignatureHelpIndeterminateReasonDefinitionTextUnavailable.fetch_add(
        1, std::memory_order_relaxed);
  } else if (typeEval.reasonCode ==
             IndeterminateReason::SignatureHelpSignatureExtractFailed) {
    gSignatureHelpIndeterminateReasonSignatureExtractFailed.fetch_add(
        1, std::memory_order_relaxed);
  } else {
    gSignatureHelpIndeterminateReasonOther.fetch_add(1,
                                                     std::memory_order_relaxed);
  }
  Json params = makeObject();
  params.o["type"] = makeNumber(4);
  params.o["message"] =
      makeString("nsf signatureHelp indeterminate: " + functionName +
                 " reason=" + typeEval.reasonCode);
  writeNotification("window/logMessage", params);
}

static std::string extractCallNameBefore(const std::string &text,
                                         size_t offset) {
  if (offset == 0 || offset > text.size())
    return "";
  size_t end = offset;
  while (end > 0 && isSpace(text[end - 1]))
    end--;
  if (end == 0 || !isIdentifierChar(text[end - 1]))
    return "";
  size_t start = end;
  while (start > 0 && isIdentifierChar(text[start - 1]))
    start--;
  if (start > 0 && text[start - 1] == '.') {
    size_t dotStart = start - 1;
    while (dotStart > 0 && isSpace(text[dotStart - 1]))
      dotStart--;
    if (dotStart == 0)
      return "";
  }
  return text.substr(start, end - start);
}

static void emitSignatureHelpResolverShadowMismatch(
    const std::string &uri, const std::string &functionName,
    const std::string &resolverLabel, const std::string &legacyLabel,
    const std::string &resolverStatus) {
  gOverloadResolverShadowMismatch.fetch_add(1, std::memory_order_relaxed);
  Json params = makeObject();
  params.o["type"] = makeNumber(4);
  params.o["message"] =
      makeString("nsf signatureHelp overloadResolver mismatch: uri=" + uri +
                 " function=" + functionName + " status=" + resolverStatus +
                 " resolver=" + resolverLabel + " legacy=" + legacyLabel);
  writeNotification("window/logMessage", params);
}

static void recordOverloadResolverResult(const ResolveCallResult &result) {
  gOverloadResolverAttempts.fetch_add(1, std::memory_order_relaxed);
  switch (result.status) {
  case ResolveCallStatus::Resolved:
    gOverloadResolverResolved.fetch_add(1, std::memory_order_relaxed);
    break;
  case ResolveCallStatus::Ambiguous:
    gOverloadResolverAmbiguous.fetch_add(1, std::memory_order_relaxed);
    break;
  case ResolveCallStatus::NoViable:
    gOverloadResolverNoViable.fetch_add(1, std::memory_order_relaxed);
    break;
  }
}

static void collectCallArgumentsInRangeLegacy(
    const std::string &text, size_t rangeStartOffset, size_t rangeEndOffset,
    std::vector<InlayCallArgument> &out) {
  out.clear();
  const size_t scanEnd = std::min(rangeEndOffset, text.size());
  if (rangeStartOffset >= scanEnd)
    return;

  enum class ScanState {
    Normal,
    LineComment,
    BlockComment,
    StringDouble,
    StringSingle
  };

  ScanState state = ScanState::Normal;
  std::vector<InlayCallFrame> stack;

  auto maybeRecordArgumentStart = [&](size_t pos) {
    if (stack.empty())
      return;
    InlayCallFrame &frame = stack.back();
    if (!frame.isCallable || !frame.expectingArgument)
      return;
    if (frame.angleDepth != 0 || frame.bracketDepth != 0 ||
        frame.braceDepth != 0)
      return;
    frame.expectingArgument = false;
    if (pos >= rangeStartOffset && pos < rangeEndOffset) {
      out.push_back({frame.functionName, frame.argumentIndex, pos});
    }
  };

  for (size_t i = 0; i < scanEnd; i++) {
    char ch = text[i];
    char next = i + 1 < text.size() ? text[i + 1] : '\0';

    if (state == ScanState::LineComment) {
      if (ch == '\n')
        state = ScanState::Normal;
      continue;
    }
    if (state == ScanState::BlockComment) {
      if (ch == '*' && next == '/') {
        state = ScanState::Normal;
        i++;
      }
      continue;
    }
    if (state == ScanState::StringDouble) {
      if (ch == '\\' && next != '\0') {
        i++;
        continue;
      }
      if (ch == '"')
        state = ScanState::Normal;
      continue;
    }
    if (state == ScanState::StringSingle) {
      if (ch == '\\' && next != '\0') {
        i++;
        continue;
      }
      if (ch == '\'')
        state = ScanState::Normal;
      continue;
    }

    if (ch == '/' && next == '/') {
      state = ScanState::LineComment;
      i++;
      continue;
    }
    if (ch == '/' && next == '*') {
      state = ScanState::BlockComment;
      i++;
      continue;
    }
    if (ch == '"') {
      bool prefixed = i > 0 && isLikelyStringPrefixChar(text[i - 1]);
      if (!prefixed)
        state = ScanState::StringDouble;
      continue;
    }
    if (ch == '\'') {
      state = ScanState::StringSingle;
      continue;
    }

    if (ch == '(') {
      std::string callName = extractCallNameBefore(text, i);
      InlayCallFrame frame;
      frame.isCallable = !callName.empty();
      frame.functionName = std::move(callName);
      stack.push_back(std::move(frame));
      continue;
    }
    if (ch == ')') {
      if (!stack.empty())
        stack.pop_back();
      continue;
    }
    if (stack.empty())
      continue;

    InlayCallFrame &frame = stack.back();
    if (!frame.isCallable)
      continue;

    if (ch == '<') {
      frame.angleDepth++;
      continue;
    }
    if (ch == '>' && frame.angleDepth > 0) {
      frame.angleDepth--;
      continue;
    }
    if (ch == '[') {
      frame.bracketDepth++;
      continue;
    }
    if (ch == ']' && frame.bracketDepth > 0) {
      frame.bracketDepth--;
      continue;
    }
    if (ch == '{') {
      frame.braceDepth++;
      continue;
    }
    if (ch == '}' && frame.braceDepth > 0) {
      frame.braceDepth--;
      continue;
    }
    if (frame.angleDepth == 0 && frame.bracketDepth == 0 &&
        frame.braceDepth == 0 && ch == ',') {
      frame.argumentIndex++;
      frame.expectingArgument = true;
      continue;
    }
    if (!isSpace(ch))
      maybeRecordArgumentStart(i);
  }
}

static void collectLineStarts(const std::string &text,
                              std::vector<size_t> &lineStarts) {
  lineStarts.clear();
  lineStarts.reserve(128);
  lineStarts.push_back(0);
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '\n' && i + 1 <= text.size())
      lineStarts.push_back(i + 1);
  }
}

static bool offsetToPosition(const std::string &text,
                             const std::vector<size_t> &lineStarts,
                             size_t offset, int &lineOut, int &characterOut) {
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

static bool handleSignatureHelpRequest(const std::string &method,
                                       const Json &id, const Json *params,
                                       ServerRequestContext &ctx,
                                       const std::vector<std::string> &,
                                       const std::vector<std::string> &) {
  if (method != "textDocument/signatureHelp" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  if (!textDocument || !position) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *lineValue = getObjectValue(*position, "line");
  const Json *charValue = getObjectValue(*position, "character");
  if (!uriValue || !lineValue || !charValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  int line = static_cast<int>(getNumberValue(*lineValue));
  int character = static_cast<int>(getNumberValue(*charValue));
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }

  size_t cursorOffset = positionToOffset(doc->text, line, character);
  std::string functionName;
  int activeParameter = 0;
  if (!parseCallAtOffset(doc->text, cursorOffset, functionName,
                         activeParameter)) {
    writeResponse(id, makeNull());
    return true;
  }

  std::string base;
  std::string member;
  int memberActiveParameter = 0;
  if (parseMemberCallAtOffset(doc->text, cursorOffset, base, member,
                              memberActiveParameter)) {
    bool isParam = false;
    TypeEvalResult baseEval = resolveHoverTypeAtDeclaration(
        uri, *doc, base, cursorOffset, ctx, isParam);
    if (!baseEval.type.empty()) {
      std::vector<HlslBuiltinSignature> methodSigs;
      if (lookupHlslBuiltinMethodSignatures(member, baseEval.type,
                                            methodSigs) &&
          !methodSigs.empty()) {
        Json signatures = makeArray();
        for (const auto &sig : methodSigs) {
          Json parameters = makeArray();
          for (const auto &p : sig.parameters) {
            Json parameter = makeObject();
            parameter.o["label"] = makeString(p);
            parameters.a.push_back(std::move(parameter));
          }
          Json signature = makeObject();
          signature.o["label"] = makeString(sig.label);
          signature.o["parameters"] = parameters;
          if (!sig.documentation.empty())
            signature.o["documentation"] = makeMarkup(sig.documentation);
          signatures.a.push_back(std::move(signature));
        }

        int clampedActive = 0;
        const auto &params = methodSigs.front().parameters;
        if (!params.empty()) {
          clampedActive =
              std::max(0, std::min(memberActiveParameter,
                                   static_cast<int>(params.size()) - 1));
        }

        Json result = makeObject();
        result.o["signatures"] = signatures;
        result.o["activeSignature"] = makeNumber(0);
        result.o["activeParameter"] = makeNumber(clampedActive);
        writeResponse(id, result);
        return true;
      }
    }
  }

  SignatureHelpTargetResult target =
      resolveSignatureHelpTarget(uri, functionName, ctx);
  SignatureHelpTargetResult legacyTarget;
  bool hasLegacyTarget = false;
  if (kEnableSemanticCacheShadowCompare) {
    legacyTarget = resolveSignatureHelpTargetLegacy(uri, functionName, ctx);
    hasLegacyTarget = true;
    if (!signatureHelpTargetEquals(target, legacyTarget)) {
      emitSignatureHelpShadowCompareMismatch(uri, functionName, target,
                                             legacyTarget);
    }
  }
  if (target.builtinSigs && !target.builtinSigs->empty()) {
    Json signatures = makeArray();
    const size_t signatureLimit =
        std::min<size_t>(50, target.builtinSigs->size());
    for (size_t i = 0; i < signatureLimit; i++) {
      const auto &sig = (*target.builtinSigs)[i];
      Json parameters = makeArray();
      for (const auto &p : sig.parameters) {
        Json parameter = makeObject();
        parameter.o["label"] = makeString(p);
        parameters.a.push_back(std::move(parameter));
      }
      Json signature = makeObject();
      signature.o["label"] = makeString(sig.label);
      signature.o["parameters"] = parameters;
      if (!sig.documentation.empty())
        signature.o["documentation"] = makeMarkup(sig.documentation);
      signatures.a.push_back(std::move(signature));
    }

    int activeSignature = 0;
    for (size_t i = 0; i < signatureLimit; i++) {
      const auto &sig = (*target.builtinSigs)[i];
      if (activeParameter < static_cast<int>(sig.parameters.size())) {
        activeSignature = static_cast<int>(i);
        break;
      }
    }

    int clampedActive = 0;
    const auto &params = (*target.builtinSigs)[static_cast<size_t>(activeSignature)].parameters;
    if (!params.empty()) {
      clampedActive =
          std::max(0, std::min(activeParameter,
                               static_cast<int>(params.size()) - 1));
    }

    Json result = makeObject();
    result.o["signatures"] = signatures;
    result.o["activeSignature"] = makeNumber(activeSignature);
    result.o["activeParameter"] = makeNumber(clampedActive);
    writeResponse(id, result);
    return true;
  }

  if (!target.hasDefinition) {
    if (hasLegacyTarget && legacyTarget.hasDefinition)
      target = legacyTarget;
  }
  if (!target.hasDefinition) {
    emitSignatureHelpIndeterminateTrace(functionName, target.typeEval);
    writeResponse(id, makeNull());
    return true;
  }
  if (kEnableOverloadResolver) {
    std::vector<CandidateSignature> candidates;
    if (collectFunctionOverloadCandidates(uri, functionName, target, ctx,
                                          candidates)) {
      std::vector<TypeDesc> argumentTypes;
      inferCallArgumentTypesAtCursor(uri, doc->text, cursorOffset,
                                     argumentTypes);
      ResolveCallContext resolveContext;
      resolveContext.defines = ctx.preprocessorDefines;
      resolveContext.allowNarrowing = false;
      resolveContext.enableVisibilityFiltering = true;
      resolveContext.allowPartialArity = true;
      ResolveCallResult resolveResult =
          resolveCallCandidates(candidates, argumentTypes, resolveContext);
      recordOverloadResolverResult(resolveResult);
      if (!resolveResult.rankedCandidates.empty() &&
          resolveResult.status != ResolveCallStatus::NoViable) {
        Json signatures = makeArray();
        size_t signatureLimit =
            std::min<size_t>(50, resolveResult.rankedCandidates.size());
        for (size_t i = 0; i < signatureLimit; i++) {
          const CandidateScore &score = resolveResult.rankedCandidates[i];
          if (score.candidateIndex < 0 ||
              static_cast<size_t>(score.candidateIndex) >= candidates.size()) {
            continue;
          }
          const CandidateSignature &candidate =
              candidates[static_cast<size_t>(score.candidateIndex)];
          Json signature = makeObject();
          signature.o["label"] = makeString(candidate.displayLabel.empty()
                                                ? (functionName + "(...)")
                                                : candidate.displayLabel);
          Json parameters = makeArray();
          for (const auto &param : candidate.displayParams) {
            Json parameter = makeObject();
            parameter.o["label"] = makeString(param);
            parameters.a.push_back(std::move(parameter));
          }
          signature.o["parameters"] = parameters;
          signatures.a.push_back(std::move(signature));
        }
        int clampedActive = 0;
        const CandidateScore &bestScore =
            resolveResult.rankedCandidates.front();
        std::string resolverLabel = "";
        if (bestScore.candidateIndex >= 0 &&
            static_cast<size_t>(bestScore.candidateIndex) < candidates.size()) {
          const CandidateSignature &bestCandidate =
              candidates[static_cast<size_t>(bestScore.candidateIndex)];
          resolverLabel = bestCandidate.displayLabel;
          if (!bestCandidate.params.empty()) {
            clampedActive = std::max(
                0, std::min(activeParameter,
                            static_cast<int>(bestCandidate.params.size()) - 1));
          }
        }
        if (kEnableOverloadResolverShadowCompare) {
          std::string legacyLabel;
          std::vector<std::string> legacyParams;
          if (resolveFunctionParametersFromTarget(functionName, target, ctx,
                                                  legacyLabel, legacyParams)) {
            if (!legacyLabel.empty() && !resolverLabel.empty() &&
                legacyLabel != resolverLabel) {
              emitSignatureHelpResolverShadowMismatch(
                  uri, functionName, resolverLabel, legacyLabel,
                  resolveCallStatusToString(resolveResult.status));
            }
          }
        }
        Json result = makeObject();
        result.o["signatures"] = signatures;
        result.o["activeSignature"] = makeNumber(0);
        result.o["activeParameter"] = makeNumber(clampedActive);
        writeResponse(id, result);
        return true;
      }
    }
  }
  std::string label;
  std::vector<std::string> paramsOut;
  if (!resolveFunctionParametersFromTarget(functionName, target, ctx, label,
                                           paramsOut)) {
    if (hasLegacyTarget && !signatureHelpTargetEquals(target, legacyTarget) &&
        resolveFunctionParametersFromTarget(functionName, legacyTarget, ctx,
                                            label, paramsOut)) {
      target = legacyTarget;
    } else {
      TypeEvalResult typeEval;
      typeEval.confidence = TypeEvalConfidence::L3;
      typeEval.reasonCode =
          IndeterminateReason::SignatureHelpSignatureExtractFailed;
      emitSignatureHelpIndeterminateTrace(functionName, typeEval);
      writeResponse(id, makeNull());
      return true;
    }
  }

  int clampedActive = 0;
  if (!paramsOut.empty()) {
    clampedActive = std::max(
        0, std::min(activeParameter, static_cast<int>(paramsOut.size()) - 1));
  }

  Json parameters = makeArray();
  for (const auto &p : paramsOut) {
    Json parameter = makeObject();
    parameter.o["label"] = makeString(p);
    parameters.a.push_back(std::move(parameter));
  }
  Json signature = makeObject();
  signature.o["label"] = makeString(label);
  signature.o["parameters"] = parameters;
  Json signatures = makeArray();
  signatures.a.push_back(std::move(signature));

  Json result = makeObject();
  result.o["signatures"] = signatures;
  result.o["activeSignature"] = makeNumber(0);
  result.o["activeParameter"] = makeNumber(clampedActive);
  writeResponse(id, result);
  return true;
}

static bool handleInlayHintRequest(const std::string &method, const Json &id,
                                   const Json *params,
                                   ServerRequestContext &ctx,
                                   const std::vector<std::string> &,
                                   const std::vector<std::string> &) {
  if (method != "textDocument/inlayHint" || !params)
    return false;

  if (!ctx.inlayHintsEnabled || !ctx.inlayHintsParameterNamesEnabled) {
    writeResponse(id, makeArray());
    return true;
  }

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *range = getObjectValue(*params, "range");
  if (!textDocument || !range) {
    writeResponse(id, makeArray());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *start = getObjectValue(*range, "start");
  const Json *end = getObjectValue(*range, "end");
  if (!uriValue || !start || !end) {
    writeResponse(id, makeArray());
    return true;
  }
  const Json *startLineValue = getObjectValue(*start, "line");
  const Json *startCharValue = getObjectValue(*start, "character");
  const Json *endLineValue = getObjectValue(*end, "line");
  const Json *endCharValue = getObjectValue(*end, "character");
  if (!startLineValue || !startCharValue || !endLineValue || !endCharValue) {
    writeResponse(id, makeArray());
    return true;
  }

  std::string uri = getStringValue(*uriValue);
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeArray());
    return true;
  }

  int startLine = static_cast<int>(getNumberValue(*startLineValue));
  int startChar = static_cast<int>(getNumberValue(*startCharValue));
  int endLine = static_cast<int>(getNumberValue(*endLineValue));
  int endChar = static_cast<int>(getNumberValue(*endCharValue));
  if (startLine < 0 || startChar < 0 || endLine < 0 || endChar < 0) {
    writeResponse(id, makeArray());
    return true;
  }

  const std::string &text = doc->text;
  size_t startOffset = positionToOffsetUtf16(text, startLine, startChar);
  size_t endOffset = positionToOffsetUtf16(text, endLine, endChar);
  if (endOffset < startOffset)
    std::swap(startOffset, endOffset);
  if (endOffset == startOffset) {
    writeResponse(id, makeArray());
    return true;
  }

  std::vector<CallSiteArgument> arguments;
  collectCallArgumentsInRange(text, startOffset, endOffset, arguments);
  if (isRequestCancelled(ctx)) {
    writeError(id, -32800, "Request cancelled");
    return true;
  }

  const uint64_t slowEpoch = updateInlaySlowEpoch(uri, doc->version);
  std::unordered_map<std::string, std::vector<std::string>> parameterCache;
  std::unordered_set<std::string> slowResolveSet;
  std::vector<size_t> lineStarts;
  collectLineStarts(text, lineStarts);
  const int maxHints = 160;
  Json hints = makeArray();
  for (const auto &argument : arguments) {
    if (isRequestCancelled(ctx)) {
      writeError(id, -32800, "Request cancelled");
      return true;
    }
    if (static_cast<int>(hints.a.size()) >= maxHints)
      break;
    const std::string cacheKey = makeInlayParameterCacheKey(
        argument.functionName, argument.isMemberCall);
    auto cacheIt = parameterCache.find(cacheKey);
    if (cacheIt == parameterCache.end()) {
      std::vector<std::string> paramsOut;
      if (argument.isMemberCall) {
        if (tryGetInlayParameterCache(cacheKey, paramsOut)) {
          cacheIt =
              parameterCache.emplace(cacheKey, std::move(paramsOut)).first;
        } else if (resolveBuiltinMethodParametersForInlay(argument.functionName,
                                                          paramsOut) &&
                   !paramsOut.empty()) {
          updateInlayParameterCache(cacheKey, paramsOut);
          cacheIt =
              parameterCache.emplace(cacheKey, std::move(paramsOut)).first;
        } else {
          cacheIt = parameterCache
                        .emplace(cacheKey, std::vector<std::string>{})
                        .first;
        }
      } else {
        const HlslBuiltinSignature *builtinSig =
            lookupHlslBuiltinSignature(argument.functionName);
        if (builtinSig) {
          paramsOut = builtinSig->parameters;
          cacheIt =
              parameterCache.emplace(cacheKey, std::move(paramsOut)).first;
        } else if (tryGetInlayParameterCache(cacheKey, paramsOut)) {
          cacheIt =
              parameterCache.emplace(cacheKey, std::move(paramsOut)).first;
        } else {
          if (resolveFunctionParameters(uri, argument.functionName, ctx,
                                        paramsOut) &&
              !paramsOut.empty()) {
            updateInlayParameterCache(cacheKey, paramsOut);
            cacheIt =
                parameterCache.emplace(cacheKey, std::move(paramsOut)).first;
          } else {
            cacheIt = parameterCache
                          .emplace(cacheKey, std::vector<std::string>{})
                          .first;
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
    if (!offsetToPosition(text, lineStarts, argument.argumentStartOffset, line,
                          character))
      continue;

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

  writeResponse(id, hints);
  if (kEnableInlayHintsSlowPath && !slowResolveSet.empty()) {
    std::vector<std::string> cacheKeys(slowResolveSet.begin(),
                                       slowResolveSet.end());
    scheduleInlaySlowResolve(uri, slowEpoch, doc->version,
                             std::move(cacheKeys), ctx);
  }
  return true;
}

static bool handleSemanticTokensFullRequest(const std::string &method,
                                            const Json &id, const Json *params,
                                            ServerRequestContext &ctx,
                                            const std::vector<std::string> &,
                                            const std::vector<std::string> &) {
  if (method != "textDocument/semanticTokens/full" || !params)
    return false;
  if (!ctx.semanticTokensEnabled) {
    writeResponse(id, makeNull());
    return true;
  }

  const Json *textDocument = getObjectValue(*params, "textDocument");
  if (!textDocument) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  if (!uriValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }
  writeResponse(id, buildSemanticTokensFull(doc->text, ctx.semanticLegend));
  return true;
}

static bool handleSemanticTokensRangeRequest(const std::string &method,
                                             const Json &id, const Json *params,
                                             ServerRequestContext &ctx,
                                             const std::vector<std::string> &,
                                             const std::vector<std::string> &) {
  if (method != "textDocument/semanticTokens/range" || !params)
    return false;
  if (!ctx.semanticTokensEnabled) {
    writeResponse(id, makeNull());
    return true;
  }

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *range = getObjectValue(*params, "range");
  if (!textDocument || !range) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *start = getObjectValue(*range, "start");
  const Json *end = getObjectValue(*range, "end");
  if (!uriValue || !start || !end) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *startLineValue = getObjectValue(*start, "line");
  const Json *startCharValue = getObjectValue(*start, "character");
  const Json *endLineValue = getObjectValue(*end, "line");
  const Json *endCharValue = getObjectValue(*end, "character");
  if (!startLineValue || !startCharValue || !endLineValue || !endCharValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }
  writeResponse(id, buildSemanticTokensRange(
                        doc->text,
                        static_cast<int>(getNumberValue(*startLineValue)),
                        static_cast<int>(getNumberValue(*startCharValue)),
                        static_cast<int>(getNumberValue(*endLineValue)),
                        static_cast<int>(getNumberValue(*endCharValue)),
                        ctx.semanticLegend));
  return true;
}

static bool definitionLocationEquals(const DefinitionLocation &a,
                                     const DefinitionLocation &b) {
  return a.uri == b.uri && a.line == b.line && a.start == b.start &&
         a.end == b.end;
}

static std::string
definitionLocationToString(const DefinitionLocation &location) {
  return location.uri + ":" + std::to_string(location.line) + ":" +
         std::to_string(location.start) + "-" + std::to_string(location.end);
}

static void emitDefinitionShadowCompareMismatch(
    const std::string &uri, const std::string &word, bool newPathFound,
    const DefinitionLocation &newPathLocation, bool oldPathFound,
    const DefinitionLocation &oldPathLocation) {
  Json params = makeObject();
  params.o["type"] = makeNumber(4);
  std::string message = "nsf definition shadowCompare mismatch: uri=" + uri +
                        " symbol=" + word + " newPath=";
  if (newPathFound) {
    message += definitionLocationToString(newPathLocation);
  } else {
    message += "none";
  }
  message += " oldPath=";
  if (oldPathFound) {
    message += definitionLocationToString(oldPathLocation);
  } else {
    message += "none";
  }
  params.o["message"] = makeString(message);
  writeNotification("window/logMessage", params);
}

static bool handleDefinitionRequest(const std::string &method, const Json &id,
                                    const Json *params,
                                    ServerRequestContext &ctx,
                                    const std::vector<std::string> &,
                                    const std::vector<std::string> &) {
  if (method != "textDocument/definition" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  if (!textDocument || !position) {
    writeResponse(id, makeArray());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *lineValue = getObjectValue(*position, "line");
  const Json *charValue = getObjectValue(*position, "character");
  if (!uriValue || !lineValue || !charValue) {
    writeResponse(id, makeArray());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  int line = static_cast<int>(getNumberValue(*lineValue));
  int character = static_cast<int>(getNumberValue(*charValue));
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeArray());
    return true;
  }
  std::string lineText = getLineAt(doc->text, line);
  std::string includePath;
  if (extractIncludePath(lineText, includePath)) {
    auto candidates =
        resolveIncludeCandidates(uri, includePath, ctx.workspaceFolders,
                                 ctx.includePaths, ctx.shaderExtensions);
    for (const auto &candidate : candidates) {
#ifdef _WIN32
      struct _stat statBuffer;
      if (_stat(candidate.c_str(), &statBuffer) == 0) {
        Json locations = makeArray();
        locations.a.push_back(makeLocation(pathToUri(candidate)));
        writeResponse(id, locations);
        includePath.clear();
        break;
      }
#endif
    }
    if (includePath.empty())
      return true;
  }
  std::string word = extractWordAt(lineText, character);
  if (word.empty()) {
    writeResponse(id, makeArray());
    return true;
  }
  DefinitionLocation newPathDefinition;
  std::unordered_set<std::string> visited;
  const bool newPathFound = findDefinitionInIncludeGraph(
      uri, word, ctx.documentSnapshot(), ctx.workspaceFolders, ctx.includePaths,
      ctx.shaderExtensions, visited, newPathDefinition);
  DefinitionLocation oldPathDefinition;
  bool oldPathFound = false;
  if (kEnableSemanticCacheShadowCompare) {
    std::unordered_set<std::string> oldVisited;
    oldPathFound = findDefinitionInIncludeGraphLegacy(
        uri, word, ctx.documentSnapshot(), ctx.workspaceFolders,
        ctx.includePaths, ctx.shaderExtensions, oldVisited, oldPathDefinition);
  }
  if (kEnableSemanticCacheShadowCompare &&
      (newPathFound != oldPathFound ||
       (newPathFound &&
        !definitionLocationEquals(newPathDefinition, oldPathDefinition)))) {
    emitDefinitionShadowCompareMismatch(uri, word, newPathFound,
                                        newPathDefinition, oldPathFound,
                                        oldPathDefinition);
  }
  if (newPathFound) {
    Json locations = makeArray();
    locations.a.push_back(
        makeLocationRange(newPathDefinition.uri, newPathDefinition.line,
                          newPathDefinition.start, newPathDefinition.end));
    writeResponse(id, locations);
    return true;
  }
  {
    SymbolDefinitionResolveOptions resolveOptions;
    resolveOptions.order =
        SymbolDefinitionSearchOrder::MacroThenWorkspaceThenScan;
    resolveOptions.allowWorkspaceScan = true;
    resolveOptions.useWorkspaceScanCache = true;
    resolveOptions.includeDocumentDirectoryInScan = true;
    resolveOptions.excludeUsfUshInScan = true;
    ResolvedSymbolDefinitionTarget resolvedTarget;
    if (resolveSymbolDefinitionTarget(uri, word, ctx, resolveOptions,
                                      resolvedTarget)) {
      Json locations = makeArray();
      locations.a.push_back(makeLocationRange(resolvedTarget.location.uri,
                                              resolvedTarget.location.line,
                                              resolvedTarget.location.start,
                                              resolvedTarget.location.end));
      writeResponse(id, locations);
      return true;
    }
  }

  auto occurrences = findOccurrences(doc->text, word);
  if (occurrences.size() == 1) {
    std::string occurrenceLineText =
        getLineAt(doc->text, occurrences.front().line);
    size_t declPos = 0;
    if (findMacroDefinitionInLine(occurrenceLineText, word, declPos) ||
        findDeclaredIdentifierInDeclarationLine(occurrenceLineText, word,
                                                declPos)) {
      int start = byteOffsetInLineToUtf16(occurrenceLineText,
                                          static_cast<int>(declPos));
      int end = start + static_cast<int>(word.size());
      Json locations = makeArray();
      locations.a.push_back(
          makeLocationRange(uri, occurrences.front().line, start, end));
      writeResponse(id, locations);
      return true;
    }
  }
  writeResponse(id, makeArray());
  return true;
}

static std::string formatFileLineDisplay(const std::string &uri, int line,
                                         const std::string &currentUri) {
  int oneBased = line >= 0 ? (line + 1) : 0;
  if (uri == currentUri) {
    return std::string("current-file:") + std::to_string(oneBased);
  }
  std::string path = uriToPath(uri);
  if (path.empty())
    path = uri;
  std::string base = std::filesystem::path(path).filename().string();
  if (base.empty())
    base = path;
  return base + ":" + std::to_string(oneBased);
}

static bool tryExtractIncludeSpan(const std::string &lineText, int cursorChar,
                                  std::string &includePathOut) {
  includePathOut.clear();
  if (cursorChar < 0)
    return false;
  size_t includePos = lineText.find("#include");
  if (includePos == std::string::npos)
    return false;

  size_t start = lineText.find('"', includePos);
  size_t end = start != std::string::npos ? lineText.find('"', start + 1)
                                          : std::string::npos;
  if (start != std::string::npos && end != std::string::npos && end > start) {
    int startChar = byteOffsetInLineToUtf16(lineText, static_cast<int>(start));
    int endChar = byteOffsetInLineToUtf16(lineText, static_cast<int>(end));
    if (cursorChar >= startChar && cursorChar <= endChar) {
      includePathOut = lineText.substr(start + 1, end - start - 1);
      return !includePathOut.empty();
    }
    return false;
  }

  start = lineText.find('<', includePos);
  end = start != std::string::npos ? lineText.find('>', start + 1)
                                   : std::string::npos;
  if (start != std::string::npos && end != std::string::npos && end > start) {
    int startChar = byteOffsetInLineToUtf16(lineText, static_cast<int>(start));
    int endChar = byteOffsetInLineToUtf16(lineText, static_cast<int>(end));
    if (cursorChar >= startChar && cursorChar <= endChar) {
      includePathOut = lineText.substr(start + 1, end - start - 1);
      return !includePathOut.empty();
    }
    return false;
  }
  return false;
}

static bool isSwizzleToken(const std::string &text) {
  if (text.empty() || text.size() > 4)
    return false;
  for (char ch : text) {
    if (ch != 'x' && ch != 'y' && ch != 'z' && ch != 'w' && ch != 'r' &&
        ch != 'g' && ch != 'b' && ch != 'a')
      return false;
  }
  return true;
}

static std::string trimCopy(const std::string &value) {
  return trimRightCopy(trimLeftCopy(value));
}

static std::string extractReturnTypeFromFunctionLabel(const std::string &label,
                                                      const std::string &name) {
  size_t namePos = label.rfind(name);
  if (namePos == std::string::npos)
    return "";
  size_t after = namePos + name.size();
  while (after < label.size() &&
         std::isspace(static_cast<unsigned char>(label[after]))) {
    after++;
  }
  if (after >= label.size() || label[after] != '(')
    return "";
  return trimCopy(label.substr(0, namePos));
}

static std::string swizzleResultType(const std::string &baseType,
                                     size_t swizzleLen) {
  std::string scalar = "float";
  if (baseType.rfind("half", 0) == 0)
    scalar = "half";
  else if (baseType.rfind("double", 0) == 0)
    scalar = "double";
  if (swizzleLen <= 1)
    return scalar;
  return scalar + std::to_string(swizzleLen);
}

static bool findLocalFunctionDeclarationUpTo(const std::string &text,
                                             const std::string &word,
                                             size_t maxOffset, int &lineOut,
                                             int &nameCharOut) {
  lineOut = -1;
  nameCharOut = 0;
  if (word.empty())
    return false;
  size_t lineStartOffset = 0;
  std::istringstream stream(text);
  std::string line;
  int lineIndex = 0;
  bool found = false;
  while (std::getline(stream, line)) {
    if (lineStartOffset >= maxOffset)
      break;
    auto tokens = lexLineTokens(line);
    if (!tokens.empty() && tokens[0].kind == LexToken::Kind::Identifier) {
      const std::string &first = tokens[0].text;
      if (first == "return" || first == "if" || first == "for" ||
          first == "while" || first == "switch") {
        lineStartOffset += line.size() + 1;
        lineIndex++;
        continue;
      }
    }
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
      const auto &tok = tokens[i];
      if (tok.kind != LexToken::Kind::Identifier || tok.text != word)
        continue;
      if (tokens[i + 1].kind != LexToken::Kind::Punct ||
          tokens[i + 1].text != "(")
        continue;
      if (i > 0 && tokens[i - 1].kind == LexToken::Kind::Punct &&
          (tokens[i - 1].text == "." || tokens[i - 1].text == "->" ||
           tokens[i - 1].text == "::"))
        continue;
      bool hasTypePrefix = false;
      bool hasAssignBefore = false;
      for (size_t j = 0; j < i; j++) {
        if (tokens[j].kind == LexToken::Kind::Punct &&
            (tokens[j].text == "=" || tokens[j].text == ":")) {
          hasAssignBefore = true;
          break;
        }
        if (tokens[j].kind == LexToken::Kind::Identifier &&
            !isQualifierToken(tokens[j].text)) {
          hasTypePrefix = true;
        }
      }
      if (!hasTypePrefix || hasAssignBefore)
        continue;
      lineOut = lineIndex;
      nameCharOut = byteOffsetInLineToUtf16(line, static_cast<int>(tok.start));
      found = true;
    }
    lineStartOffset += line.size() + 1;
    lineIndex++;
  }
  return found;
}

static void collectMemberNamesFromTextRecursive(
    const std::string &baseUri, const std::string &text,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions, int depth,
    std::unordered_set<std::string> &visitedUris,
    std::unordered_set<std::string> &seenNames,
    std::vector<std::string> &outFields) {
  if (depth <= 0)
    return;
  if (!visitedUris.insert(baseUri).second)
    return;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    std::string code = line;
    size_t lineComment = code.find("//");
    if (lineComment != std::string::npos)
      code = code.substr(0, lineComment);
    std::string includePath;
    if (extractIncludePath(code, includePath)) {
      auto candidates =
          resolveIncludeCandidates(baseUri, includePath, workspaceFolders,
                                   includePaths, shaderExtensions);
      std::vector<std::string> candidateUris;
      candidateUris.reserve(candidates.size());
      for (const auto &candidate : candidates)
        candidateUris.push_back(pathToUri(candidate));
      prefetchDocumentTexts(candidateUris, documents);
      for (const auto &candidate : candidates) {
        std::string candidateUri = pathToUri(candidate);
        std::string candidateText;
        if (!loadDocumentText(candidateUri, documents, candidateText))
          continue;
        collectMemberNamesFromTextRecursive(
            candidateUri, candidateText, documents, workspaceFolders,
            includePaths, shaderExtensions, depth - 1, visitedUris, seenNames,
            outFields);
        break;
      }
      continue;
    }
    auto names = extractDeclaredNamesFromLine(code);
    for (const auto &n : names) {
      if (!seenNames.insert(n).second)
        continue;
      outFields.push_back(n);
    }
    if (outFields.size() >= 512)
      return;
  }
}

static bool collectStructFieldsFromTextWithInlineIncludes(
    const std::string &baseUri, const std::string &text,
    const std::string &structName,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::vector<std::string> &fieldsOut) {
  fieldsOut.clear();
  std::unordered_set<std::string> seen;
  std::istringstream stream(text);
  std::string line;
  bool inTargetStruct = false;
  int braceDepth = 0;
  while (std::getline(stream, line)) {
    std::string code = line;
    size_t lineComment = code.find("//");
    if (lineComment != std::string::npos)
      code = code.substr(0, lineComment);
    if (!inTargetStruct) {
      std::string name;
      if (extractStructNameInLine(code, name) && name == structName) {
        inTargetStruct = true;
        if (code.find('{') != std::string::npos)
          braceDepth = 1;
        else
          braceDepth = 0;
      }
      continue;
    }

    if (braceDepth == 0) {
      if (code.find('{') != std::string::npos)
        braceDepth = 1;
      continue;
    }

    if (braceDepth == 1) {
      std::string includePath;
      if (extractIncludePath(code, includePath)) {
        auto candidates =
            resolveIncludeCandidates(baseUri, includePath, workspaceFolders,
                                     includePaths, shaderExtensions);
        std::vector<std::string> candidateUris;
        candidateUris.reserve(candidates.size());
        for (const auto &candidate : candidates)
          candidateUris.push_back(pathToUri(candidate));
        prefetchDocumentTexts(candidateUris, documents);
        for (const auto &candidate : candidates) {
          std::string candidateUri = pathToUri(candidate);
          std::string candidateText;
          if (!loadDocumentText(candidateUri, documents, candidateText))
            continue;
          std::unordered_set<std::string> visited;
          collectMemberNamesFromTextRecursive(
              candidateUri, candidateText, documents, workspaceFolders,
              includePaths, shaderExtensions, 12, visited, seen, fieldsOut);
          break;
        }
      } else {
        auto names = extractDeclaredNamesFromLine(code);
        for (const auto &n : names) {
          if (!seen.insert(n).second)
            continue;
          fieldsOut.push_back(n);
        }
      }
    }

    for (char ch : code) {
      if (ch == '{')
        braceDepth++;
      else if (ch == '}') {
        braceDepth--;
        if (braceDepth <= 0)
          return !fieldsOut.empty();
      }
    }
    if (fieldsOut.size() >= 512)
      return true;
  }
  return false;
}

static bool
findStructMemberDeclarationAtOrAfterLine(const std::string &text, int startLine,
                                         const std::string &memberName,
                                         int &lineOut, int &minCharacterOut) {
  lineOut = -1;
  minCharacterOut = 0;
  if (memberName.empty())
    return false;

  std::istringstream stream(text);
  std::string line;
  int lineIndex = 0;
  bool inBlockComment = false;
  bool inString = false;
  bool bodyStarted = false;
  int braceDepth = 0;

  while (std::getline(stream, line)) {
    if (lineIndex < startLine) {
      lineIndex++;
      continue;
    }

    if (bodyStarted && braceDepth == 1) {
      size_t pos = 0;
      if (findDeclaredIdentifierInDeclarationLine(line, memberName, pos)) {
        lineOut = lineIndex;
        int endByte = static_cast<int>(pos + memberName.size());
        minCharacterOut = byteOffsetInLineToUtf16(line, endByte);
        return true;
      }
    }

    bool inLineComment = false;
    for (size_t i = 0; i < line.size(); i++) {
      char ch = line[i];
      char next = i + 1 < line.size() ? line[i + 1] : '\0';
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
        if (ch == '"' && (i == 0 || line[i - 1] != '\\'))
          inString = false;
        continue;
      }
      if (ch == '/' && next == '/') {
        inLineComment = true;
        break;
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
        if (!bodyStarted) {
          bodyStarted = true;
          braceDepth = 1;
        } else {
          braceDepth++;
        }
      } else if (ch == '}' && bodyStarted) {
        braceDepth--;
        if (braceDepth <= 0)
          return false;
      }
    }

    lineIndex++;
  }
  return false;
}

static bool handleHoverRequest(const std::string &method, const Json &id,
                               const Json *params, ServerRequestContext &ctx,
                               const std::vector<std::string> &keywords,
                               const std::vector<std::string> &) {
  if (method != "textDocument/hover" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  if (!textDocument || !position) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *lineValue = getObjectValue(*position, "line");
  const Json *charValue = getObjectValue(*position, "character");
  if (!uriValue || !lineValue || !charValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  int line = static_cast<int>(getNumberValue(*lineValue));
  int character = static_cast<int>(getNumberValue(*charValue));
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string lineText = getLineAt(doc->text, line);
  size_t cursorOffset = positionToOffset(doc->text, line, character);

  {
    std::string includePath;
    if (tryExtractIncludeSpan(lineText, character, includePath)) {
      auto candidates =
          resolveIncludeCandidates(uri, includePath, ctx.workspaceFolders,
                                   ctx.includePaths, ctx.shaderExtensions);
      std::string resolved;
      for (const auto &candidate : candidates) {
#ifdef _WIN32
        struct _stat statBuffer;
        if (_stat(candidate.c_str(), &statBuffer) == 0) {
          resolved =
              std::filesystem::path(candidate).lexically_normal().string();
          break;
        }
#endif
      }
      std::string md;
      md += formatCppCodeBlock("#include \"" + includePath + "\"");
      if (!resolved.empty()) {
        md += "\n\nResolved path: ";
        md += resolved;
      }
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  std::string word = extractWordAt(lineText, character);
  if (word.empty()) {
    writeResponse(id, makeNull());
    return true;
  }

  std::string base;
  std::string member;
  if (extractMemberAccessAtOffset(doc->text, cursorOffset, base, member)) {
    MemberAccessBaseTypeOptions baseOptions;
    baseOptions.includeWorkspaceIndexFallback = true;
    MemberAccessBaseTypeResult baseResolution =
        resolveMemberAccessBaseType(uri, *doc, base, cursorOffset, ctx,
                                    baseOptions);
    std::string baseType = baseResolution.typeName;
    if (baseType.empty())
      baseType = base;

    if (word == base) {
      HoverSymbolMarkdownInput symbolInput;
      symbolInput.code =
          (baseType.empty() ? std::string() : (baseType + " ")) + base;
      symbolInput.notes.push_back("(Member access base)");
      symbolInput.typeName = baseType;
      DeclCandidate decl;
      if (findBestDeclarationUpTo(doc->text, base, cursorOffset, decl) &&
          decl.found) {
        if (decl.braceDepth > 0) {
          symbolInput.notes.push_back("(Local variable)");
        }
        symbolInput.definedAt = formatFileLineDisplay(uri, decl.line, uri);
      }
      std::string md = renderHoverSymbolMarkdown(symbolInput);
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }

    if (isSwizzleToken(word)) {
      std::string resultType = swizzleResultType(baseType, word.size());
      std::string md;
      md += formatCppCodeBlock(resultType + " " + base + "." + word);
      md += "\n\n(Swizzle) Base: ";
      md += base;
      md += " (";
      md += baseType;
      md += ")\n\nType: ";
      md += resultType;
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }

    {
      std::string md;
      if (formatHlslBuiltinMethodMarkdown(word, baseType, md)) {
        Json hover = makeObject();
        hover.o["contents"] = makeMarkup(md);
        writeResponse(id, hover);
        return true;
      }
    }

    MemberHoverInfo memberHoverInfo;
    resolveMemberHoverInfo(uri, baseType, word, ctx, memberHoverInfo);
    std::string md;
    if (memberHoverInfo.found) {
      md += formatCppCodeBlock(memberHoverInfo.memberType + " " + word);
      md += "\n\n(Field) Owner: ";
      md += baseType;
      md += "\n\nType: ";
      md += memberHoverInfo.memberType;
      if (memberHoverInfo.hasStructLocation) {
        md += "\n\nDefined at: ";
        md += formatFileLineDisplay(memberHoverInfo.ownerStructLocation.uri,
                                    memberHoverInfo.ownerStructLocation.line,
                                    uri);
        if (!memberHoverInfo.memberLeadingDoc.empty()) {
          md += "\n\n";
          md += memberHoverInfo.memberLeadingDoc;
        }
        if (!memberHoverInfo.memberInlineDoc.empty()) {
          md += "\n\n";
          md += memberHoverInfo.memberInlineDoc;
        }
      }
    } else {
      md += formatCppCodeBlock(word);
    }
    Json hover = makeObject();
    hover.o["contents"] = makeMarkup(md);
    writeResponse(id, hover);
    return true;
  }

  {
    std::string md;
    if (formatHlslSystemSemanticMarkdown(word, md)) {
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    std::string md;
    if (formatHlslDirectiveMarkdown(word, md)) {
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    std::string md;
    if (formatHlslKeywordMarkdown(word, md)) {
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  const HlslBuiltinSignature *builtinSig = lookupHlslBuiltinSignature(word);
  if (builtinSig) {
    std::string md;
    md += formatCppCodeBlock(builtinSig->label.empty() ? word
                                                       : builtinSig->label);
    md += "\n\n(HLSL built-in function)";
    if (!builtinSig->documentation.empty()) {
      md += "\n\n";
      md += builtinSig->documentation;
    }
    Json hover = makeObject();
    hover.o["contents"] = makeMarkup(md);
    writeResponse(id, hover);
    return true;
  }

  const std::string *builtinDoc = lookupHlslBuiltinDoc(word);
  if (builtinDoc) {
    std::string md;
    md += formatCppCodeBlock("ret " + word + "(...)");
    md += "\n\n(HLSL built-in function)\n\n";
    md += *builtinDoc;
    Json hover = makeObject();
    hover.o["contents"] = makeMarkup(md);
    writeResponse(id, hover);
    return true;
  }

  bool looksLikeCall = false;
  {
    std::string callName;
    CallSiteKind callKind = CallSiteKind::FunctionCall;
    if (detectCallLikeCalleeAtOffset(doc->text, cursorOffset, callName,
                                     callKind) &&
        callName == word && callKind == CallSiteKind::FunctionCall) {
      looksLikeCall = true;
    }
  }
  if (looksLikeCall) {
    int localDeclLine = -1;
    int localDeclChar = 0;
    if (findLocalFunctionDeclarationUpTo(doc->text, word, cursorOffset,
                                         localDeclLine, localDeclChar)) {
      std::string label;
      std::vector<std::string> paramsOut;
      const bool fastSig = queryFastAstFunctionSignature(
          uri, doc->text, doc->epoch, word, localDeclLine, localDeclChar, label,
          paramsOut);
      if ((fastSig ||
           extractFunctionSignatureAt(doc->text, localDeclLine, localDeclChar,
                                      word, label, paramsOut)) &&
          !label.empty()) {
        HoverFunctionMarkdownInput functionInput;
        functionInput.code = label;
        functionInput.kindLabel = "(HLSL function)";
        functionInput.returnType =
            extractReturnTypeFromFunctionLabel(label, word);
        functionInput.parameters = paramsOut;
        functionInput.definedAt = formatFileLineDisplay(uri, localDeclLine, uri);
        functionInput.leadingDoc =
            extractLeadingDocumentationAtLine(doc->text, localDeclLine);
        functionInput.inlineDoc =
            extractTrailingInlineCommentAtLine(doc->text, localDeclLine, 0);
        std::string md = renderHoverFunctionMarkdown(functionInput);
        Json hover = makeObject();
        hover.o["contents"] = makeMarkup(md);
        writeResponse(id, hover);
        return true;
      }
    }
  }

  std::vector<IndexedDefinition> defs;
  if (workspaceIndexFindDefinitions(word, defs, 64)) {
    std::vector<IndexedDefinition> funcDefs;
    std::vector<IndexedDefinition> macroDefs;
    std::vector<IndexedDefinition> structDefs;
    for (const auto &d : defs) {
      if (d.kind == 12)
        funcDefs.push_back(d);
      else if (d.kind == 14)
        macroDefs.push_back(d);
      else if (d.kind == 23)
        structDefs.push_back(d);
    }

    if (!funcDefs.empty()) {
      IndexedDefinition primary = funcDefs.front();
      struct HoverFunctionCandidate {
        std::string label;
        std::vector<std::string> params;
        IndexedDefinition def;
      };
      std::vector<HoverFunctionCandidate> labels;
      labels.reserve(funcDefs.size());
      std::unordered_set<std::string> seen;
      seen.reserve(funcDefs.size());
      for (const auto &d : funcDefs) {
        std::string defText;
        if (!ctx.readDocumentText(d.uri, defText))
          continue;
        std::string label;
        std::vector<std::string> paramsOut;
        uint64_t dEpoch = 0;
        const Document *dDoc = ctx.findDocument(d.uri);
        if (dDoc)
          dEpoch = dDoc->epoch;
        const bool fastSig = queryFastAstFunctionSignature(
            d.uri, defText, dEpoch, word, d.line, d.start, label, paramsOut);
        if ((!fastSig && !extractFunctionSignatureAt(defText, d.line, d.start,
                                                     word, label, paramsOut)) ||
            label.empty()) {
          label = word + "(...)";
          paramsOut.clear();
        } else if (!d.type.empty()) {
          std::string trimmedLabel = trimLeftCopy(label);
          if (trimmedLabel.rfind(word, 0) == 0)
            label = d.type + " " + label;
        }
        std::string key;
        key.reserve(label.size() + d.uri.size() + 64);
        key.append(label);
        key.push_back('|');
        key.append(d.uri);
        key.push_back('|');
        key.append(std::to_string(d.line));
        key.push_back('|');
        key.append(std::to_string(d.start));
        key.push_back('|');
        key.append(std::to_string(d.end));
        if (!seen.insert(key).second)
          continue;
        labels.push_back(HoverFunctionCandidate{label, paramsOut, d});
      }
      size_t selectedLabelIndex = 0;
      if (looksLikeCall && kEnableOverloadResolver && labels.size() > 1) {
        std::vector<CandidateSignature> resolverCandidates;
        resolverCandidates.reserve(labels.size());
        for (const auto &item : labels) {
          CandidateSignature candidate;
          candidate.name = word;
          candidate.displayLabel = item.label;
          candidate.displayParams = item.params;
          candidate.sourceUri = item.def.uri;
          candidate.sourceLine = item.def.line;
          candidate.visibilityCondition = "";
          candidate.params.reserve(item.params.size());
          for (const auto &param : item.params) {
            ParamDesc desc;
            desc.name = extractParameterName(param);
            desc.type = parseParamTypeDescFromDecl(param);
            candidate.params.push_back(std::move(desc));
          }
          resolverCandidates.push_back(std::move(candidate));
        }
        std::vector<TypeDesc> argumentTypes;
        inferCallArgumentTypesAtCursor(uri, doc->text, cursorOffset,
                                       argumentTypes);
        ResolveCallContext resolveContext;
        resolveContext.defines = ctx.preprocessorDefines;
        resolveContext.allowNarrowing = false;
        resolveContext.enableVisibilityFiltering = true;
        resolveContext.allowPartialArity = true;
        ResolveCallResult resolveResult = resolveCallCandidates(
            resolverCandidates, argumentTypes, resolveContext);
        recordOverloadResolverResult(resolveResult);
        if (!resolveResult.rankedCandidates.empty() &&
            (resolveResult.status == ResolveCallStatus::Resolved ||
             resolveResult.status == ResolveCallStatus::Ambiguous)) {
          const int idx = resolveResult.rankedCandidates.front().candidateIndex;
          if (idx >= 0 && static_cast<size_t>(idx) < labels.size())
            selectedLabelIndex = static_cast<size_t>(idx);
        }
      }
      std::string primaryLabel =
          labels.empty() ? (word + "(...)") : labels[selectedLabelIndex].label;
      std::vector<std::string> primaryParams;
      if (!labels.empty()) {
        primary = labels[selectedLabelIndex].def;
        primaryParams = labels[selectedLabelIndex].params;
      }

      std::string primaryDoc;
      std::string primaryInlineDoc;
      std::string defText;
      if (ctx.readDocumentText(primary.uri, defText)) {
        primaryDoc = extractLeadingDocumentationAtLine(defText, primary.line);
        primaryInlineDoc = extractTrailingInlineCommentAtLine(
            defText, primary.line, primary.end);
      }

      HoverFunctionMarkdownInput functionInput;
      functionInput.code = primaryLabel;
      functionInput.kindLabel = "(HLSL function)";
      functionInput.returnType =
          primary.type.empty()
              ? extractReturnTypeFromFunctionLabel(primaryLabel, word)
              : primary.type;
      functionInput.parameters = primaryParams;
      functionInput.definedAt =
          formatFileLineDisplay(primary.uri, primary.line, uri);
      functionInput.leadingDoc = primaryDoc;
      functionInput.inlineDoc = primaryInlineDoc;
      if (!looksLikeCall && labels.size() > 1) {
        functionInput.selectionNote =
            "(Multiple candidates) Unable to select best overload reliably.";
      }
      if (labels.size() > 1) {
        functionInput.listTitle = "Overloads";
        size_t shown = 0;
        for (size_t i = 0; i < labels.size() && shown < 50; i++) {
          HoverLocationListItem item;
          item.label = labels[i].label;
          item.locationDisplay =
              formatFileLineDisplay(labels[i].def.uri, labels[i].def.line, uri);
          functionInput.listItems.push_back(std::move(item));
          shown++;
        }
        functionInput.appendEllipsisAfterList = labels.size() > shown;
      }
      std::string md = renderHoverFunctionMarkdown(functionInput);
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }

    if (!macroDefs.empty() && funcDefs.empty()) {
      IndexedDefinition d = macroDefs.front();
      std::string defText;
      HoverMacroMarkdownInput macroInput;
      if (ctx.readDocumentText(d.uri, defText)) {
        std::string macroLine = getLineAt(defText, d.line);
        std::string macroBlock = macroLine;
        int nextLine = d.line + 1;
        while (true) {
          std::string trimmed = trimRightCopy(macroLine);
          if (trimmed.empty() || trimmed.back() != '\\')
            break;
          macroLine = getLineAt(defText, nextLine);
          if (macroLine.empty())
            break;
          macroBlock += "\n";
          macroBlock += macroLine;
          nextLine++;
          if (nextLine - d.line > 32)
            break;
        }
        macroInput.code = macroBlock;
        macroInput.definedAt = formatFileLineDisplay(d.uri, d.line, uri);
        macroInput.leadingDoc =
            extractLeadingDocumentationAtLine(defText, d.line);
        macroInput.inlineDoc =
            extractTrailingInlineCommentAtLine(defText, d.line, d.end);
      } else {
        macroInput.code = "#define " + word;
      }
      if (macroDefs.size() > 1) {
        macroInput.listTitle = "Definitions";
        size_t shown = 0;
        for (size_t i = 0; i < macroDefs.size() && shown < 50; i++) {
          HoverLocationListItem item;
          item.label = "#define " + macroDefs[i].name;
          item.locationDisplay =
              formatFileLineDisplay(macroDefs[i].uri, macroDefs[i].line, uri);
          macroInput.listItems.push_back(std::move(item));
          shown++;
        }
        macroInput.appendEllipsisAfterList = macroDefs.size() > shown;
      }
      std::string md = renderHoverMacroMarkdown(macroInput);
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }

    if (!structDefs.empty()) {
      IndexedDefinition d = structDefs.front();
      std::string md;
      md += formatCppCodeBlock("struct " + word);
      md += "\n\nDefined at: ";
      md += formatFileLineDisplay(d.uri, d.line, uri);
      std::vector<std::string> fields;
      if (workspaceIndexGetStructFields(word, fields) && !fields.empty()) {
        md += "\n\nMembers:";
        size_t shown = 0;
        for (size_t i = 0; i < fields.size() && shown < 256; i++) {
          md += "\n- `";
          std::string t;
          workspaceIndexGetStructMemberType(word, fields[i], t);
          if (!t.empty()) {
            md += t;
            md += " ";
          }
          md += fields[i];
          md += "`";
          shown++;
        }
        if (fields.size() > shown)
          md += "\n- `...`";
      }
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    DefinitionLocation structLoc;
    if (workspaceIndexFindStructDefinition(word, structLoc)) {
      std::string md;
      md += formatCppCodeBlock("struct " + word);
      md += "\n\nDefined at: ";
      md += formatFileLineDisplay(structLoc.uri, structLoc.line, uri);
      std::vector<std::string> fields;
      if (workspaceIndexGetStructFields(word, fields) && !fields.empty()) {
        md += "\n\nMembers:";
        size_t shown = 0;
        for (size_t i = 0; i < fields.size() && shown < 256; i++) {
          md += "\n- `";
          std::string t;
          workspaceIndexGetStructMemberType(word, fields[i], t);
          if (!t.empty()) {
            md += t;
            md += " ";
          }
          md += fields[i];
          md += "`";
          shown++;
        }
        if (fields.size() > shown)
          md += "\n- `...`";
      }
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    std::vector<std::string> fields;
    bool haveFields = workspaceIndexGetStructFields(word, fields);
    if (!haveFields || fields.empty()) {
      fields.clear();
      haveFields = collectStructFieldsFromTextWithInlineIncludes(
          uri, doc->text, word, ctx.documentSnapshot(), ctx.workspaceFolders,
          ctx.includePaths, ctx.shaderExtensions, fields);
    }
    if (!haveFields || fields.empty()) {
      fields.clear();
      haveFields = collectStructFieldsFromText(doc->text, word, fields);
    }
    if (!haveFields || fields.empty()) {
      fields.clear();
      std::unordered_set<std::string> visitedStructs;
      haveFields = collectStructFieldsInIncludeGraph(
          uri, word, ctx.documentSnapshot(), ctx.workspaceFolders,
          ctx.includePaths, ctx.shaderExtensions, visitedStructs, fields);
    }
    if (haveFields && !fields.empty()) {
      DefinitionLocation loc;
      bool hasLoc = workspaceIndexFindDefinition(word, loc);
      std::string md;
      md += formatCppCodeBlock("struct " + word);
      if (hasLoc) {
        md += "\n\nDefined at: ";
        md += formatFileLineDisplay(loc.uri, loc.line, uri);
      }
      md += "\n\nMembers:";
      std::unordered_set<std::string> seen;
      size_t shown = 0;
      for (size_t i = 0; i < fields.size() && shown < 256; i++) {
        if (!seen.insert(fields[i]).second)
          continue;
        md += "\n- `";
        std::string t;
        workspaceIndexGetStructMemberType(word, fields[i], t);
        if (!t.empty()) {
          md += t;
          md += " ";
        }
        md += fields[i];
        md += "`";
        shown++;
      }
      if (fields.size() > shown)
        md += "\n- `...`";
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    DeclCandidate decl;
    if (findBestDeclarationUpTo(doc->text, word, cursorOffset, decl) &&
        decl.found) {
      bool isParam = false;
      TypeEvalResult typeEval = resolveHoverTypeAtDeclaration(
          uri, *doc, word, cursorOffset, ctx, isParam);
      const std::string &typeName = typeEval.type;
      if (decl.braceDepth == 0 && !isParam) {
        int nameChar = byteOffsetInLineToUtf16(decl.lineText, decl.nameBytePos);
        std::string functionLabel;
        std::vector<std::string> functionParams;
        const bool fastSig = queryFastAstFunctionSignature(
            uri, doc->text, doc->epoch, word, decl.line, nameChar,
            functionLabel, functionParams);
        if ((fastSig ||
             extractFunctionSignatureAt(doc->text, decl.line, nameChar, word,
                                        functionLabel, functionParams)) &&
            !functionLabel.empty()) {
          HoverFunctionMarkdownInput functionInput;
          functionInput.code = functionLabel;
          functionInput.kindLabel = "(HLSL function)";
          functionInput.returnType =
              extractReturnTypeFromFunctionLabel(functionLabel, word);
          functionInput.parameters = functionParams;
          functionInput.definedAt =
              formatFileLineDisplay(uri, decl.line, uri);
          functionInput.leadingDoc =
              extractLeadingDocumentationAtLine(doc->text, decl.line);
          int endChar = byteOffsetInLineToUtf16(
              decl.lineText, static_cast<int>(decl.nameBytePos + word.size()));
          functionInput.inlineDoc =
              extractTrailingInlineCommentAtLine(doc->text, decl.line, endChar);
          std::string md = renderHoverFunctionMarkdown(functionInput);
          Json hover = makeObject();
          hover.o["contents"] = makeMarkup(md);
          writeResponse(id, hover);
          return true;
        }
      }
      std::string code = (typeName.empty() ? std::string() : (typeName + " "));
      code += word;
      HoverSymbolMarkdownInput symbolInput;
      symbolInput.code = code;
      if (decl.braceDepth > 0) {
        symbolInput.notes.push_back("(Local variable)");
      } else if (isParam) {
        symbolInput.notes.push_back("(Parameter)");
      }
      symbolInput.typeName = typeName;
      symbolInput.indeterminateReason = typeEval.reasonCode;
      symbolInput.definedAt = formatFileLineDisplay(uri, decl.line, uri);
      if (decl.braceDepth == 0 && !isParam) {
        int endByte = static_cast<int>(decl.nameBytePos + word.size());
        int endChar = byteOffsetInLineToUtf16(decl.lineText, endByte);
        symbolInput.inlineDoc =
            extractTrailingInlineCommentAtLine(doc->text, decl.line, endChar);
      }
      std::string md = renderHoverSymbolMarkdown(symbolInput);
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    SymbolDefinitionResolveOptions resolveOptions;
    resolveOptions.order =
        SymbolDefinitionSearchOrder::WorkspaceThenGraphThenMacro;
    resolveOptions.allowWorkspaceScan = false;
    ResolvedSymbolDefinitionTarget resolvedTarget;
    if (resolveSymbolDefinitionTarget(uri, word, ctx, resolveOptions,
                                      resolvedTarget)) {
      if (resolvedTarget.hasMacroGeneratedFunction) {
        const auto &macroFn = resolvedTarget.macroGeneratedFunction;
        HoverFunctionMarkdownInput functionInput;
        functionInput.code = macroFn.label;
        functionInput.kindLabel = "(HLSL function)";
        functionInput.returnType = macroFn.returnType;
        functionInput.parameters = macroFn.parameterDecls;
        functionInput.definedAt = formatFileLineDisplay(
            macroFn.definition.uri, macroFn.definition.line, uri);
        std::string defText;
        if (ctx.readDocumentText(macroFn.definition.uri, defText)) {
          functionInput.leadingDoc = extractLeadingDocumentationAtLine(
              defText, macroFn.definition.line);
          functionInput.inlineDoc = extractTrailingInlineCommentAtLine(
              defText, macroFn.definition.line, macroFn.definition.end);
        }
        std::string md = renderHoverFunctionMarkdown(functionInput);
        Json hover = makeObject();
        hover.o["contents"] = makeMarkup(md);
        writeResponse(id, hover);
        return true;
      }

      std::string typeName;
      workspaceIndexGetSymbolType(word, typeName);
      const DefinitionLocation &loc = resolvedTarget.location;
      std::string defText;
      if (ctx.readDocumentText(loc.uri, defText)) {
        std::string label;
        std::vector<std::string> paramsOut;
        uint64_t locEpoch = 0;
        const Document *locDoc = ctx.findDocument(loc.uri);
        if (locDoc) {
          locEpoch = locDoc->epoch;
        }
        const bool fastSig = queryFastAstFunctionSignature(
            loc.uri, defText, locEpoch, word, loc.line, loc.start, label,
            paramsOut);
        if ((fastSig || extractFunctionSignatureAt(defText, loc.line, loc.start,
                                                   word, label, paramsOut)) &&
            !label.empty()) {
          HoverFunctionMarkdownInput functionInput;
          functionInput.code = label;
          functionInput.kindLabel = "(HLSL function)";
          functionInput.returnType =
              extractReturnTypeFromFunctionLabel(label, word);
          functionInput.parameters = paramsOut;
          functionInput.definedAt =
              formatFileLineDisplay(loc.uri, loc.line, uri);
          functionInput.leadingDoc =
              extractLeadingDocumentationAtLine(defText, loc.line);
          functionInput.inlineDoc =
              extractTrailingInlineCommentAtLine(defText, loc.line, loc.end);
          std::string md = renderHoverFunctionMarkdown(functionInput);
          Json hover = makeObject();
          hover.o["contents"] = makeMarkup(md);
          writeResponse(id, hover);
          return true;
        }
      }

      HoverSymbolMarkdownInput symbolInput;
      symbolInput.code =
          (typeName.empty() ? std::string() : (typeName + " ")) + word;
      symbolInput.typeName = typeName;
      symbolInput.definedAt = formatFileLineDisplay(loc.uri, loc.line, uri);
      if (!defText.empty() || ctx.readDocumentText(loc.uri, defText)) {
        symbolInput.leadingDoc =
            extractLeadingDocumentationAtLine(defText, loc.line);
        symbolInput.inlineDoc =
            extractTrailingInlineCommentAtLine(defText, loc.line, loc.end);
      }
      std::string md = renderHoverSymbolMarkdown(symbolInput);
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    std::string md = formatCppCodeBlock(word);
    Json hover = makeObject();
    hover.o["contents"] = makeMarkup(md);
    writeResponse(id, hover);
    return true;
  }
}

static bool
handleCompletionRequest(const std::string &method, const Json &id,
                        const Json *params, ServerRequestContext &ctx,
                        const std::vector<std::string> &keywords,
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
    if (isAttributeCompletionContext(lineText, character)) {
      Json items = makeArray();
      appendCompletionItems(items, attributeKeywords, 14);
      writeResponse(id, items);
      return true;
    }
    size_t cursorOffset = positionToOffset(doc->text, line, character);
    std::string base;
    std::string member;
    if (extractMemberAccessAtOffset(doc->text, cursorOffset, base, member)) {
      MemberAccessBaseTypeOptions baseOptions;
      baseOptions.includeIncludeGraphFallback = true;
      MemberAccessBaseTypeResult baseResolution =
          resolveMemberAccessBaseType(uri, *doc, base, cursorOffset, ctx,
                                      baseOptions);
      if (baseResolution.resolved) {
        MemberCompletionQuery query;
        if (collectMemberCompletionQuery(uri, baseResolution.typeName, ctx,
                                         query)) {
          Json items = buildMemberCompletionItems(query);
          if (!items.a.empty()) {
            writeResponse(id, items);
            return true;
          }
        }
      }
    }
  }

  Json items = makeArray();
  std::unordered_set<std::string> seen;
  auto appendUniqueItems = [&](const std::vector<std::string> &labels,
                               int kind) {
    for (const auto &label : labels)
      appendUniqueCompletionItem(items, seen, label, kind);
  };
  appendUniqueItems(getHlslDirectiveCompletionItems(), 14);
  appendUniqueItems(getHlslKeywordNames(), 14);
  appendUniqueItems(getHlslScalarVectorMatrixTypeNames(), 7);
  appendUniqueItems(getTypeModelObjectTypeNames(), 7);
  appendUniqueItems(getHlslSystemSemanticNames(), 13);
  appendUniqueItems(getHlslBuiltinNames(), 3);
  if (doc && line >= 0 && character >= 0) {
    std::string lineText = getLineAt(doc->text, line);
    if (isAttributeCompletionContext(lineText, character)) {
      appendUniqueItems(attributeKeywords, 14);
    }
  }
  writeResponse(id, items);
  return true;
}

static bool handleReferencesRequest(const std::string &method, const Json &id,
                                    const Json *params,
                                    ServerRequestContext &ctx,
                                    const std::vector<std::string> &,
                                    const std::vector<std::string> &) {
  if (method != "textDocument/references" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  if (!textDocument || !position) {
    writeResponse(id, makeArray());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *lineValue = getObjectValue(*position, "line");
  const Json *charValue = getObjectValue(*position, "character");
  if (!uriValue || !lineValue || !charValue) {
    writeResponse(id, makeArray());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  int line = static_cast<int>(getNumberValue(*lineValue));
  int character = static_cast<int>(getNumberValue(*charValue));
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeArray());
    return true;
  }
  std::string lineText = getLineAt(doc->text, line);
  std::string word = extractWordAt(lineText, character);
  if (word.empty()) {
    writeResponse(id, makeArray());
    return true;
  }
  auto occurrences = collectOccurrencesForSymbol(
      uri, word, ctx.documentSnapshot(), ctx.workspaceFolders, ctx.includePaths,
      ctx.shaderExtensions);
  Json locations = makeArray();
  for (const auto &occ : occurrences) {
    locations.a.push_back(
        makeLocationRange(occ.uri, occ.line, occ.start, occ.end));
  }
  writeResponse(id, locations);
  return true;
}

static bool handlePrepareRenameRequest(const std::string &method,
                                       const Json &id, const Json *params,
                                       ServerRequestContext &ctx,
                                       const std::vector<std::string> &,
                                       const std::vector<std::string> &) {
  if (method != "textDocument/prepareRename" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  if (!textDocument || !position) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *lineValue = getObjectValue(*position, "line");
  const Json *charValue = getObjectValue(*position, "character");
  if (!uriValue || !lineValue || !charValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  int line = static_cast<int>(getNumberValue(*lineValue));
  int character = static_cast<int>(getNumberValue(*charValue));
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string lineText = getLineAt(doc->text, line);
  std::string word = extractWordAt(lineText, character);
  if (word.empty()) {
    writeResponse(id, makeNull());
    return true;
  }
  auto occs = findOccurrences(lineText, word);
  if (occs.empty()) {
    writeResponse(id, makeNull());
    return true;
  }
  Json range = makeRangeExact(line, occs.front().start, occs.front().end);
  Json result = makeObject();
  result.o["range"] = range;
  result.o["placeholder"] = makeString(word);
  writeResponse(id, result);
  return true;
}

static bool handleRenameRequest(const std::string &method, const Json &id,
                                const Json *params, ServerRequestContext &ctx,
                                const std::vector<std::string> &,
                                const std::vector<std::string> &) {
  if (method != "textDocument/rename" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  const Json *newNameValue = getObjectValue(*params, "newName");
  const Json *uriValue =
      textDocument ? getObjectValue(*textDocument, "uri") : nullptr;
  if (!textDocument || !position || !newNameValue || !uriValue) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *lineValue = getObjectValue(*position, "line");
  const Json *charValue = getObjectValue(*position, "character");
  if (!lineValue || !charValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  int line = static_cast<int>(getNumberValue(*lineValue));
  int character = static_cast<int>(getNumberValue(*charValue));
  std::string newName = getStringValue(*newNameValue);
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string lineText = getLineAt(doc->text, line);
  std::string word = extractWordAt(lineText, character);
  if (word.empty()) {
    writeResponse(id, makeNull());
    return true;
  }
  auto occurrences = collectOccurrencesForSymbol(
      uri, word, ctx.documentSnapshot(), ctx.workspaceFolders, ctx.includePaths,
      ctx.shaderExtensions);
  Json changes = makeObject();
  for (const auto &occ : occurrences) {
    Json edit = makeObject();
    edit.o["range"] = makeRangeExact(occ.line, occ.start, occ.end);
    edit.o["newText"] = makeString(newName);
    auto existing = changes.o.find(occ.uri);
    if (existing == changes.o.end()) {
      Json edits = makeArray();
      edits.a.push_back(std::move(edit));
      changes.o[occ.uri] = std::move(edits);
    } else {
      existing->second.a.push_back(std::move(edit));
    }
  }
  Json workspaceEdit = makeObject();
  workspaceEdit.o["changes"] = changes;
  writeResponse(id, workspaceEdit);
  return true;
}

static bool handleDocumentSymbolRequest(const std::string &method,
                                        const Json &id, const Json *params,
                                        ServerRequestContext &ctx,
                                        const std::vector<std::string> &,
                                        const std::vector<std::string> &) {
  if (method != "textDocument/documentSymbol" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  if (!textDocument) {
    writeResponse(id, makeArray());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  if (!uriValue) {
    writeResponse(id, makeArray());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeArray());
    return true;
  }
  Json symbols = makeArray();
  std::istringstream stream(doc->text);
  std::string lineText;
  int line = 0;
  while (std::getline(stream, lineText)) {
    auto pushSymbol = [&](const std::string &name, int kind, int startChar) {
      Json symbol = makeObject();
      symbol.o["name"] = makeString(name);
      symbol.o["kind"] = makeNumber(kind);
      symbol.o["range"] = makeRange(line, startChar);
      symbol.o["selectionRange"] = makeRange(line, startChar);
      symbol.o["children"] = makeArray();
      symbols.a.push_back(std::move(symbol));
    };
    auto trimStart = lineText.find_first_not_of(" \t");
    std::string view =
        trimStart == std::string::npos ? "" : lineText.substr(trimStart);
    auto tryPushDeclaration = [&](const std::string &prefix, int kind) {
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
      pushSymbol(
          lineText.substr(nameStart, nameEnd - nameStart), kind,
          byteOffsetInLineToUtf16(lineText, static_cast<int>(nameStart)));
      return true;
    };
    auto tryPushFunction = [&]() {
      if (trimStart == std::string::npos)
        return false;
      size_t openParen = view.find('(');
      size_t closeParen = openParen == std::string::npos
                              ? std::string::npos
                              : view.find(')', openParen + 1);
      if (openParen == std::string::npos || closeParen == std::string::npos ||
          openParen == 0) {
        return false;
      }
      std::string prefix = view.substr(0, openParen);
      while (!prefix.empty() &&
             std::isspace(static_cast<unsigned char>(prefix.back()))) {
        prefix.pop_back();
      }
      if (prefix.empty())
        return false;
      std::istringstream prefixStream(prefix);
      std::vector<std::string> tokens;
      std::string token;
      while (prefixStream >> token) {
        tokens.push_back(token);
      }
      if (tokens.size() < 2)
        return false;
      const std::string &firstToken = tokens.front();
      if (firstToken == "return" || firstToken == "if" || firstToken == "for" ||
          firstToken == "while" || firstToken == "switch") {
        return false;
      }
      const std::string &name = tokens.back();
      if (name.empty())
        return false;
      unsigned char firstChar = static_cast<unsigned char>(name[0]);
      if (!std::isalpha(firstChar) && name[0] != '_')
        return false;
      std::string suffix = view.substr(closeParen + 1);
      size_t suffixStart = suffix.find_first_not_of(" \t");
      suffix =
          suffixStart == std::string::npos ? "" : suffix.substr(suffixStart);
      if (!suffix.empty() && suffix[0] != ':' && suffix[0] != '{')
        return false;
      size_t nameStart = lineText.find(name, trimStart);
      if (nameStart == std::string::npos)
        return false;
      pushSymbol(
          name, 12,
          byteOffsetInLineToUtf16(lineText, static_cast<int>(nameStart)));
      return true;
    };
    if (!tryPushDeclaration("struct ", 23) &&
        !tryPushDeclaration("cbuffer ", 3) &&
        !tryPushDeclaration("technique ", 2) &&
        !tryPushDeclaration("pass ", 6)) {
      tryPushFunction();
    }
    line++;
  }
  writeResponse(id, symbols);
  return true;
}

bool handleCoreRequestMethods(const std::string &method, const Json &id,
                              const Json *params, ServerRequestContext &ctx,
                              const std::vector<std::string> &keywords,
                              const std::vector<std::string> &directives) {
  for (auto handler : kCoreRequestHandlers) {
    if (handler(method, id, params, ctx, keywords, directives))
      return true;
  }
  return false;
}

SignatureHelpMetricsSnapshot takeSignatureHelpMetricsSnapshot() {
  SignatureHelpMetricsSnapshot snapshot;
  snapshot.indeterminateTotal =
      gSignatureHelpIndeterminateTotal.exchange(0, std::memory_order_relaxed);
  snapshot.indeterminateReasonCallTargetUnknown =
      gSignatureHelpIndeterminateReasonCallTargetUnknown.exchange(
          0, std::memory_order_relaxed);
  snapshot.indeterminateReasonDefinitionTextUnavailable =
      gSignatureHelpIndeterminateReasonDefinitionTextUnavailable.exchange(
          0, std::memory_order_relaxed);
  snapshot.indeterminateReasonSignatureExtractFailed =
      gSignatureHelpIndeterminateReasonSignatureExtractFailed.exchange(
          0, std::memory_order_relaxed);
  snapshot.indeterminateReasonOther =
      gSignatureHelpIndeterminateReasonOther.exchange(
          0, std::memory_order_relaxed);
  snapshot.overloadResolverAttempts =
      gOverloadResolverAttempts.exchange(0, std::memory_order_relaxed);
  snapshot.overloadResolverResolved =
      gOverloadResolverResolved.exchange(0, std::memory_order_relaxed);
  snapshot.overloadResolverAmbiguous =
      gOverloadResolverAmbiguous.exchange(0, std::memory_order_relaxed);
  snapshot.overloadResolverNoViable =
      gOverloadResolverNoViable.exchange(0, std::memory_order_relaxed);
  snapshot.overloadResolverShadowMismatch =
      gOverloadResolverShadowMismatch.exchange(0, std::memory_order_relaxed);
  return snapshot;
}
