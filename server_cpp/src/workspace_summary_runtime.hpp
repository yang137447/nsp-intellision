#pragma once

#include "definition_location.hpp"
#include "json.hpp"
#include "workspace_index.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Runtime boundary around workspace_index.* for cross-file summary queries.
//
// Responsibilities:
// - expose summary-first query APIs used by request handlers and runtimes
// - own the externally visible workspace summary version that open documents use
//   in their analysis key
// - translate file-watch / rebuild / reconfigure events into workspace-index
//   updates plus version bumps
//
// Current M5 contract:
// - cross-file fallback for references / rename / workspace symbol / ambiguous
//   include-context resolution must go through this boundary instead of ad-hoc
//   include-graph scans
// - reverse-include closure used for open-document refresh also comes from this
//   boundary
//
// Non-goals:
// - does not own request scheduling or per-document caches
// - does not expose raw workspace-index internal storage to callers

// Reconfigures the workspace summary roots and bumps the externally visible
// workspace summary version consumed by document_runtime.*.
void workspaceSummaryRuntimeConfigure(
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions);

// Applies file-watch changes to the summary store. Non-empty changes bump the
// workspace summary version so affected open documents can refresh analysis
// keys.
void workspaceSummaryRuntimeHandleFileChanges(
    const std::vector<std::string> &uris);

// Best-effort single-definition lookup used by summary-first fallback paths.
bool workspaceSummaryRuntimeFindDefinition(const std::string &symbol,
                                           DefinitionLocation &outLocation);

// Best-effort single-struct-definition lookup used by summary-first fallback
// paths.
bool workspaceSummaryRuntimeFindStructDefinition(
    const std::string &symbol, DefinitionLocation &outLocation);

// Multi-definition summary query. Callers should prefer this over re-scanning
// workspace files when ambiguous or multi-hit behavior is required.
bool workspaceSummaryRuntimeFindDefinitions(
    const std::string &symbol, std::vector<IndexedDefinition> &outDefs,
    size_t limit);

// Workspace-symbol style fuzzy query over indexed summary definitions.
bool workspaceSummaryRuntimeQuerySymbols(
    const std::string &query, std::vector<IndexedDefinition> &outDefs,
    size_t limit);

// Summary-backed struct member name lookup.
bool workspaceSummaryRuntimeGetStructFields(const std::string &structName,
                                            std::vector<std::string> &outFields);

// Summary-backed struct member type lookup.
bool workspaceSummaryRuntimeGetStructMemberType(const std::string &structName,
                                                const std::string &memberName,
                                                std::string &outType);

// Summary-backed symbol type lookup.
bool workspaceSummaryRuntimeGetSymbolType(const std::string &symbol,
                                          std::string &outType);

// Returns whether the underlying workspace index can already serve a summary
// for the current configuration. A persisted startup cache may make this true
// while background validation is still running.
bool workspaceSummaryRuntimeIsReady();

// Returns the current indexing status payload forwarded from workspace_index.*.
Json workspaceSummaryRuntimeGetIndexingState();

// Nudges background indexing work without clearing disk cache.
void workspaceSummaryRuntimeKickIndexing(const std::string &reason);

// Requests a rebuild and bumps the public workspace summary version.
void workspaceSummaryRuntimeRebuild(const std::string &reason,
                                    bool clearDiskCache);

// Forwards concurrency limits to workspace_index.*.
void workspaceSummaryRuntimeSetConcurrencyLimits(size_t workerCount,
                                                 size_t queueCapacity);

// Collects the reverse-include closure for the changed provider files. This is
// the current fact source for deciding which open documents should refresh after
// file-watch events.
void workspaceSummaryRuntimeCollectReverseIncludeClosure(
    const std::vector<std::string> &uris, std::vector<std::string> &outPaths,
    size_t limit);

// Collects indexed root units that include one of the target files.
void workspaceSummaryRuntimeCollectIncludingUnits(
    const std::vector<std::string> &uris, std::vector<std::string> &outPaths,
    size_t limit);

// Collects the indexed include closure for one unit path or uri.
void workspaceSummaryRuntimeCollectIncludeClosureForUnit(
    const std::string &unitPathOrUri, std::vector<std::string> &outPaths,
    size_t limit);

// Public workspace summary version consumed by document_runtime.* analysis keys.
uint64_t workspaceSummaryRuntimeGetVersion();

// Stops the underlying workspace-index background worker.
void workspaceSummaryRuntimeShutdown();
