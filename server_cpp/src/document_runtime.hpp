#pragma once

#include "global_context_runtime.hpp"
#include "json.hpp"
#include "local_structural_runtime.hpp"
#include "semantic_cache.hpp"
#include "server_documents.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Shared current-document runtime state used by both interactive and deferred
// analysis paths.
//
// Responsibilities:
// - define the immutable analysis-context key shared by interactive and deferred
//   snapshots
// - store the currently published snapshots for one opened document
// - centralize analysis-key refresh and stale-eligibility rules
//
// Current M4 contract:
// - InteractiveSnapshot and DeferredDocSnapshot are keyed by the same
//   AnalysisSnapshotKey
// - ActiveUnitSnapshot carries the shared include / branch / defines context
//   consumed by both runtimes
// - active unit / include closure / defines / workspace summary version /
//   resource model changes must flow through this module before snapshot reuse is
//   considered
//
// Non-goals:
// - this header does not build semantic results itself
// - it does not schedule request lanes or background workers

struct HlslAstDocument;

// Immutable current-document analysis context key shared across runtime layers.
//
// `stableContextFingerprint` excludes document version / epoch and is used to
// decide whether last-good stale reuse is allowed.
// `fullFingerprint` includes document version / epoch and is used to decide
// whether a published snapshot is still current for the latest text.
struct AnalysisSnapshotKey {
  std::string documentUri;
  int documentVersion = 0;
  uint64_t documentEpoch = 0;
  std::string activeUnitPath;
  std::string activeUnitIncludeClosureFingerprint;
  std::string activeUnitBranchFingerprint;
  std::string workspaceFoldersFingerprint;
  std::string definesFingerprint;
  std::string includePathsFingerprint;
  std::string shaderExtensionsFingerprint;
  std::string resourceModelHash;
  uint64_t workspaceSummaryVersion = 0;
  std::string stableContextFingerprint;
  std::string fullFingerprint;
};

// Legacy owner/update APIs still use the ImmediateSyntaxSnapshot name, but the
// stored runtime truth is now the explicit local-structural layer.
using ImmediateSyntaxSnapshot = LocalStructuralSnapshot;

// Published current-document semantic snapshot for the current document.
struct InteractiveSnapshot {
  AnalysisSnapshotKey key;
  uint64_t documentEpoch = 0;
  int documentVersion = 0;
  std::shared_ptr<const SemanticSnapshot> semanticSnapshot;
  uint64_t builtAtMs = 0;
};

struct DeferredRangeCacheEntry {
  int startLine = 0;
  int endLine = 0;
  Json value;
};

// Published deferred semantic snapshot for the current document.
//
// For stale-eligible didChange, callers may continue carrying the previous
// `fullDiagnostics` as a last-good continuity artifact until a replacement full
// diagnostics result for the new document version is ready. Other full-document
// artifacts like semantic tokens / inlay hints can still be invalidated eagerly
// because they are not published through the same whole-document replacement
// channel.
struct DeferredDocSnapshot {
  AnalysisSnapshotKey key;
  uint64_t documentEpoch = 0;
  int documentVersion = 0;
  std::shared_ptr<const HlslAstDocument> astDocument;
  std::shared_ptr<const SemanticSnapshot> semanticSnapshot;
  Json fullDiagnostics;
  bool hasFullDiagnostics = false;
  std::string fullDiagnosticsFingerprint;
  Json semanticTokensFull;
  bool hasSemanticTokensFull = false;
  Json inlayHintsFull;
  bool hasInlayHintsFull = false;
  Json documentSymbols;
  bool hasDocumentSymbols = false;
  std::vector<DeferredRangeCacheEntry> semanticTokensRangeCache;
  std::vector<DeferredRangeCacheEntry> inlayHintsRangeCache;
  uint64_t builtAtMs = 0;
};

// Input bundle used to rebuild analysis keys after didOpen/didChange or shared
// context refresh.
//
// `globalContextOptions` are forwarded to global_context_runtime.* which owns
// the single fact source for active unit/include/macro/branch/workspace-summary
// context. `resourceModelHash` stays local here because it participates only in
// document analysis-key invalidation.
struct DocumentRuntimeUpdateOptions {
  GlobalContextRuntimeOptions globalContextOptions;
  std::string resourceModelHash;
};

