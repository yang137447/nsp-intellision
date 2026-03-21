#include "server_request_handlers.hpp"

#include "active_unit.hpp"
#include "callsite_parser.hpp"
#include "call_query.hpp"
#include "completion_rendering.hpp"
#include "declaration_query.hpp"
#include "definition_fallback.hpp"
#include "definition_location.hpp"
#include "expanded_source.hpp"
#include "full_ast.hpp"
#include "hlsl_ast.hpp"
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
#include "semantic_snapshot.hpp"
#include "semantic_tokens.hpp"
#include "server_occurrences.hpp"
#include "server_parse.hpp"
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
#include <fstream>
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
    const std::unordered_map<std::string, int> &defines,
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

bool collectStructFieldsInIncludeGraph(
    const std::string &uri, const std::string &structName,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    std::unordered_set<std::string> &visited,
    std::vector<std::string> &fieldsOut);

size_t positionToOffset(const std::string &text, int line, int character);

std::vector<LocatedOccurrence> collectOccurrencesForSymbol(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines);
std::vector<std::string> getIncludeGraphUrisCached(
    const std::string &rootUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions);

bool findMacroDefinitionInLine(const std::string &line, const std::string &word,
                               size_t &posOut);

bool findDeclaredIdentifierInDeclarationLine(const std::string &line,
                                             const std::string &word,
                                             size_t &posOut);
static std::string extractReturnTypeFromFunctionLabel(const std::string &label,
                                                      const std::string &name);

static bool queryFunctionSignatureWithSemanticFallback(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::string &name, int lineIndex, int nameCharacter,
    ServerRequestContext &ctx, std::string &labelOut,
    std::vector<std::string> &parametersOut);

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
static bool collectIncludeContextDefinitionLocations(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    std::vector<DefinitionLocation> &outLocations);

static void appendStructFieldMarkdown(
    std::string &markdown,
    const std::vector<SemanticSnapshotStructFieldInfo> &fields);

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
  if (!parseCallSiteAtOffset(doc->text, cursorOffset, functionName,
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
                                     doc->epoch, ctx.workspaceFolders,
                                     ctx.includePaths, ctx.shaderExtensions,
                                     ctx.preprocessorDefines,
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
  {
    std::vector<DefinitionLocation> includeContextLocations;
    if (collectIncludeContextDefinitionLocations(uri, word, ctx,
                                                 includeContextLocations)) {
      Json locations = makeArray();
      for (const auto &location : includeContextLocations) {
        locations.a.push_back(makeLocationRange(location.uri, location.line,
                                                location.start, location.end));
      }
      writeResponse(id, locations);
      return true;
    }
  }
  DefinitionLocation newPathDefinition;
  std::unordered_set<std::string> visited;
  const bool newPathFound = findDefinitionInIncludeGraph(
      uri, word, ctx.documentSnapshot(), ctx.workspaceFolders, ctx.includePaths,
      ctx.shaderExtensions, ctx.preprocessorDefines, visited,
      newPathDefinition);
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

static bool pathHasNsfExtension(const std::string &path) {
  std::filesystem::path fsPath(path);
  std::string ext = fsPath.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return ext == ".nsf";
}

static std::string normalizeComparablePath(std::string value) {
  value = std::filesystem::path(value).lexically_normal().string();
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   char c = static_cast<char>(std::tolower(ch));
                   return c == '\\' ? '/' : c;
                 });
  while (value.size() > 1 && value.back() == '/')
    value.pop_back();
  return value;
}

static bool extractIncludePathOutsideCommentsLocal(
    const std::string &line, bool &inBlockCommentInOut,
    std::string &outIncludePath) {
  outIncludePath.clear();
  bool inString = false;
  bool escape = false;
  for (size_t i = 0; i < line.size();) {
    const char ch = line[i];
    const char next = (i + 1 < line.size()) ? line[i + 1] : '\0';

    if (inBlockCommentInOut) {
      if (ch == '*' && next == '/') {
        inBlockCommentInOut = false;
        i += 2;
        continue;
      }
      i++;
      continue;
    }

    if (!inString && ch == '/' && next == '*') {
      inBlockCommentInOut = true;
      i += 2;
      continue;
    }
    if (!inString && ch == '/' && next == '/')
      break;

    if (ch == '"' && !escape) {
      inString = !inString;
      i++;
      continue;
    }
    if (inString) {
      escape = (!escape && ch == '\\');
      i++;
      continue;
    }
    escape = false;

    if (ch != '#') {
      i++;
      continue;
    }

    size_t j = i + 1;
    while (j < line.size() &&
           std::isspace(static_cast<unsigned char>(line[j]))) {
      j++;
    }
    if (j + 7 > line.size() || line.compare(j, 7, "include") != 0) {
      i++;
      continue;
    }
    j += 7;
    while (j < line.size() &&
           std::isspace(static_cast<unsigned char>(line[j]))) {
      j++;
    }
    if (j >= line.size())
      return false;

    const char opener = line[j];
    const char closer = opener == '<' ? '>' : (opener == '"' ? '"' : '\0');
    if (closer == '\0')
      return false;
    j++;
    const size_t start = j;
    while (j < line.size() && line[j] != closer)
      j++;
    if (j >= line.size())
      return false;
    outIncludePath = line.substr(start, j - start);
    return !outIncludePath.empty();
  }
  return false;
}

