#pragma once

#include "diagnostics.hpp"
#include "document_runtime.hpp"
#include "semantic_tokens.hpp"

#include <unordered_map>
#include <string>

struct ServerRequestContext;

// Background per-document runtime for deferred semantic artifacts.
//
// Responsibilities:
// - own latest-only background scheduling for deferred document work
// - materialize and cache current-doc AST / semantic snapshot / semantic tokens /
//   document symbols / full diagnostics / full-document inlay hints
// - publish only snapshots that still match the latest analysis key
//
// Non-goals:
// - not part of the interactive hot path contract
// - does not own UI formatting for individual LSP responses

struct DeferredDocBuildContext {
  // Background build input snapshot captured when the job is scheduled.
  std::unordered_map<std::string, Document> documents;
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  std::unordered_map<std::string, int> defines;
  SemanticTokenLegend semanticLegend;
  DiagnosticsBuildOptions diagnosticsOptions;
  bool prewarmFullDiagnostics = false;
  bool prewarmInlayHints = false;
  bool inlayHintsEnabled = true;
  bool inlayHintsParameterNamesEnabled = true;
};

struct DeferredDocRuntimeMetricsSnapshot {
  // `scheduled` counts enqueue attempts; scheduling the same uri again before a
  // worker picks it up increments `mergedLatestOnly` instead of letting older
  // pending work survive. The same counter also includes jobs the worker drops
  // after dequeue once a newer version of the same uri has already superseded
  // them, because those drops are still successful latest-only collapse.
  uint64_t scheduled = 0;
  uint64_t mergedLatestOnly = 0;
  uint64_t droppedStale = 0;
  uint64_t buildCount = 0;
  uint64_t queueWaitSamples = 0;
  double queueWaitTotalMs = 0.0;
  double queueWaitMaxMs = 0.0;
  double buildTotalMs = 0.0;
  double buildMaxMs = 0.0;
};

// Returns the latest deferred snapshot for the current analysis key, building
// it synchronously only when the caller has no usable published snapshot yet.
std::shared_ptr<const DeferredDocSnapshot> getOrBuildDeferredDocSnapshot(
    const std::string &uri, const Document &doc, const ServerRequestContext &ctx);

// Builds or reuses the full semantic tokens result for the current analysis key.
Json buildDeferredSemanticTokensFull(const std::string &uri, const Document &doc,
                                     const ServerRequestContext &ctx);

// Builds semantic tokens for a range by reusing the current deferred snapshot
// and slicing its full-document result when possible.
Json buildDeferredSemanticTokensRange(const std::string &uri, const Document &doc,
                                      int startLine, int startCharacter,
                                      int endLine, int endCharacter,
                                      const ServerRequestContext &ctx);

// Builds or reuses current-document symbols from the deferred snapshot layer.
Json buildDeferredDocumentSymbols(const std::string &uri, const Document &doc,
                                  const ServerRequestContext &ctx);

// Builds or reuses full semantic diagnostics for the current analysis key.
DiagnosticsBuildResult buildDeferredFullDiagnostics(
    const std::string &uri, const Document &doc, const ServerRequestContext &ctx,
    const DiagnosticsBuildOptions &options);

// Invalidates only the cached full-document inlay result for a uri. The next
// inlay request may rebuild through the deferred snapshot layer.
void deferredDocRuntimeInvalidateInlayHints(const std::string &uri);

// Enqueues deferred background work for one document.
//
// Current contract:
// - scheduling is latest-only per uri: a newer pending job replaces the older
//   pending job before build
// - stale jobs discovered by the worker must be dropped instead of published
void deferredDocRuntimeSchedule(const Document &doc,
                                const DeferredDocBuildContext &context);

// Returns and clears the aggregated runtime counters since the previous read.
DeferredDocRuntimeMetricsSnapshot takeDeferredDocRuntimeMetricsSnapshot();

// Stops the background worker and clears pending deferred jobs.
void deferredDocRuntimeShutdown();
