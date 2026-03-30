#pragma once

#include "json.hpp"
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

struct ChangedRange {
  int startLine = 0;
  int startCharacter = 0;
  int endLine = 0;
  int endCharacter = 0;
  int newEndLine = 0;
  int newEndCharacter = 0;
};

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

// Shared active-unit analysis preconditions for one opened document.
//
// These fields are the single fact source for interactive/deferred builds when
// an active unit exists, so callers should not rebuild include closure or
// branch context independently. `documentVersion/documentEpoch` track the open
// active-unit text that produced this snapshot so non-active documents can
// safely reuse the cached context only while the active unit itself is unchanged.
struct ActiveUnitSnapshot {
  std::string uri;
  std::string path;
  int documentVersion = 0;
  uint64_t documentEpoch = 0;
  std::vector<std::string> includeClosureUris;
  std::string includeClosureFingerprint;
  std::string activeBranchFingerprint;
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  std::unordered_map<std::string, int> defines;
  std::string workspaceFoldersFingerprint;
  std::string definesFingerprint;
  std::string includePathsFingerprint;
  std::string shaderExtensionsFingerprint;
  uint64_t workspaceSummaryVersion = 0;
};

// Latest immediate-syntax result published for the current document version.
struct ImmediateSyntaxSnapshot {
  uint64_t documentEpoch = 0;
  int documentVersion = 0;
  Json diagnostics;
  int changedWindowStartLine = 0;
  int changedWindowEndLine = 0;
  bool changedWindowOnly = false;
};

// Published interactive semantic snapshot for the current document.
struct InteractiveSnapshot {
  AnalysisSnapshotKey key;
  uint64_t documentEpoch = 0;
  int documentVersion = 0;
  std::shared_ptr<const SemanticSnapshot> semanticSnapshot;
  uint64_t builtAtMs = 0;
};

// Published deferred semantic snapshot for the current document.
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
  uint64_t builtAtMs = 0;
};

// Input bundle used to rebuild analysis keys after didOpen/didChange or shared
// context refresh.
//
// `activeUnitText/documentVersion/documentEpoch` describe the currently open
// active unit when available. They let document_runtime.* distinguish
// "same active-unit context, safe to reuse" from "active unit text changed and
// this snapshot must be rebuilt".
struct DocumentRuntimeUpdateOptions {
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  std::unordered_map<std::string, int> defines;
  std::string activeUnitText;
  int activeUnitDocumentVersion = 0;
  uint64_t activeUnitDocumentEpoch = 0;
  uint64_t workspaceSummaryVersion = 0;
  std::string resourceModelHash;
};

// Stored runtime state for one opened document.
//
// `analysisSnapshotKey` and `activeUnitSnapshot` are the current shared context.
// `interactiveSnapshot` / `lastGoodInteractiveSnapshot` / `deferredDocSnapshot`
// are published results keyed off that context.
//
// `semanticNeutralEditHint` marks whitespace/comment-like edits that can safely
// reuse last-good semantic state.
// `syntaxOnlyEditHint` marks small punctuation-only edits where immediate syntax
// feedback should win over synchronous interactive prewarm on didChange.
struct DocumentRuntime {
  std::string uri;
  std::string text;
  int version = 0;
  uint64_t epoch = 0;
  AnalysisSnapshotKey analysisSnapshotKey;
  ActiveUnitSnapshot activeUnitSnapshot;
  std::vector<ChangedRange> changedRanges;
  bool semanticNeutralEditHint = false;
  bool syntaxOnlyEditHint = false;
  ImmediateSyntaxSnapshot immediateSyntaxSnapshot;
  std::shared_ptr<const InteractiveSnapshot> interactiveSnapshot;
  std::shared_ptr<const InteractiveSnapshot> lastGoodInteractiveSnapshot;
  std::shared_ptr<const DeferredDocSnapshot> deferredDocSnapshot;
};

// Builds the shared analysis key for one document/version/context tuple.
AnalysisSnapshotKey buildAnalysisSnapshotKey(
    const std::string &uri, int version, uint64_t epoch,
    const ActiveUnitSnapshot &activeUnitSnapshot,
    const std::string &resourceModelHash);

// Returns the current resource model hash that participates in analysis-key
// invalidation.
std::string getDocumentRuntimeResourceModelHash();

// Returns / bumps the document-runtime view of workspace summary version.
uint64_t documentRuntimeGetWorkspaceSummaryVersion();
uint64_t documentRuntimeBumpWorkspaceSummaryVersion();

// Upserts one opened document runtime, recomputes shared context, and preserves
// only stale-eligible snapshots.
void documentRuntimeUpsert(const Document &document,
                           const std::vector<ChangedRange> &changedRanges,
                           const DocumentRuntimeUpdateOptions &options);

// Returns a copy of the stored runtime for one uri.
bool documentRuntimeGet(const std::string &uri, DocumentRuntime &runtimeOut);

// Removes one document runtime entry.
void documentRuntimeErase(const std::string &uri);

// Refreshes analysis keys for all opened documents after shared context changes.
void documentRuntimeRefreshAnalysisKeys(
    const DocumentRuntimeUpdateOptions &options);

// Same as documentRuntimeRefreshAnalysisKeys, but scoped to the affected uris.
void documentRuntimeRefreshAnalysisKeysForUris(
    const std::vector<std::string> &uris,
    const DocumentRuntimeUpdateOptions &options);

// Stores the latest immediate-syntax snapshot if it still matches the current
// document version.
void documentRuntimeUpdateImmediateSyntaxSnapshot(
    const std::string &uri, const ImmediateSyntaxSnapshot &snapshot);

// Stores a published interactive snapshot only if it still matches the current
// full analysis key.
void documentRuntimeStoreInteractiveSnapshot(
    const std::string &uri,
    const std::shared_ptr<const InteractiveSnapshot> &snapshot);

// Stores a published deferred snapshot only if it still matches the current
// full analysis key.
void documentRuntimeStoreDeferredSnapshot(
    const std::string &uri,
    const std::shared_ptr<const DeferredDocSnapshot> &snapshot);