static void collectIncludeContextUnitsByWorkspaceScan(
    const std::string &uri, const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::vector<std::string> &unitPaths) {
  const std::string targetPath = uriToPath(uri);
  if (targetPath.empty())
    return;
  const std::string targetPathN = normalizeComparablePath(targetPath);
  std::unordered_set<std::string> seen;
  for (const auto &path : unitPaths)
    seen.insert(normalizeComparablePath(path));
  const auto timeBudget = std::chrono::milliseconds(600);
  const auto startedAt = std::chrono::steady_clock::now();
  size_t visitedFiles = 0;
  const size_t fileBudget = 2048;

  auto isAbsolutePath = [](const std::string &path) {
    if (path.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':')
      return true;
    return path.rfind("\\\\", 0) == 0 || path.rfind("/", 0) == 0;
  };
  auto joinPath = [](const std::string &base, const std::string &child) {
    if (base.empty())
      return child;
    if (base.back() == '/' || base.back() == '\\')
      return base + child;
    return base + "\\" + child;
  };

  std::vector<std::string> searchRoots;
  std::unordered_set<std::string> seenRoots;
  auto addSearchRoot = [&](const std::string &root) {
    if (root.empty())
      return;
    const std::string normalized = normalizeComparablePath(root);
    if (!seenRoots.insert(normalized).second)
      return;
    searchRoots.push_back(root);
  };

  for (const auto &inc : includePaths) {
    if (inc.empty())
      continue;
    if (isAbsolutePath(inc)) {
      addSearchRoot(inc);
      continue;
    }
    for (const auto &folder : workspaceFolders) {
      if (folder.empty())
        continue;
      addSearchRoot(joinPath(folder, inc));
    }
  }

  if (searchRoots.empty())
    return;

  for (const auto &folder : searchRoots) {
    std::error_code ec;
    std::filesystem::path root(folder);
    if (!std::filesystem::exists(root, ec))
      continue;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator endIt;
    for (; it != endIt && !ec; it.increment(ec)) {
      if (visitedFiles >= fileBudget)
        return;
      if (std::chrono::steady_clock::now() - startedAt > timeBudget)
        return;
      if (!it->is_regular_file(ec))
        continue;
      const std::filesystem::path candidatePath = it->path();
      if (!pathHasNsfExtension(candidatePath.string()))
        continue;
      visitedFiles++;

      std::ifstream stream(candidatePath, std::ios::binary);
      if (!stream)
        continue;
      bool inBlockComment = false;
      std::string line;
      const std::string candidateUri = pathToUri(candidatePath.string());
      while (std::getline(stream, line)) {
        std::string includePath;
        if (!extractIncludePathOutsideCommentsLocal(line, inBlockComment,
                                                    includePath)) {
          continue;
        }
        auto resolvedCandidates = resolveIncludeCandidates(
            candidateUri, includePath, workspaceFolders, includePaths,
            shaderExtensions);
        for (const auto &resolved : resolvedCandidates) {
          if (normalizeComparablePath(resolved) != targetPathN)
            continue;
          const std::string candidateUnitPath = candidatePath.string();
          const std::string candidateUnitPathN =
              normalizeComparablePath(candidateUnitPath);
          if (seen.insert(candidateUnitPathN).second)
            unitPaths.push_back(candidateUnitPath);
          break;
        }
      }
    }
  }
}

static std::vector<std::string>
collectIncludeContextUnitPaths(const std::string &uri,
                               const ServerRequestContext &ctx) {
  std::vector<std::string> unitPaths;
  if (!getActiveUnitPath().empty())
    return unitPaths;

  const std::string path = uriToPath(uri);
  if (path.empty() || pathHasNsfExtension(path))
    return unitPaths;

  workspaceIndexCollectIncludingUnits({uri}, unitPaths, 256);
  if (unitPaths.size() <= 1) {
    collectIncludeContextUnitsByWorkspaceScan(
        uri, ctx.workspaceFolders, ctx.includePaths, ctx.shaderExtensions,
        unitPaths);
  }
  std::sort(unitPaths.begin(), unitPaths.end());
  unitPaths.erase(std::unique(unitPaths.begin(), unitPaths.end()),
                  unitPaths.end());
  return unitPaths;
}

