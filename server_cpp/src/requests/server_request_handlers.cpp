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

struct SignatureHelpTimingMetricState {
  uint64_t interactiveOverloadSamples = 0;
  double interactiveOverloadTotalMs = 0.0;
  double interactiveOverloadMaxMs = 0.0;
  uint64_t builtinSignatureSamples = 0;
  double builtinSignatureTotalMs = 0.0;
  double builtinSignatureMaxMs = 0.0;
  uint64_t responseWriteSamples = 0;
  double responseWriteTotalMs = 0.0;
  double responseWriteMaxMs = 0.0;
};

static std::mutex gSignatureHelpTimingMetricsMutex;
static SignatureHelpTimingMetricState gSignatureHelpTimingMetrics;

struct DefinitionMetricState {
  uint64_t currentDocInteractiveSamples = 0;
  double currentDocInteractiveTotalMs = 0.0;
  double currentDocInteractiveMaxMs = 0.0;
  uint64_t activeMacroSamples = 0;
  double activeMacroTotalMs = 0.0;
  double activeMacroMaxMs = 0.0;
  uint64_t activeMacroCachedViewHits = 0;
  uint64_t activeMacroContextBuilds = 0;
  uint64_t currentUnitCallSamples = 0;
  double currentUnitCallTotalMs = 0.0;
  double currentUnitCallMaxMs = 0.0;
  uint64_t responseWriteSamples = 0;
  double responseWriteTotalMs = 0.0;
  double responseWriteMaxMs = 0.0;
};

static std::mutex gDefinitionMetricsMutex;
static DefinitionMetricState gDefinitionMetrics;

struct HoverMetricState {
  uint64_t currentDocDeclarationSamples = 0;
  double currentDocDeclarationTotalMs = 0.0;
  double currentDocDeclarationMaxMs = 0.0;
  uint64_t requestSetupSamples = 0;
  double requestSetupTotalMs = 0.0;
  double requestSetupMaxMs = 0.0;
  uint64_t activeMacroSamples = 0;
  double activeMacroTotalMs = 0.0;
  double activeMacroMaxMs = 0.0;
  uint64_t activeMacroCachedViewHits = 0;
  uint64_t activeMacroContextBuilds = 0;
  uint64_t currentDocFunctionSamples = 0;
  double currentDocFunctionTotalMs = 0.0;
  double currentDocFunctionMaxMs = 0.0;
  uint64_t includeContextSummarySamples = 0;
  double includeContextSummaryTotalMs = 0.0;
  double includeContextSummaryMaxMs = 0.0;
  uint64_t builtinDocSamples = 0;
  double builtinDocTotalMs = 0.0;
  double builtinDocMaxMs = 0.0;
  uint64_t markdownRenderSamples = 0;
  double markdownRenderTotalMs = 0.0;
  double markdownRenderMaxMs = 0.0;
  uint64_t responseWriteSamples = 0;
  double responseWriteTotalMs = 0.0;
  double responseWriteMaxMs = 0.0;
};

static std::mutex gHoverMetricsMutex;
static HoverMetricState gHoverMetrics;

struct InlayMetricState {
  uint64_t deferredSnapshotHitCount = 0;
  uint64_t deferredSnapshotMissCount = 0;
  uint64_t rangeBuildSamples = 0;
  double rangeBuildTotalMs = 0.0;
  double rangeBuildMaxMs = 0.0;
  uint64_t fullBuildSamples = 0;
  double fullBuildTotalMs = 0.0;
  double fullBuildMaxMs = 0.0;
  uint64_t rangeFilterSamples = 0;
  double rangeFilterTotalMs = 0.0;
  double rangeFilterMaxMs = 0.0;
  uint64_t responseWriteSamples = 0;
  double responseWriteTotalMs = 0.0;
  double responseWriteMaxMs = 0.0;
};

static std::mutex gInlayMetricsMutex;
static InlayMetricState gInlayMetrics;

