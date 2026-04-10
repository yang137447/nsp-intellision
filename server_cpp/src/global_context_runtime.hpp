#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Shared edit-time range description used by document runtime invalidation and
// global-context reuse heuristics.
struct ChangedRange {
  int startLine = 0;
  int startCharacter = 0;
  int endLine = 0;
  int endCharacter = 0;
  int newEndLine = 0;
  int newEndCharacter = 0;
};

// Shared active-unit analysis preconditions consumed by interactive/deferred
// document runtimes and shared-visible queries.
//
// This snapshot is the single fact source for active unit selection, active
// include closure, active branch state, and macro/configuration inputs. Feature
// code should consume it instead of rebuilding ad-hoc context.
struct ActiveUnitSnapshot {
  std::string uri;
  std::string path;
  int documentVersion = 0;
  uint64_t documentEpoch = 0;
  std::vector<char> activeLineStates;
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

// Shared key that scopes cross-file interactive visibility shards.
struct InteractiveVisibilityKey {
  std::string activeUnitPath;
  std::string includeClosureFingerprint;
  std::string activeBranchFingerprint;
  std::string definesFingerprint;
  uint64_t workspaceSummaryVersion = 0;
  std::string fullFingerprint;
};

// Inputs required to build the shared global context snapshot.
//
// `activeUnitText/documentVersion/documentEpoch` describe the currently opened
// active unit when available. The runtime may reuse a cached snapshot across
// comment/whitespace edits that do not affect preprocessor state, but only this
// module decides when that reuse is safe.
struct GlobalContextRuntimeOptions {
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  std::unordered_map<std::string, int> defines;
  std::string activeUnitText;
  int activeUnitDocumentVersion = 0;
  uint64_t activeUnitDocumentEpoch = 0;
  uint64_t workspaceSummaryVersion = 0;
};

// Shared runtime snapshot that represents the currently selected global context
// for editing-time analysis.
struct GlobalContextSnapshot {
  // Debug/logical identity for this preserved global context. This is stable
  // across preprocessor-neutral active-unit edits when the logical context is
  // unchanged, even if the snapshot needs refreshed active-unit version/epoch
  // metadata.
  uint64_t debugLogicalId = 0;
  ActiveUnitSnapshot activeUnitSnapshot;
  InteractiveVisibilityKey interactiveVisibilityKey;
};

// Rebuilds or reuses the shared global context snapshot.
//
// Callers should provide the changed document when the current event is a
// `didChange`/`didOpen` for the active unit so the runtime can keep the old
// snapshot across preprocessor-neutral edits. For other callers, pass empty
// `changedDocumentUri` and `changedRanges`.
std::shared_ptr<const GlobalContextSnapshot> globalContextRuntimeRefresh(
    const GlobalContextRuntimeOptions &options,
    const std::string &changedDocumentUri = std::string(),
    const std::vector<ChangedRange> *changedRanges = nullptr);

// Returns whether this snapshot currently has enough data for active-unit
// global-context consumers. Workspace-summary readiness is checked live so the
// answer can flip to ready when background indexing finishes, without forcing
// every document runtime to rebuild.
bool globalContextRuntimeIsReady(const GlobalContextSnapshot &snapshot);