static std::vector<std::string>
collectIncludeContextUnitsForHover(const std::string &uri,
                                   const ServerRequestContext &ctx) {
  std::vector<std::string> labels;
  std::unordered_set<std::string> seen;
  for (const auto &candidatePath : collectIncludeContextUnitPaths(uri, ctx)) {
    std::filesystem::path fsPath(candidatePath);
    std::string label = fsPath.filename().string();
    if (label.empty())
      label = candidatePath;
    if (!seen.insert(label).second)
      continue;
    labels.push_back(label);
  }
  std::sort(labels.begin(), labels.end());
  return labels;
}

static std::string
buildIncludeContextHoverNote(const std::vector<std::string> &unitLabels) {
  if (unitLabels.size() <= 1)
    return std::string();
  std::string note = "(Include context ambiguous) Candidate units: ";
  for (size_t i = 0; i < unitLabels.size(); i++) {
    if (i > 0)
      note += ", ";
    note += unitLabels[i];
  }
  return note;
}

static bool collectIncludeContextDefinitionLocations(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    std::vector<DefinitionLocation> &outLocations) {
  outLocations.clear();
  if (!getActiveUnitPath().empty())
    return false;

  const std::string path = uriToPath(uri);
  if (path.empty() || pathHasNsfExtension(path))
    return false;

  std::vector<std::string> candidateUnitPaths =
      collectIncludeContextUnitPaths(uri, ctx);
  if (candidateUnitPaths.size() <= 1)
    return false;

  std::unordered_set<std::string> seen;
  for (const auto &candidateUnitPath : candidateUnitPaths) {
    const std::string candidateUnitUri = pathToUri(candidateUnitPath);
    std::unordered_set<std::string> visited;
    DefinitionLocation location;
    if (!findDefinitionInIncludeGraph(
            candidateUnitUri, word, ctx.documentSnapshot(), ctx.workspaceFolders,
            ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
            visited, location)) {
      continue;
    }
    const std::string key =
        location.uri + "|" + std::to_string(location.line) + "|" +
        std::to_string(location.start) + "|" + std::to_string(location.end);
    if (!seen.insert(key).second)
      continue;
    outLocations.push_back(location);
  }

  return outLocations.size() > 1;
}

static void appendIncludeContextDefinitionListItems(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    std::vector<HoverLocationListItem> &outItems) {
  outItems.clear();
  std::vector<std::string> candidateUnitPaths =
      collectIncludeContextUnitPaths(uri, ctx);
  if (candidateUnitPaths.size() <= 1)
    return;

  std::unordered_set<std::string> seen;
  for (const auto &candidateUnitPath : candidateUnitPaths) {
    const std::string candidateUnitUri = pathToUri(candidateUnitPath);
    std::unordered_set<std::string> visited;
    DefinitionLocation location;
    if (!findDefinitionInIncludeGraph(
            candidateUnitUri, word, ctx.documentSnapshot(), ctx.workspaceFolders,
            ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
            visited, location)) {
      continue;
    }
    HoverLocationListItem item;
    item.label = std::filesystem::path(candidateUnitPath).filename().string();
    if (item.label.empty())
      item.label = candidateUnitPath;
    item.locationDisplay =
        formatFileLineDisplay(location.uri, location.line, uri);
    std::string defText;
    if (ctx.readDocumentText(location.uri, defText)) {
      std::string label;
      std::vector<std::string> paramsOut;
      uint64_t dEpoch = 0;
      const Document *dDoc = ctx.findDocument(location.uri);
      if (dDoc)
        dEpoch = dDoc->epoch;
      const bool fastSig = queryFunctionSignatureWithSemanticFallback(
          location.uri, defText, dEpoch, word, location.line, location.start,
          ctx, label, paramsOut);
      if (fastSig && !label.empty()) {
        item.locationDisplay =
            label + " @ " + formatFileLineDisplay(location.uri, location.line,
                                                  uri);
      }
    }
    const std::string key = item.label + "|" + item.locationDisplay;
    if (!seen.insert(key).second)
      continue;
    outItems.push_back(std::move(item));
  }
}