static void recordSignatureHelpDuration(uint64_t &samples, double &totalMs,
                                        double &maxMs, double durationMs) {
  samples++;
  totalMs += durationMs;
  maxMs = std::max(maxMs, durationMs);
}

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

void recordSignatureHelpInteractiveOverloads(double durationMs) {
  std::lock_guard<std::mutex> lock(gSignatureHelpTimingMetricsMutex);
  recordSignatureHelpDuration(
      gSignatureHelpTimingMetrics.interactiveOverloadSamples,
      gSignatureHelpTimingMetrics.interactiveOverloadTotalMs,
      gSignatureHelpTimingMetrics.interactiveOverloadMaxMs, durationMs);
}

void recordSignatureHelpBuiltinSignature(double durationMs) {
  std::lock_guard<std::mutex> lock(gSignatureHelpTimingMetricsMutex);
  recordSignatureHelpDuration(
      gSignatureHelpTimingMetrics.builtinSignatureSamples,
      gSignatureHelpTimingMetrics.builtinSignatureTotalMs,
      gSignatureHelpTimingMetrics.builtinSignatureMaxMs, durationMs);
}

void recordSignatureHelpResponseWrite(double durationMs) {
  std::lock_guard<std::mutex> lock(gSignatureHelpTimingMetricsMutex);
  recordSignatureHelpDuration(
      gSignatureHelpTimingMetrics.responseWriteSamples,
      gSignatureHelpTimingMetrics.responseWriteTotalMs,
      gSignatureHelpTimingMetrics.responseWriteMaxMs, durationMs);
}

void recordDefinitionCurrentDocInteractive(double durationMs) {
  std::lock_guard<std::mutex> lock(gDefinitionMetricsMutex);
  recordSignatureHelpDuration(gDefinitionMetrics.currentDocInteractiveSamples,
                              gDefinitionMetrics.currentDocInteractiveTotalMs,
                              gDefinitionMetrics.currentDocInteractiveMaxMs,
                              durationMs);
}

void recordDefinitionActiveMacro(double durationMs, bool usedCachedView) {
  std::lock_guard<std::mutex> lock(gDefinitionMetricsMutex);
  recordSignatureHelpDuration(gDefinitionMetrics.activeMacroSamples,
                              gDefinitionMetrics.activeMacroTotalMs,
                              gDefinitionMetrics.activeMacroMaxMs, durationMs);
  if (usedCachedView) {
    gDefinitionMetrics.activeMacroCachedViewHits++;
  } else {
    gDefinitionMetrics.activeMacroContextBuilds++;
  }
}

void recordDefinitionCurrentUnitCall(double durationMs) {
  std::lock_guard<std::mutex> lock(gDefinitionMetricsMutex);
  recordSignatureHelpDuration(gDefinitionMetrics.currentUnitCallSamples,
                              gDefinitionMetrics.currentUnitCallTotalMs,
                              gDefinitionMetrics.currentUnitCallMaxMs,
                              durationMs);
}

void recordDefinitionResponseWrite(double durationMs) {
  std::lock_guard<std::mutex> lock(gDefinitionMetricsMutex);
  recordSignatureHelpDuration(gDefinitionMetrics.responseWriteSamples,
                              gDefinitionMetrics.responseWriteTotalMs,
                              gDefinitionMetrics.responseWriteMaxMs,
                              durationMs);
}

void recordHoverRequestSetup(double durationMs) {
  std::lock_guard<std::mutex> lock(gHoverMetricsMutex);
  recordSignatureHelpDuration(gHoverMetrics.requestSetupSamples,
                              gHoverMetrics.requestSetupTotalMs,
                              gHoverMetrics.requestSetupMaxMs, durationMs);
}

void recordHoverActiveMacro(double durationMs, bool usedCachedView) {
  std::lock_guard<std::mutex> lock(gHoverMetricsMutex);
  recordSignatureHelpDuration(gHoverMetrics.activeMacroSamples,
                              gHoverMetrics.activeMacroTotalMs,
                              gHoverMetrics.activeMacroMaxMs, durationMs);
  if (usedCachedView) {
    gHoverMetrics.activeMacroCachedViewHits++;
  } else {
    gHoverMetrics.activeMacroContextBuilds++;
  }
}

