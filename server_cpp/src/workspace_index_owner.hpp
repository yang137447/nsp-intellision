#pragma once

#include "workspace_index_internal.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Internal owner for the mutable workspace-index runtime.
//
// Responsibilities:
// - own indexed summary state plus reverse-include cache
// - schedule rebuild / file-watch update work and publish indexing state
// - keep the public workspaceIndex* facade stable while split implementation
//   files move cache / scan / scheduler responsibilities out of workspace_index.cpp
//
// Non-goals:
// - this is not a public cross-module API; callers should continue to use the
//   workspaceIndex* free functions from workspace_index.hpp
// - it does not redefine summary facts outside workspace_index_internal.hpp
class WorkspaceIndex {
public:
  void configure(const std::vector<std::string> &workspaceFolders,
                 const std::vector<std::string> &includePaths,
                 const std::vector<std::string> &shaderExtensions);

  void handleFileChanges(const std::vector<std::string> &uris);

  bool findDefinition(const std::string &symbol, DefinitionLocation &out);

  bool findStructDefinition(const std::string &symbol,
                            DefinitionLocation &out);

  bool findDefinitions(const std::string &symbol,
                       std::vector<IndexedDefinition> &outDefs, size_t limit);

  bool querySymbols(const std::string &query,
                    std::vector<IndexedDefinition> &outDefs, size_t limit);

  bool getStructFields(const std::string &structName,
                       std::vector<std::string> &outFields);

  bool getStructMemberType(const std::string &structName,
                           const std::string &memberName,
                           std::string &outType);

  bool getSymbolType(const std::string &symbol, std::string &outType);

  bool isReady() const;

  Json getIndexingStateSnapshot() const;

  void setConcurrencyLimits(size_t workerCount, size_t queueCapacity);

  void kickIndexing(const std::string &reason);

  void rebuildIndex(const std::string &reason, bool clearDiskCache);

  void collectReverseIncludeClosure(const std::vector<std::string> &uris,
                                    std::vector<std::string> &outPaths,
                                    size_t limit) const;

  void collectIncludingUnits(const std::vector<std::string> &uris,
                             std::vector<std::string> &outPaths,
                             size_t limit) const;

  void collectIncludeClosureForUnit(const std::string &unitPathOrUri,
                                    std::vector<std::string> &outPaths,
                                    size_t limit) const;

  void shutdown();

private:
  void ensureThread();

  void indexFilesParallel(const std::vector<fs::path> &files,
                          const std::unordered_set<std::string> &extSet,
                          const std::vector<std::string> &workspaceFolders,
                          const std::vector<std::string> &includePaths,
                          const std::vector<std::string> &extensions,
                          std::unordered_map<std::string, FileMeta> &outByPath,
                          const std::function<void(size_t, size_t)> &onProgress);

  void run();

  void buildAll(const std::string &key,
                const std::vector<std::string> &workspaceFolders,
                const std::vector<std::string> &includePaths,
                const std::vector<std::string> &roots,
                const std::vector<std::string> &extensions,
                const std::string &reason, bool clearDiskCache);

  void applyFileChanges(const std::vector<std::string> &uris);

  void indexOneFile(const fs::path &path,
                    const std::unordered_set<std::string> &extSet,
                    IndexStore &target);

  Json makeIndexingStateJsonLocked() const;

  void publishIndexingStateChanged(bool force);

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::thread thread;
  std::atomic<bool> threadStarted{false};
  std::atomic<int> indexToken{0};
  bool stopping = false;

  bool configured = false;
  bool pendingRebuild = false;
  bool pendingRebuildClearDiskCache = false;
  std::string pendingRebuildReason = "startup";
  bool ready = false;
  std::string configuredKey;
  std::vector<std::string> configuredWorkspaceFolders;
  std::vector<std::string> configuredRoots;
  std::vector<std::string> configuredIncludePaths;
  std::vector<std::string> configuredExtensions;
  std::vector<std::string> pendingChangedUris;
  std::string activeModelsHash;
  std::string activeModelsHashError;
  int64_t indexingEpoch = 0;
  std::string indexingState = "Idle";
  std::string indexingReason = "startup";
  int64_t indexingUpdatedAtMs = 0;
  size_t pendingQueuedTasks = 0;
  size_t pendingRunningWorkers = 0;
  size_t pendingDirtyFiles = 0;
  size_t pendingProbeRemainingBudget = 0;
  std::string progressPhase = "Idle";
  size_t progressVisited = 0;
  size_t progressTotal = 0;
  size_t limitWorkerCount = 2;
  size_t limitQueueCapacity = 4096;
  int64_t lastStatePushAtMs = 0;
  IndexStore store;
  std::unordered_map<std::string, std::vector<std::string>>
      reverseIncludeByTarget;
};