static std::vector<LocatedOccurrence> collectActiveOccurrencesInDocumentLocal(
    const std::string &uri, const std::string &text, const std::string &word,
    const std::unordered_map<std::string, int> &defines) {
  std::vector<LocatedOccurrence> locations;
  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  auto occurrences = findOccurrences(expandedSource.text, word);
  for (const auto &occ : occurrences) {
    locations.push_back(LocatedOccurrence{uri, occ.line, occ.start, occ.end});
  }
  return locations;
}

static bool collectIncludeContextOccurrences(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    std::vector<LocatedOccurrence> &outOccurrences) {
  outOccurrences.clear();
  std::vector<std::string> candidateUnitPaths =
      collectIncludeContextUnitPaths(uri, ctx);
  if (candidateUnitPaths.size() <= 1)
    return false;

  std::unordered_set<std::string> seen;
  const auto documents = ctx.documentSnapshot();
  for (const auto &candidateUnitPath : candidateUnitPaths) {
    const std::string candidateUnitUri = pathToUri(candidateUnitPath);
    auto orderedUris =
        getIncludeGraphUrisCached(candidateUnitUri, documents,
                                  ctx.workspaceFolders, ctx.includePaths,
                                  ctx.shaderExtensions);
    prefetchDocumentTexts(orderedUris, documents);
    for (const auto &candidateUri : orderedUris) {
      std::string text;
      if (!loadDocumentText(candidateUri, documents, text))
        continue;
      auto occurrences = collectActiveOccurrencesInDocumentLocal(
          candidateUri, text, word, ctx.preprocessorDefines);
      for (const auto &occ : occurrences) {
        const std::string key =
            occ.uri + "|" + std::to_string(occ.line) + "|" +
            std::to_string(occ.start) + "|" + std::to_string(occ.end);
        if (!seen.insert(key).second)
          continue;
        outOccurrences.push_back(occ);
      }
    }
  }
  return !outOccurrences.empty();
}

static bool queryFunctionSignatureWithSemanticFallback(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::string &name, int lineIndex, int nameCharacter,
    ServerRequestContext &ctx, std::string &labelOut,
    std::vector<std::string> &parametersOut) {
  return querySemanticSnapshotFunctionSignature(
             uri, text, epoch, ctx.workspaceFolders, ctx.includePaths,
             ctx.shaderExtensions, ctx.preprocessorDefines, name, lineIndex,
             nameCharacter, labelOut, parametersOut) ||
         queryFullAstFunctionSignature(uri, text, epoch, name, lineIndex,
                                       nameCharacter, labelOut,
                                       parametersOut);
}

static void appendStructFieldMarkdown(
    std::string &markdown,
    const std::vector<SemanticSnapshotStructFieldInfo> &fields) {
  if (fields.empty())
    return;
  markdown += "\n\nMembers:";
  size_t shown = 0;
  for (size_t i = 0; i < fields.size() && shown < 256; i++) {
    markdown += "\n- `";
    if (!fields[i].type.empty()) {
      markdown += fields[i].type;
      markdown += " ";
    }
    markdown += fields[i].name;
    markdown += "`";
    shown++;
  }
  if (fields.size() > shown)
    markdown += "\n- `...`";
}

