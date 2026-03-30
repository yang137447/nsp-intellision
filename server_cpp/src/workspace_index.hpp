#pragma once

#include "definition_location.hpp"
#include "json.hpp"

#include <string>
#include <unordered_map>
#include <vector>

// Low-level cross-file workspace index.
//
// Responsibilities:
// - maintain the persisted in-repo summary store for workspace files
// - answer summary-first cross-file queries (definitions, symbol types,
//   workspace symbol, include closures, reverse includes)
// - own background indexing / file-watch update machinery
//
// Current M5 contract:
// - references / rename / workspace symbol callers should consume this index via
//   workspace_summary_runtime.* instead of launching hot-path workspace scans
// - reverse include closure is part of the indexed summary and is the basis for
//   refreshing only impacted open documents on file-watch updates
//
// Non-goals:
// - does not schedule LSP request lanes
// - does not own current-document interactive snapshots

struct IndexedStructMember {
  std::string name;
  std::string type;
};

struct IndexedStruct {
  std::string name;
  std::vector<IndexedStructMember> members;
};

struct IndexedDefinition {
  std::string name;
  std::string type;
  std::string uri;
  int line = 0;
  int start = 0;
  int end = 0;
  int kind = 0;
};

// Configures workspace roots and shader extensions for background indexing.
void workspaceIndexConfigure(const std::vector<std::string> &workspaceFolders,
                             const std::vector<std::string> &includePaths,
                             const std::vector<std::string> &shaderExtensions);

// Applies file-watch changes to the indexed summary.
void workspaceIndexHandleFileChanges(const std::vector<std::string> &uris);

// Returns the best single definition currently known to the summary index.
bool workspaceIndexFindDefinition(const std::string &symbol,
                                  DefinitionLocation &outLocation);

// Returns the best single struct definition currently known to the summary
// index.
bool workspaceIndexFindStructDefinition(const std::string &symbol,
                                        DefinitionLocation &outLocation);

// Returns up to `limit` indexed definitions for one symbol.
bool workspaceIndexFindDefinitions(const std::string &symbol,
                                   std::vector<IndexedDefinition> &outDefs,
                                   size_t limit);

// Workspace symbol query over indexed definitions.
bool workspaceIndexQuerySymbols(const std::string &query,
                                std::vector<IndexedDefinition> &outDefs,
                                size_t limit);

// Struct-member listing from the indexed summary.
bool workspaceIndexGetStructFields(const std::string &structName,
                                   std::vector<std::string> &outFields);

// Struct-member type lookup from the indexed summary.
bool workspaceIndexGetStructMemberType(const std::string &structName,
                                       const std::string &memberName,
                                       std::string &outType);

// Best-effort symbol type lookup from the indexed summary.
bool workspaceIndexGetSymbolType(const std::string &symbol,
                                 std::string &outType);

// Returns whether the index can currently serve a summary for the active
// configuration. When a persisted cache is available this can become true
// before background validation finishes, so callers should treat it as
// "queryable now" rather than "all validation work is complete".
bool workspaceIndexIsReady();

// Returns the current indexing status payload.
Json workspaceIndexGetIndexingState();

// Requests additional indexing work without clearing disk cache.
void workspaceIndexKickIndexing(const std::string &reason);

// Requests a rebuild, optionally clearing disk cache.
void workspaceIndexRebuild(const std::string &reason, bool clearDiskCache);

// Sets background indexing worker/queue limits.
void workspaceIndexSetConcurrencyLimits(size_t workerCount,
                                        size_t queueCapacity);

// Returns the indexed reverse-include closure for the changed provider files.
void workspaceIndexCollectReverseIncludeClosure(
    const std::vector<std::string> &uris, std::vector<std::string> &outPaths,
    size_t limit);

// Returns indexed root units that include one of the target files.
void workspaceIndexCollectIncludingUnits(const std::vector<std::string> &uris,
                                         std::vector<std::string> &outPaths,
                                         size_t limit);

// Returns the indexed include closure for a single unit path or uri.
void workspaceIndexCollectIncludeClosureForUnit(const std::string &unitPathOrUri,
                                                std::vector<std::string> &outPaths,
                                                size_t limit);

// Stops background indexing workers and clears pending work.
void workspaceIndexShutdown();
