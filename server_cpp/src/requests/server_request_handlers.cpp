#include "server_request_handlers.hpp"

#include "active_unit.hpp"
#include "callsite_parser.hpp"
#include "call_query.hpp"
#include "completion_rendering.hpp"
#include "declaration_query.hpp"
#include "document_owner.hpp"
#include "definition_location.hpp"
#include "expanded_source.hpp"
#include "hlsl_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_docs.hpp"
#include "hover_markdown.hpp"
#include "hover_rendering.hpp"
#include "include_resolver.hpp"
#include "indeterminate_reasons.hpp"
#include "interactive_semantic_runtime.hpp"
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
#include "server_request_handler_common.hpp"
#include "server_request_handler_background.hpp"
#include "server_request_handler_completion.hpp"
#include "server_request_handler_definition.hpp"
#include "server_request_handler_hover.hpp"
#include "server_request_handler_references.hpp"
#include "server_request_handler_signature.hpp"
#include "symbol_query.hpp"
#include "text_utils.hpp"
#include "type_desc.hpp"
#include "type_eval.hpp"
#include "type_model.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"
#include "workspace_summary_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <sys/stat.h>
#endif

size_t positionToOffset(const std::string &text, int line, int character);

std::vector<LocatedOccurrence> collectOccurrencesForSymbol(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines);

bool findDeclaredIdentifierInDeclarationLine(const std::string &line,
                                             const std::string &word,
                                             size_t &posOut);
using CoreRequestHandler = bool (*)(const std::string &, const Json &,
                                    const Json *, ServerRequestContext &,
                                    const std::vector<std::string> &,
                                    const std::vector<std::string> &);

static const CoreRequestHandler kCoreRequestHandlers[] = {
    request_definition_handlers::handleDefinitionRequest,
    request_hover_handlers::handleHoverRequest,
    request_completion_handlers::handleCompletionRequest,
    request_signature_handlers::handleSignatureHelpRequest,
    request_background_handlers::handleInlayHintRequest,

    request_background_handlers::handleSemanticTokensFullRequest,
    request_background_handlers::handleSemanticTokensRangeRequest,

    request_reference_handlers::handleReferencesRequest,
    request_reference_handlers::handlePrepareRenameRequest,
    request_reference_handlers::handleRenameRequest,

    request_background_handlers::handleDocumentSymbolRequest,
    request_background_handlers::handleWorkspaceSymbolRequest,
};

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

void
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

void emitSignatureHelpResolverShadowMismatch(
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

void recordOverloadResolverResult(const ResolveCallResult &result) {
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