static bool writeIncludeContextHoverResponse(
    const Json &id, const std::string &uri, const std::string &word,
    const std::string &includeContextNote, ServerRequestContext &ctx) {
  if (includeContextNote.empty())
    return false;

  std::vector<DefinitionLocation> locations;
  if (!collectIncludeContextDefinitionLocations(uri, word, ctx, locations) ||
      locations.empty()) {
    return false;
  }

  std::vector<HoverLocationListItem> listItems;
  appendIncludeContextDefinitionListItems(uri, word, ctx, listItems);
  if (listItems.empty())
    return false;

  const DefinitionLocation &primary = locations.front();
  std::string defText;
  if (!ctx.readDocumentText(primary.uri, defText))
    return false;

  std::string label;
  std::vector<std::string> paramsOut;
  uint64_t locEpoch = 0;
  const Document *locDoc = ctx.findDocument(primary.uri);
  if (locDoc)
    locEpoch = locDoc->epoch;
  const bool fastSig = queryFunctionSignatureWithSemanticFallback(
      primary.uri, defText, locEpoch, word, primary.line, primary.start, ctx,
      label, paramsOut);
  if (!fastSig || label.empty()) {
    return false;
  }

  HoverFunctionMarkdownInput functionInput;
  functionInput.code = label;
  functionInput.kindLabel = "(HLSL function)";
  functionInput.returnType = extractReturnTypeFromFunctionLabel(label, word);
  functionInput.parameters = paramsOut;
  functionInput.definedAt = formatFileLineDisplay(primary.uri, primary.line, uri);
  functionInput.leadingDoc = extractLeadingDocumentationAtLine(defText, primary.line);
  functionInput.inlineDoc =
      extractTrailingInlineCommentAtLine(defText, primary.line, primary.end);
  functionInput.selectionNote = includeContextNote;
  functionInput.listTitle = "Candidate units";
  functionInput.listItems = std::move(listItems);

  Json hover = makeObject();
  hover.o["contents"] = makeMarkup(renderHoverFunctionMarkdown(functionInput));
  writeResponse(id, hover);
  return true;
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

static void collectFieldInfosFromTextRecursive(
    const std::string &baseUri, const std::string &text,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines, int depth,
    std::unordered_set<std::string> &visitedUris,
    std::unordered_set<std::string> &seenNames,
    std::vector<SemanticSnapshotStructFieldInfo> &outFields) {
  if (depth <= 0 || outFields.size() >= 512)
    return;
  if (!visitedUris.insert(baseUri).second)
    return;

  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  const HlslAstDocument ast = buildHlslAstDocument(expandedSource);
  std::vector<char> globalConsumed(ast.globalVariables.size(), 0);

  for (const auto &decl : ast.topLevelDecls) {
    if (decl.kind == HlslTopLevelDeclKind::Include) {
      const std::string includePath = decl.name;
      if (!includePath.empty()) {
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
          collectFieldInfosFromTextRecursive(
              candidateUri, candidateText, documents, workspaceFolders,
              includePaths, shaderExtensions, defines, depth - 1, visitedUris,
              seenNames, outFields);
          break;
        }
      }
    } else if (decl.kind == HlslTopLevelDeclKind::GlobalVariable) {
      const HlslAstGlobalVariableDecl *matchedGlobal = nullptr;
      size_t matchedIndex = 0;
      for (size_t index = 0; index < ast.globalVariables.size(); index++) {
        if (globalConsumed[index])
          continue;
        const auto &candidate = ast.globalVariables[index];
        if (candidate.line != decl.line || candidate.name != decl.name)
          continue;
        matchedGlobal = &candidate;
        matchedIndex = index;
        break;
      }
      if (!matchedGlobal)
        continue;
      globalConsumed[matchedIndex] = 1;
      if (!seenNames.insert(matchedGlobal->name).second)
        continue;
      SemanticSnapshotStructFieldInfo item;
      item.name = matchedGlobal->name;
      item.type = matchedGlobal->type;
      item.line = matchedGlobal->line;
      outFields.push_back(std::move(item));
      if (outFields.size() >= 512)
        return;
    }
    if (outFields.size() >= 512)
      return;
  }
}

static bool collectStructFieldInfosFromTextWithInlineIncludes(
    const std::string &baseUri, const std::string &text,
    const std::string &structName,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    std::vector<SemanticSnapshotStructFieldInfo> &fieldsOut) {
  fieldsOut.clear();
  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  const HlslAstDocument ast = buildHlslAstDocument(expandedSource);

  const HlslAstStructDecl *targetStruct = nullptr;
  for (const auto &decl : ast.structs) {
    if (decl.name == structName) {
      targetStruct = &decl;
      break;
    }
  }

  std::unordered_set<std::string> seen;
  if (targetStruct) {
    for (const auto &field : targetStruct->fields) {
      if (!seen.insert(field.name).second)
        continue;
      SemanticSnapshotStructFieldInfo item;
      item.name = field.name;
      item.type = field.type;
      item.line = field.line;
      fieldsOut.push_back(std::move(item));
      if (fieldsOut.size() >= 512)
        return true;
    }
  }

  std::vector<std::string> inlineIncludePaths;
  if (targetStruct) {
    inlineIncludePaths.reserve(targetStruct->inlineIncludes.size());
    for (const auto &inlineInclude : targetStruct->inlineIncludes) {
      if (!inlineInclude.path.empty())
        inlineIncludePaths.push_back(inlineInclude.path);
    }
  }

  std::unordered_set<std::string> seenIncludePaths;
  for (const auto &inlineIncludePath : inlineIncludePaths) {
    if (inlineIncludePath.empty() ||
        !seenIncludePaths.insert(inlineIncludePath).second) {
      continue;
    }
    auto candidates =
        resolveIncludeCandidates(baseUri, inlineIncludePath, workspaceFolders,
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
      collectFieldInfosFromTextRecursive(
          candidateUri, candidateText, documents, workspaceFolders,
          includePaths, shaderExtensions, defines, 12, visited, seen,
          fieldsOut);
      break;
    }
    if (fieldsOut.size() >= 512)
      return true;
  }
  return !fieldsOut.empty();
}

