#pragma once

#include "call_query.hpp"
#include "definition_location.hpp"
#include "document_runtime.hpp"
#include "member_query.hpp"
#include "semantic_snapshot.hpp"

#include <memory>
#include <string>
#include <vector>

struct ServerRequestContext;

// Current-document interactive semantic runtime.
//
// Responsibilities:
// - answer hot-path semantic requests from current-doc state first
// - expose last-good snapshot reuse only while stable context is unchanged
// - merge deferred/workspace information only after current-doc misses
//
// Current query order contract:
// current interactive snapshot -> last-good interactive snapshot ->
// deferred document snapshot -> workspace summary
//
// Non-goals:
// - does not own cross-file workspace search plans
// - does not permit workspace summary to override an already-resolved
//   current-document result

struct InteractiveRuntimeMetricsSnapshot {
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

struct InteractiveCompletionItem {
  std::string label;
  std::string detail;
  int kind = 0;
};

// Returns the latest current-doc snapshot if it matches the full analysis key,
// otherwise falls back to last-good only when the stable-context fingerprint
// still matches. This API must not trigger hidden workspace scans.
std::shared_ptr<const InteractiveSnapshot> getOrBuildInteractiveSnapshot(
    const std::string &uri, const Document &doc, const ServerRequestContext &ctx,
    bool *usedLastGoodOut = nullptr);

// Prewarms the current-doc interactive snapshot after didOpen/didChange or
// analysis-context refresh. Callers should use owner orchestration APIs instead
// of invoking this directly in parallel with runtime mutation.
void interactiveSemanticRuntimePrewarm(const std::string &uri,
                                       const Document &doc,
                                       const ServerRequestContext &ctx);

// Collects completion candidates in current-doc-first order. Workspace summary
// may add miss-only candidates but must not replace current-doc hits.
void interactiveCollectCompletionItems(
    const std::string &uri, const Document &doc, size_t cursorOffset,
    const std::string &prefix, const ServerRequestContext &ctx,
    std::vector<InteractiveCompletionItem> &outItems);

// Resolves a current-document definition target through interactive -> last-good
// -> deferred snapshots before considering workspace summary.
bool interactiveResolveDefinitionLocation(const std::string &uri,
                                          const Document &doc,
                                          const std::string &symbol,
                                          size_t cursorOffset,
                                          const ServerRequestContext &ctx,
                                          DefinitionLocation &outLocation);

// Resolves hover type information for declarations with the same snapshot order
// as other interactive semantic queries.
TypeEvalResult interactiveResolveHoverTypeAtDeclaration(
    const std::string &uri, const Document &doc, const std::string &symbol,
    size_t cursorOffset, const ServerRequestContext &ctx, bool &isParamOut);

// Resolves the base type for member access by consulting interactive semantic
// state first. Optional workspace-summary fallback is explicit in options.
MemberAccessBaseTypeResult interactiveResolveMemberAccessBaseType(
    const std::string &uri, const Document &doc, const std::string &base,
    size_t cursorOffset, const ServerRequestContext &ctx,
    const MemberAccessBaseTypeOptions &options);

// Resolves one function signature for signatureHelp in current-doc-first order.
bool interactiveResolveFunctionSignature(
    const std::string &uri, const Document &doc, const std::string &name,
    int lineIndex, int nameCharacter, const ServerRequestContext &ctx,
    std::string &labelOut, std::vector<std::string> &parametersOut);

// Collects overloads for signatureHelp in current-doc-first order.
bool interactiveResolveFunctionOverloads(
    const std::string &uri, const Document &doc, const std::string &name,
    const ServerRequestContext &ctx,
    std::vector<SemanticSnapshotFunctionOverloadInfo> &overloadsOut);

// Collects member-completion inputs from current-doc semantic state first, then
// falls back to workspace summary only for unresolved struct fields.
bool interactiveCollectMemberCompletionQuery(const std::string &uri,
                                             const std::string &ownerType,
                                             ServerRequestContext &ctx,
                                             MemberCompletionQuery &out);

// Returns and clears the aggregated runtime counters since the previous read.
InteractiveRuntimeMetricsSnapshot takeInteractiveRuntimeMetricsSnapshot();