// Stored runtime state for one opened document.
//
// `globalContextSnapshot` is the shared global-context source published by
// global_context_runtime.*. `activeUnitSnapshot` and `interactiveVisibilityKey`
// mirror its data so existing interactive/deferred consumers can keep reading a
// document-local runtime view without rebuilding global context themselves.
// `currentDocSemanticSnapshot` / `lastGoodCurrentDocSemanticSnapshot` /
// `deferredDocSnapshot` are the explicit layered runtime state keyed off that
// context.
//
// `semanticNeutralEditHint` marks whitespace/comment-like edits that can safely
// reuse last-good semantic state.
// `syntaxOnlyEditHint` marks small punctuation-only edits where local
// structural feedback should win over synchronous interactive prewarm on
// didChange.
struct DocumentRuntime {
  std::string uri;
  std::string text;
  int version = 0;
  uint64_t epoch = 0;
  std::shared_ptr<const GlobalContextSnapshot> globalContextSnapshot;
  AnalysisSnapshotKey analysisSnapshotKey;
  InteractiveVisibilityKey interactiveVisibilityKey;
  ActiveUnitSnapshot activeUnitSnapshot;
  std::vector<ChangedRange> changedRanges;
  bool semanticNeutralEditHint = false;
  bool syntaxOnlyEditHint = false;
  LocalStructuralSnapshot localStructuralSnapshot;
  std::shared_ptr<const InteractiveSnapshot> currentDocSemanticSnapshot;
  std::shared_ptr<const InteractiveSnapshot> lastGoodCurrentDocSemanticSnapshot;
  std::shared_ptr<const DeferredDocSnapshot> deferredDocSnapshot;
  // The diagnostics layer that currently owns the visible publish result for
  // this analysis key. Valid layers are `local-structural`,
  // `current-doc-semantic`, and `global-context`.
  //
  // `current-doc-semantic` is the continuity layer used while a new
  // global-context snapshot is not yet ready; callers may preserve the previous
  // last-good full diagnostics truth instead of letting a not-ready
  // global-context publish replace it prematurely.
  std::string lastDiagnosticsPublishLayer;
  uint64_t lastDiagnosticsPublishEpoch = 0;
  int lastDiagnosticsPublishVersion = 0;
  std::string lastDiagnosticsPublishFingerprint;
  uint64_t lastLocalStructuralPublishEpoch = 0;
  int lastLocalStructuralPublishVersion = 0;
};

// Builds the shared analysis key for one document/version/context tuple.
AnalysisSnapshotKey buildAnalysisSnapshotKey(
    const std::string &uri, int version, uint64_t epoch,
    const ActiveUnitSnapshot &activeUnitSnapshot,
    const std::string &resourceModelHash);

// Returns the current resource model hash that participates in analysis-key
// invalidation.
std::string getDocumentRuntimeResourceModelHash();

// Upserts one opened document runtime, recomputes shared context, and preserves
// only stale-eligible snapshots.
void documentRuntimeUpsert(const Document &document,
                           const std::vector<ChangedRange> &changedRanges,
                           const DocumentRuntimeUpdateOptions &options);

// Returns a copy of the stored runtime for one uri.
bool documentRuntimeGet(const std::string &uri, DocumentRuntime &runtimeOut);

// Returns whether any currently open runtime still uses this interactive
// visibility key fingerprint.
bool documentRuntimeAnyUsesInteractiveVisibilityFingerprint(
    const std::string &fullFingerprint);

// Removes one document runtime entry.
void documentRuntimeErase(const std::string &uri);

// Refreshes analysis keys for all opened documents after shared context changes.
void documentRuntimeRefreshAnalysisKeys(
    const DocumentRuntimeUpdateOptions &options,
    const std::shared_ptr<const GlobalContextSnapshot> &globalContextSnapshot =
        nullptr);

// Same as documentRuntimeRefreshAnalysisKeys, but scoped to the affected uris.
void documentRuntimeRefreshAnalysisKeysForUris(
    const std::vector<std::string> &uris,
    const DocumentRuntimeUpdateOptions &options,
    const std::shared_ptr<const GlobalContextSnapshot> &globalContextSnapshot =
        nullptr);

// Stores the latest immediate-syntax snapshot if it still matches the current
// document version.
void documentRuntimeUpdateImmediateSyntaxSnapshot(
    const std::string &uri, const ImmediateSyntaxSnapshot &snapshot);

// Stores the last diagnostics publish layer for the matching document version.
void documentRuntimeUpdateLastDiagnosticsPublishLayer(
    const std::string &uri, uint64_t documentEpoch, int documentVersion,
    const std::string &layer);

// Stores a published current-doc semantic snapshot only if it still matches the
// current full analysis key. This is the single publish point for explicit
// current-doc semantic readiness.
void documentRuntimeStoreCurrentDocSemanticSnapshot(
    const std::string &uri,
    const std::shared_ptr<const InteractiveSnapshot> &snapshot);

// Stores a published deferred snapshot only if it still matches the current
// full analysis key.
void documentRuntimeStoreDeferredSnapshot(
    const std::string &uri,
    const std::shared_ptr<const DeferredDocSnapshot> &snapshot);

// Stores a published deferred snapshot while atomically preserving additive
// same-version artifacts that concurrent requests may have written into the
// current deferred snapshot after the caller started building `snapshot`.
//
// This is intended for deferred-worker final publish paths that should not
// clobber semantic tokens / document symbols / range caches computed in
// parallel, but it must not be used for explicit invalidation stores.
void documentRuntimeMergeAndStoreDeferredSnapshot(
    const std::string &uri,
    const std::shared_ptr<const DeferredDocSnapshot> &snapshot);