static bool structHasActiveInlineInclude(
    const std::string &text, const std::string &structName,
    const std::unordered_map<std::string, int> &defines) {
  if (structName.empty())
    return false;

  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  const HlslAstDocument ast = buildHlslAstDocument(expandedSource);
  for (const auto &decl : ast.structs) {
    if (decl.name != structName)
      continue;
    if (!decl.inlineIncludes.empty())
      return true;
  }
  return false;
}

static void appendStructFieldInfosUniqueByName(
    const std::vector<SemanticSnapshotStructFieldInfo> &source,
    std::unordered_set<std::string> &seenNames,
    std::vector<SemanticSnapshotStructFieldInfo> &dest) {
  for (const auto &field : source) {
    if (field.name.empty() || !seenNames.insert(field.name).second)
      continue;
    dest.push_back(field);
  }
}

static bool queryStructFieldInfosWithInlineIncludeFallback(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::string &structName, bool allowInlineIncludeFallback,
    ServerRequestContext &ctx,
    std::vector<SemanticSnapshotStructFieldInfo> &fieldsOut) {
  fieldsOut.clear();
  std::unordered_set<std::string> seenNames;

  std::vector<SemanticSnapshotStructFieldInfo> snapshotFields;
  if (querySemanticSnapshotStructFieldInfos(
          uri, text, epoch, ctx.workspaceFolders, ctx.includePaths,
          ctx.shaderExtensions, ctx.preprocessorDefines, structName,
          snapshotFields) &&
      !snapshotFields.empty()) {
    appendStructFieldInfosUniqueByName(snapshotFields, seenNames, fieldsOut);
  }

  if (!allowInlineIncludeFallback ||
      !structHasActiveInlineInclude(text, structName, ctx.preprocessorDefines)) {
    return !fieldsOut.empty();
  }

  std::vector<SemanticSnapshotStructFieldInfo> inlineIncludeFields;
  if (!collectStructFieldInfosFromTextWithInlineIncludes(
          uri, text, structName, ctx.documentSnapshot(), ctx.workspaceFolders,
          ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
          inlineIncludeFields) ||
      inlineIncludeFields.empty()) {
    return !fieldsOut.empty();
  }

  appendStructFieldInfosUniqueByName(inlineIncludeFields, seenNames, fieldsOut);
  return !fieldsOut.empty();
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

    if (lineIndex == startLine) {
      size_t pos = 0;
      if (findDeclaredIdentifierInDeclarationLine(line, memberName, pos)) {
        lineOut = lineIndex;
        int endByte = static_cast<int>(pos + memberName.size());
        minCharacterOut = byteOffsetInLineToUtf16(line, endByte);
        return true;
      }
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
  const std::string includeContextNote =
      buildIncludeContextHoverNote(collectIncludeContextUnitsForHover(uri, ctx));
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

  if (writeIncludeContextHoverResponse(id, uri, word, includeContextNote, ctx))
    return true;

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
      const bool fastSig = queryFunctionSignatureWithSemanticFallback(
          uri, doc->text, doc->epoch, word, localDeclLine, localDeclChar, ctx,
          label, paramsOut);
      if (fastSig && !label.empty()) {
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
        if (!includeContextNote.empty())
          functionInput.selectionNote = includeContextNote;
        if (!includeContextNote.empty()) {
          appendIncludeContextDefinitionListItems(uri, word, ctx,
                                                 functionInput.listItems);
          if (!functionInput.listItems.empty())
            functionInput.listTitle = "Candidate units";
        }
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
      std::unordered_set<std::string> seenUris;
      for (const auto &d : funcDefs) {
        if (seenUris.insert(d.uri).second) {
          std::string defText;
          if (ctx.readDocumentText(d.uri, defText)) {
            uint64_t dEpoch = 0;
            if (const Document *dDoc = ctx.findDocument(d.uri))
              dEpoch = dDoc->epoch;
            std::vector<SemanticSnapshotFunctionOverloadInfo> overloads;
            if (querySemanticSnapshotFunctionOverloads(
                    d.uri, defText, dEpoch, ctx.workspaceFolders,
                    ctx.includePaths, ctx.shaderExtensions,
                    ctx.preprocessorDefines, word, overloads)) {
              for (const auto &overload : overloads) {
                IndexedDefinition overloadDef;
                overloadDef.name = word;
                overloadDef.type = overload.returnType;
                overloadDef.uri = d.uri;
                overloadDef.line = overload.line;
                overloadDef.start = overload.character;
                overloadDef.end =
                    overload.character + static_cast<int>(word.size());
                std::string key;
                key.reserve(overload.label.size() + d.uri.size() + 64);
                key.append(overload.label);
                key.push_back('|');
                key.append(d.uri);
                key.push_back('|');
                key.append(std::to_string(overload.line));
                key.push_back('|');
                key.append(std::to_string(overload.character));
                if (!seen.insert(key).second)
                  continue;
                labels.push_back(HoverFunctionCandidate{
                    overload.label, overload.parameters, overloadDef});
              }
            }
          }
        }
        std::string defText;
        if (!ctx.readDocumentText(d.uri, defText))
          continue;
        std::string label;
        std::vector<std::string> paramsOut;
        uint64_t dEpoch = 0;
        const Document *dDoc = ctx.findDocument(d.uri);
        if (dDoc)
          dEpoch = dDoc->epoch;
        const bool fastSig = queryFunctionSignatureWithSemanticFallback(
            d.uri, defText, dEpoch, word, d.line, d.start, ctx, label,
            paramsOut);
        if (!fastSig || label.empty()) {
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
                                       doc->epoch, ctx.workspaceFolders,
                                       ctx.includePaths, ctx.shaderExtensions,
                                       ctx.preprocessorDefines,
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
      if (!includeContextNote.empty()) {
        if (!functionInput.selectionNote.empty())
          functionInput.selectionNote += "\n\n";
        functionInput.selectionNote += includeContextNote;
        appendIncludeContextDefinitionListItems(uri, word, ctx,
                                               functionInput.listItems);
        if (!functionInput.listItems.empty())
          functionInput.listTitle = "Candidate units";
      }
      const bool hasIncludeContextSummary =
          functionInput.listTitle == "Candidate units" &&
          !functionInput.listItems.empty();
      if (labels.size() > 1 && !hasIncludeContextSummary) {
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
      std::string structText;
      std::vector<SemanticSnapshotStructFieldInfo> fields;
      uint64_t dEpoch = 0;
      if (const Document *dDoc = ctx.findDocument(d.uri))
        dEpoch = dDoc->epoch;
      if (ctx.readDocumentText(d.uri, structText) &&
          querySemanticSnapshotStructFieldInfos(
              d.uri, structText, dEpoch, ctx.workspaceFolders, ctx.includePaths,
              ctx.shaderExtensions, ctx.preprocessorDefines, word, fields) &&
          !fields.empty()) {
        appendStructFieldMarkdown(md, fields);
      } else {
        std::vector<std::string> fieldNames;
        if (workspaceIndexGetStructFields(word, fieldNames) &&
            !fieldNames.empty()) {
          std::vector<SemanticSnapshotStructFieldInfo> fallback;
          fallback.reserve(fieldNames.size());
          for (const auto &fieldName : fieldNames) {
            SemanticSnapshotStructFieldInfo item;
            item.name = fieldName;
            workspaceIndexGetStructMemberType(word, fieldName, item.type);
            fallback.push_back(std::move(item));
          }
          appendStructFieldMarkdown(md, fallback);
        }
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
      std::string structText;
      std::vector<SemanticSnapshotStructFieldInfo> fields;
      uint64_t structEpoch = 0;
      if (const Document *structDoc = ctx.findDocument(structLoc.uri))
        structEpoch = structDoc->epoch;
      if (ctx.readDocumentText(structLoc.uri, structText) &&
          queryStructFieldInfosWithInlineIncludeFallback(
              structLoc.uri, structText, structEpoch, word,
              /*allowInlineIncludeFallback=*/true, ctx, fields) &&
          !fields.empty()) {
        appendStructFieldMarkdown(md, fields);
      } else {
        std::vector<std::string> fieldNames;
        if (workspaceIndexGetStructFields(word, fieldNames) &&
            !fieldNames.empty()) {
          std::vector<SemanticSnapshotStructFieldInfo> fallback;
          fallback.reserve(fieldNames.size());
          for (const auto &fieldName : fieldNames) {
            SemanticSnapshotStructFieldInfo item;
            item.name = fieldName;
            workspaceIndexGetStructMemberType(word, fieldName, item.type);
            fallback.push_back(std::move(item));
          }
          appendStructFieldMarkdown(md, fallback);
        }
      }
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    std::vector<SemanticSnapshotStructFieldInfo> fieldInfos;
    DefinitionLocation indexedStructLoc;
    const bool hasIndexedStructLoc =
        workspaceIndexFindStructDefinition(word, indexedStructLoc);
    const bool allowInlineIncludeFallback =
        !hasIndexedStructLoc || indexedStructLoc.uri == uri;
    queryStructFieldInfosWithInlineIncludeFallback(
        uri, doc->text, doc->epoch, word, allowInlineIncludeFallback, ctx,
        fieldInfos);

    std::vector<std::string> fields;
    bool haveFields = false;
    if (fieldInfos.empty())
      haveFields = workspaceIndexGetStructFields(word, fields);

    if ((!haveFields || fields.empty()) && fieldInfos.empty()) {
      fields.clear();
      std::unordered_set<std::string> visitedStructs;
      haveFields = collectStructFieldsInIncludeGraph(
          uri, word, ctx.documentSnapshot(), ctx.workspaceFolders,
          ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
          visitedStructs, fields);
    }
    if ((haveFields && !fields.empty()) || !fieldInfos.empty()) {
      DefinitionLocation loc;
      bool hasLoc = workspaceIndexFindDefinition(word, loc);
      std::string md;
      md += formatCppCodeBlock("struct " + word);
      if (hasLoc) {
        md += "\n\nDefined at: ";
        md += formatFileLineDisplay(loc.uri, loc.line, uri);
      }
      if (!fieldInfos.empty()) {
        std::unordered_set<std::string> seen;
        std::vector<SemanticSnapshotStructFieldInfo> uniqueFields;
        uniqueFields.reserve(fieldInfos.size());
        for (const auto &field : fieldInfos) {
          if (!seen.insert(field.name).second)
            continue;
          uniqueFields.push_back(field);
        }
        appendStructFieldMarkdown(md, uniqueFields);
      } else {
        std::unordered_set<std::string> seen;
        std::vector<SemanticSnapshotStructFieldInfo> fallback;
        fallback.reserve(fields.size());
        for (const auto &fieldName : fields) {
          if (!seen.insert(fieldName).second)
            continue;
          SemanticSnapshotStructFieldInfo item;
          item.name = fieldName;
          workspaceIndexGetStructMemberType(word, fieldName, item.type);
          fallback.push_back(std::move(item));
        }
        appendStructFieldMarkdown(md, fallback);
      }
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
        const bool fastSig = queryFunctionSignatureWithSemanticFallback(
            uri, doc->text, doc->epoch, word, decl.line, nameChar, ctx,
            functionLabel, functionParams);
        if (fastSig && !functionLabel.empty()) {
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
          if (!includeContextNote.empty())
            functionInput.selectionNote = includeContextNote;
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
      if (!includeContextNote.empty())
        symbolInput.notes.push_back(includeContextNote);
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
        if (!includeContextNote.empty())
          functionInput.selectionNote = includeContextNote;
        if (!includeContextNote.empty()) {
          appendIncludeContextDefinitionListItems(uri, word, ctx,
                                                 functionInput.listItems);
          if (!functionInput.listItems.empty())
            functionInput.listTitle = "Candidate units";
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
        const bool fastSig = queryFunctionSignatureWithSemanticFallback(
            loc.uri, defText, locEpoch, word, loc.line, loc.start, ctx, label,
            paramsOut);
        if (fastSig && !label.empty()) {
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
          if (!includeContextNote.empty())
            functionInput.selectionNote = includeContextNote;
          if (!includeContextNote.empty()) {
            appendIncludeContextDefinitionListItems(uri, word, ctx,
                                                   functionInput.listItems);
            if (!functionInput.listItems.empty())
              functionInput.listTitle = "Candidate units";
          }
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
      if (!includeContextNote.empty())
        symbolInput.notes.push_back(includeContextNote);
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
  std::vector<LocatedOccurrence> occurrences;
  if (!collectIncludeContextOccurrences(uri, word, ctx, occurrences)) {
    occurrences = collectOccurrencesForSymbol(
        uri, word, ctx.documentSnapshot(), ctx.workspaceFolders,
        ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines);
  }
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
  if (!getActiveUnitPath().empty()) {
    // fall through
  } else {
    const std::string path = uriToPath(uri);
    if (!path.empty() && !pathHasNsfExtension(path)) {
      std::vector<std::string> candidateUnitPaths =
          collectIncludeContextUnitPaths(uri, ctx);
      if (candidateUnitPaths.size() > 1) {
        writeResponse(id, makeNull());
        return true;
      }
    }
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
  if (!getActiveUnitPath().empty()) {
    // fall through
  } else {
    const std::string path = uriToPath(uri);
    if (!path.empty() && !pathHasNsfExtension(path)) {
      std::vector<std::string> candidateUnitPaths =
          collectIncludeContextUnitPaths(uri, ctx);
      if (candidateUnitPaths.size() > 1) {
        writeResponse(id, makeNull());
        return true;
      }
    }
  }
  auto occurrences = collectOccurrencesForSymbol(
      uri, word, ctx.documentSnapshot(), ctx.workspaceFolders, ctx.includePaths,
      ctx.shaderExtensions, ctx.preprocessorDefines);
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