void recordHoverCurrentDocDeclaration(double durationMs) {
  std::lock_guard<std::mutex> lock(gHoverMetricsMutex);
  recordSignatureHelpDuration(gHoverMetrics.currentDocDeclarationSamples,
                              gHoverMetrics.currentDocDeclarationTotalMs,
                              gHoverMetrics.currentDocDeclarationMaxMs,
                              durationMs);
}

void recordHoverCurrentDocFunction(double durationMs) {
  std::lock_guard<std::mutex> lock(gHoverMetricsMutex);
  recordSignatureHelpDuration(gHoverMetrics.currentDocFunctionSamples,
                              gHoverMetrics.currentDocFunctionTotalMs,
                              gHoverMetrics.currentDocFunctionMaxMs,
                              durationMs);
}

void recordHoverIncludeContextSummary(double durationMs) {
  std::lock_guard<std::mutex> lock(gHoverMetricsMutex);
  recordSignatureHelpDuration(gHoverMetrics.includeContextSummarySamples,
                              gHoverMetrics.includeContextSummaryTotalMs,
                              gHoverMetrics.includeContextSummaryMaxMs,
                              durationMs);
}

void recordHoverBuiltinDoc(double durationMs) {
  std::lock_guard<std::mutex> lock(gHoverMetricsMutex);
  recordSignatureHelpDuration(gHoverMetrics.builtinDocSamples,
                              gHoverMetrics.builtinDocTotalMs,
                              gHoverMetrics.builtinDocMaxMs, durationMs);
}

void recordHoverMarkdownRender(double durationMs) {
  std::lock_guard<std::mutex> lock(gHoverMetricsMutex);
  recordSignatureHelpDuration(gHoverMetrics.markdownRenderSamples,
                              gHoverMetrics.markdownRenderTotalMs,
                              gHoverMetrics.markdownRenderMaxMs, durationMs);
}

void recordHoverResponseWrite(double durationMs) {
  std::lock_guard<std::mutex> lock(gHoverMetricsMutex);
  recordSignatureHelpDuration(gHoverMetrics.responseWriteSamples,
                              gHoverMetrics.responseWriteTotalMs,
                              gHoverMetrics.responseWriteMaxMs, durationMs);
}

void recordInlayDeferredSnapshotHit() {
  std::lock_guard<std::mutex> lock(gInlayMetricsMutex);
  gInlayMetrics.deferredSnapshotHitCount++;
}

void recordInlayDeferredSnapshotMiss() {
  std::lock_guard<std::mutex> lock(gInlayMetricsMutex);
  gInlayMetrics.deferredSnapshotMissCount++;
}

void recordInlayRangeBuild(double durationMs) {
  std::lock_guard<std::mutex> lock(gInlayMetricsMutex);
  recordSignatureHelpDuration(gInlayMetrics.rangeBuildSamples,
                              gInlayMetrics.rangeBuildTotalMs,
                              gInlayMetrics.rangeBuildMaxMs, durationMs);
}

void recordInlayFullBuild(double durationMs) {
  std::lock_guard<std::mutex> lock(gInlayMetricsMutex);
  recordSignatureHelpDuration(gInlayMetrics.fullBuildSamples,
                              gInlayMetrics.fullBuildTotalMs,
                              gInlayMetrics.fullBuildMaxMs, durationMs);
}

void recordInlayRangeFilter(double durationMs) {
  std::lock_guard<std::mutex> lock(gInlayMetricsMutex);
  recordSignatureHelpDuration(gInlayMetrics.rangeFilterSamples,
                              gInlayMetrics.rangeFilterTotalMs,
                              gInlayMetrics.rangeFilterMaxMs, durationMs);
}

