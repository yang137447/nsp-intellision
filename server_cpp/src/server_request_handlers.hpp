#pragma once

#include "definition_location.hpp"
#include "json.hpp"
#include "semantic_tokens.hpp"
#include "server_documents.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Request-layer boundary for the core LSP handlers.
//
// Responsibilities:
// - expose the immutable request-scoped snapshot consumed by handlers
// - keep the current scheduling contract readable at the API boundary
// - dispatch core request methods onto current-doc / deferred / workspace
//   runtimes without making handlers own long-lived document state
//
// Non-goals:
// - this header is not the owner of per-document caches or snapshot lifetime
// - it does not define resource facts; handlers must still defer to shared
//   registries and query modules for language knowledge

struct ServerRequestContext {
  // Request-scoped document/config snapshot. Handlers should treat this as
  // read-only input and must not mutate document runtime state through it.
  std::unordered_map<std::string, Document> documents;
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  SemanticTokenLegend semanticLegend;
  bool inlayHintsEnabled = true;
  bool inlayHintsParameterNamesEnabled = true;
  bool semanticTokensEnabled = true;
  bool diagnosticsExpensiveRulesEnabled = true;
  int diagnosticsTimeBudgetMs = 1200;
  int diagnosticsMaxItems = 1200;
  bool diagnosticsFastEnabled = true;
  int diagnosticsFastDelayMs = 90;
  int diagnosticsFastTimeBudgetMs = 180;
  int diagnosticsFastMaxItems = 240;
  bool diagnosticsFullEnabled = true;
  int diagnosticsFullDelayMs = 700;
  bool diagnosticsFullExpensiveRulesEnabled = true;
  int diagnosticsFullTimeBudgetMs = 1200;
  int diagnosticsFullMaxItems = 1200;
  int diagnosticsWorkerCount = 2;
  bool diagnosticsAutoWorkerCount = true;
  bool semanticCacheEnabled = true;
  bool diagnosticsIndeterminateEnabled = true;
  int diagnosticsIndeterminateSeverity = 4;
  int diagnosticsIndeterminateMaxItems = 200;
  bool diagnosticsIndeterminateSuppressWhenErrors = true;
  int indexingWorkerCount = 16;
  int indexingQueueCapacity = 4096;
  std::unordered_map<std::string, int> preprocessorDefines;
  std::function<bool()> isCancellationRequested;

  const std::unordered_map<std::string, Document> &documentSnapshot() const {
    return documents;
  }

  const Document *findDocument(const std::string &uri) const {
    auto it = documents.find(uri);
    if (it == documents.end())
      return nullptr;
    return &it->second;
  }

  bool readDocumentText(const std::string &uri, std::string &text) const {
    return loadDocumentText(uri, documents, text);
  }
};

struct SignatureHelpMetricsSnapshot {
  uint64_t indeterminateTotal = 0;
  uint64_t indeterminateReasonCallTargetUnknown = 0;
  uint64_t indeterminateReasonDefinitionTextUnavailable = 0;
  uint64_t indeterminateReasonSignatureExtractFailed = 0;
  uint64_t indeterminateReasonOther = 0;
  uint64_t overloadResolverAttempts = 0;
  uint64_t overloadResolverResolved = 0;
  uint64_t overloadResolverAmbiguous = 0;
  uint64_t overloadResolverNoViable = 0;
  uint64_t overloadResolverShadowMismatch = 0;
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

void recordSignatureHelpInteractiveOverloads(double durationMs);
void recordSignatureHelpBuiltinSignature(double durationMs);
void recordSignatureHelpResponseWrite(double durationMs);
SignatureHelpMetricsSnapshot takeSignatureHelpMetricsSnapshot();

struct CompletionMetricsSnapshot {
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

void recordCompletionInteractiveCollect(double durationMs);
void recordCompletionWorkspaceSummaryQuery(double durationMs);
void recordCompletionItemAssembly(double durationMs);
void recordCompletionResponseWrite(double durationMs);
void recordCompletionMemberAccessDetected();
void recordCompletionMemberTypeResolved();
void recordCompletionMemberItemsReturned();
void recordCompletionMemberGenericFallback();
void recordCompletionMemberBaseResolve(double durationMs);
void recordCompletionMemberQuery(double durationMs);
CompletionMetricsSnapshot takeCompletionMetricsSnapshot();

struct CompletionDebugSnapshot {
  bool memberAccessDetected = false;
  int line = -1;
  int character = -1;
  std::string lineText;
  std::string base;
  std::string member;
  bool memberTypeResolved = false;
  std::string resolvedType;
  bool memberItemsReturned = false;
  uint64_t fieldCount = 0;
  uint64_t methodCount = 0;
  std::string path;
};

void updateLastCompletionDebugSnapshot(const CompletionDebugSnapshot &snapshot);
CompletionDebugSnapshot getLastCompletionDebugSnapshot();

struct DefinitionMetricsSnapshot {
  uint64_t currentDocInteractiveSamples = 0;
  double currentDocInteractiveTotalMs = 0.0;
  double currentDocInteractiveMaxMs = 0.0;
  uint64_t currentUnitCallSamples = 0;
  double currentUnitCallTotalMs = 0.0;
  double currentUnitCallMaxMs = 0.0;
  uint64_t responseWriteSamples = 0;
  double responseWriteTotalMs = 0.0;
  double responseWriteMaxMs = 0.0;
};

void recordDefinitionCurrentDocInteractive(double durationMs);
void recordDefinitionCurrentUnitCall(double durationMs);
void recordDefinitionResponseWrite(double durationMs);
DefinitionMetricsSnapshot takeDefinitionMetricsSnapshot();

struct HoverMetricsSnapshot {
  uint64_t currentDocDeclarationSamples = 0;
  double currentDocDeclarationTotalMs = 0.0;
  double currentDocDeclarationMaxMs = 0.0;
  uint64_t requestSetupSamples = 0;
  double requestSetupTotalMs = 0.0;
  double requestSetupMaxMs = 0.0;
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

void recordHoverRequestSetup(double durationMs);
void recordHoverCurrentDocDeclaration(double durationMs);
void recordHoverCurrentDocFunction(double durationMs);
void recordHoverIncludeContextSummary(double durationMs);
void recordHoverBuiltinDoc(double durationMs);
void recordHoverMarkdownRender(double durationMs);
void recordHoverResponseWrite(double durationMs);
HoverMetricsSnapshot takeHoverMetricsSnapshot();

struct InlayMetricsSnapshot {
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

void recordInlayDeferredSnapshotHit();
void recordInlayDeferredSnapshotMiss();
void recordInlayRangeBuild(double durationMs);
void recordInlayFullBuild(double durationMs);
void recordInlayRangeFilter(double durationMs);
void recordInlayResponseWrite(double durationMs);
InlayMetricsSnapshot takeInlayMetricsSnapshot();

// Dispatches the core LSP request methods owned by server_request_handlers.cpp.
//
// Current M1 scheduling contract:
// - interactive high-priority path: completion, hover, signature help, and
//   current-document short-path definition
// - background latest-only + cancellable path: semantic tokens, inlay hints,
//   document symbols, references, prepareRename, rename, workspace symbol
//
// Handler implementations may consult workspace summary on current-doc miss, but
// interactive requests must not reintroduce include-graph scans or full
// workspace rescans as hidden hot-path fallback.
bool handleCoreRequestMethods(const std::string &method, const Json &id,
                              const Json *params, ServerRequestContext &ctx,
                              const std::vector<std::string> &keywords,
                              const std::vector<std::string> &directives);