void recordInlayResponseWrite(double durationMs) {
  std::lock_guard<std::mutex> lock(gInlayMetricsMutex);
  recordSignatureHelpDuration(gInlayMetrics.responseWriteSamples,
                              gInlayMetrics.responseWriteTotalMs,
                              gInlayMetrics.responseWriteMaxMs, durationMs);
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
  snapshot.overloadResolverShadowMismatch = 0;
  {
    std::lock_guard<std::mutex> lock(gSignatureHelpTimingMetricsMutex);
    snapshot.interactiveOverloadSamples =
        gSignatureHelpTimingMetrics.interactiveOverloadSamples;
    snapshot.interactiveOverloadTotalMs =
        gSignatureHelpTimingMetrics.interactiveOverloadTotalMs;
    snapshot.interactiveOverloadMaxMs =
        gSignatureHelpTimingMetrics.interactiveOverloadMaxMs;
    snapshot.builtinSignatureSamples =
        gSignatureHelpTimingMetrics.builtinSignatureSamples;
    snapshot.builtinSignatureTotalMs =
        gSignatureHelpTimingMetrics.builtinSignatureTotalMs;
    snapshot.builtinSignatureMaxMs =
        gSignatureHelpTimingMetrics.builtinSignatureMaxMs;
    snapshot.responseWriteSamples =
        gSignatureHelpTimingMetrics.responseWriteSamples;
    snapshot.responseWriteTotalMs =
        gSignatureHelpTimingMetrics.responseWriteTotalMs;
    snapshot.responseWriteMaxMs =
        gSignatureHelpTimingMetrics.responseWriteMaxMs;
    gSignatureHelpTimingMetrics = SignatureHelpTimingMetricState{};
  }
  return snapshot;
}

DefinitionMetricsSnapshot takeDefinitionMetricsSnapshot() {
  std::lock_guard<std::mutex> lock(gDefinitionMetricsMutex);
  DefinitionMetricsSnapshot snapshot;
  snapshot.currentDocInteractiveSamples =
      gDefinitionMetrics.currentDocInteractiveSamples;
  snapshot.currentDocInteractiveTotalMs =
      gDefinitionMetrics.currentDocInteractiveTotalMs;
  snapshot.currentDocInteractiveMaxMs =
      gDefinitionMetrics.currentDocInteractiveMaxMs;
  snapshot.activeMacroSamples = gDefinitionMetrics.activeMacroSamples;
  snapshot.activeMacroTotalMs = gDefinitionMetrics.activeMacroTotalMs;
  snapshot.activeMacroMaxMs = gDefinitionMetrics.activeMacroMaxMs;
  snapshot.activeMacroCachedViewHits =
      gDefinitionMetrics.activeMacroCachedViewHits;
  snapshot.activeMacroContextBuilds =
      gDefinitionMetrics.activeMacroContextBuilds;
  snapshot.currentUnitCallSamples = gDefinitionMetrics.currentUnitCallSamples;
  snapshot.currentUnitCallTotalMs = gDefinitionMetrics.currentUnitCallTotalMs;
  snapshot.currentUnitCallMaxMs = gDefinitionMetrics.currentUnitCallMaxMs;
  snapshot.responseWriteSamples = gDefinitionMetrics.responseWriteSamples;
  snapshot.responseWriteTotalMs = gDefinitionMetrics.responseWriteTotalMs;
  snapshot.responseWriteMaxMs = gDefinitionMetrics.responseWriteMaxMs;
  gDefinitionMetrics = DefinitionMetricState{};
  return snapshot;
}

HoverMetricsSnapshot takeHoverMetricsSnapshot() {
  std::lock_guard<std::mutex> lock(gHoverMetricsMutex);
  HoverMetricsSnapshot snapshot;
  snapshot.currentDocDeclarationSamples =
      gHoverMetrics.currentDocDeclarationSamples;
  snapshot.currentDocDeclarationTotalMs =
      gHoverMetrics.currentDocDeclarationTotalMs;
  snapshot.currentDocDeclarationMaxMs =
      gHoverMetrics.currentDocDeclarationMaxMs;
  snapshot.requestSetupSamples = gHoverMetrics.requestSetupSamples;
  snapshot.requestSetupTotalMs = gHoverMetrics.requestSetupTotalMs;
  snapshot.requestSetupMaxMs = gHoverMetrics.requestSetupMaxMs;
  snapshot.activeMacroSamples = gHoverMetrics.activeMacroSamples;
  snapshot.activeMacroTotalMs = gHoverMetrics.activeMacroTotalMs;
  snapshot.activeMacroMaxMs = gHoverMetrics.activeMacroMaxMs;
  snapshot.activeMacroCachedViewHits =
      gHoverMetrics.activeMacroCachedViewHits;
  snapshot.activeMacroContextBuilds = gHoverMetrics.activeMacroContextBuilds;
  snapshot.currentDocFunctionSamples = gHoverMetrics.currentDocFunctionSamples;
  snapshot.currentDocFunctionTotalMs = gHoverMetrics.currentDocFunctionTotalMs;
  snapshot.currentDocFunctionMaxMs = gHoverMetrics.currentDocFunctionMaxMs;
  snapshot.includeContextSummarySamples =
      gHoverMetrics.includeContextSummarySamples;
  snapshot.includeContextSummaryTotalMs =
      gHoverMetrics.includeContextSummaryTotalMs;
  snapshot.includeContextSummaryMaxMs =
      gHoverMetrics.includeContextSummaryMaxMs;
  snapshot.builtinDocSamples = gHoverMetrics.builtinDocSamples;
  snapshot.builtinDocTotalMs = gHoverMetrics.builtinDocTotalMs;
  snapshot.builtinDocMaxMs = gHoverMetrics.builtinDocMaxMs;
  snapshot.markdownRenderSamples = gHoverMetrics.markdownRenderSamples;
  snapshot.markdownRenderTotalMs = gHoverMetrics.markdownRenderTotalMs;
  snapshot.markdownRenderMaxMs = gHoverMetrics.markdownRenderMaxMs;
  snapshot.responseWriteSamples = gHoverMetrics.responseWriteSamples;
  snapshot.responseWriteTotalMs = gHoverMetrics.responseWriteTotalMs;
  snapshot.responseWriteMaxMs = gHoverMetrics.responseWriteMaxMs;
  gHoverMetrics = HoverMetricState{};
  return snapshot;
}

InlayMetricsSnapshot takeInlayMetricsSnapshot() {
  std::lock_guard<std::mutex> lock(gInlayMetricsMutex);
  InlayMetricsSnapshot snapshot;
  snapshot.deferredSnapshotHitCount = gInlayMetrics.deferredSnapshotHitCount;
  snapshot.deferredSnapshotMissCount = gInlayMetrics.deferredSnapshotMissCount;
  snapshot.rangeBuildSamples = gInlayMetrics.rangeBuildSamples;
  snapshot.rangeBuildTotalMs = gInlayMetrics.rangeBuildTotalMs;
  snapshot.rangeBuildMaxMs = gInlayMetrics.rangeBuildMaxMs;
  snapshot.fullBuildSamples = gInlayMetrics.fullBuildSamples;
  snapshot.fullBuildTotalMs = gInlayMetrics.fullBuildTotalMs;
  snapshot.fullBuildMaxMs = gInlayMetrics.fullBuildMaxMs;
  snapshot.rangeFilterSamples = gInlayMetrics.rangeFilterSamples;
  snapshot.rangeFilterTotalMs = gInlayMetrics.rangeFilterTotalMs;
  snapshot.rangeFilterMaxMs = gInlayMetrics.rangeFilterMaxMs;
  snapshot.responseWriteSamples = gInlayMetrics.responseWriteSamples;
  snapshot.responseWriteTotalMs = gInlayMetrics.responseWriteTotalMs;
  snapshot.responseWriteMaxMs = gInlayMetrics.responseWriteMaxMs;
  gInlayMetrics = InlayMetricState{};
  return snapshot;
}
