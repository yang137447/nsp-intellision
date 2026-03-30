#include "workspace_index_owner.hpp"

#include "active_unit.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

static int64_t nowUnixMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

static uint64_t fileTimeToUInt(const fs::file_time_type &t) {
  using namespace std::chrono;
  auto s = duration_cast<seconds>(t.time_since_epoch());
  return static_cast<uint64_t>(s.count());
}

static void sendIndexingEvent(const std::string &state, int token,
                              const std::string &kind, size_t visited,
                              size_t total,
                              const std::string &phase = std::string()) {
  Json params = makeObject();
  params.o["state"] = makeString(state);
  params.o["token"] = makeNumber(token);
  params.o["kind"] = makeString(kind);
  if (!phase.empty())
    params.o["phase"] = makeString(phase);
  const std::string unitPath = getActiveUnitPath();
  if (!unitPath.empty())
    params.o["unit"] = makeString(unitPath);
  if (visited > 0)
    params.o["visited"] = makeNumber(static_cast<double>(visited));
  if (total > 0)
    params.o["fileBudget"] = makeNumber(static_cast<double>(total));
  writeNotification("nsf/indexing", params);
}

void WorkspaceIndex::ensureThread() {
  if (threadStarted.exchange(true))
    return;
  thread = std::thread([this]() {
    try {
      run();
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        ready = false;
        pendingRunningWorkers = 0;
        pendingQueuedTasks = 0;
        indexingState = "Error";
        indexingReason = "workerException";
        indexingUpdatedAtMs = nowUnixMs();
      }
      publishIndexingStateChanged(true);
    }
  });
}

void WorkspaceIndex::indexFilesParallel(
    const std::vector<fs::path> &files,
    const std::unordered_set<std::string> &extSet,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &extensions,
    std::unordered_map<std::string, FileMeta> &outByPath,
    const std::function<void(size_t, size_t)> &onProgress) {
  outByPath.clear();
  const size_t total = files.size();
  if (total == 0)
    return;

  size_t workerLimitSnapshot = 1;
  size_t queueCapacitySnapshot = 4096;
  {
    std::lock_guard<std::mutex> lock(mutex);
    workerLimitSnapshot = limitWorkerCount;
    queueCapacitySnapshot = limitQueueCapacity;
  }
  const size_t workerCount =
      std::max(static_cast<size_t>(1), std::min(workerLimitSnapshot, total));
  const size_t queueCapacity =
      std::max(static_cast<size_t>(64), queueCapacitySnapshot);
  const size_t shardCount = std::max(static_cast<size_t>(8), workerCount * 2);

  std::vector<std::unordered_map<std::string, FileMeta>> shards(shardCount);
  std::vector<std::mutex> shardMutexes(shardCount);
  std::deque<fs::path> queue;
  std::mutex queueMutex;
  std::condition_variable queueNotEmpty;
  std::condition_variable queueNotFull;
  std::atomic<size_t> processed{0};
  std::atomic<size_t> activeWorkers{0};
  bool producerDone = false;

  auto updatePendingSnapshot = [&](size_t queued, size_t running) {
    std::lock_guard<std::mutex> lock(mutex);
    pendingQueuedTasks = queued;
    pendingRunningWorkers = running;
    indexingUpdatedAtMs = nowUnixMs();
  };

  if (workerCount <= 1) {
    outByPath.reserve(total);
    updatePendingSnapshot(total, 1);
    size_t done = 0;
    for (const auto &file : files) {
      std::string key;
      FileMeta meta;
      if (parseFileToMeta(file, extSet, workspaceFolders, includePaths,
                          extensions, key, meta)) {
        outByPath[key] = std::move(meta);
      }
      done++;
      if (done == total || done % 32 == 0)
        updatePendingSnapshot(total > done ? total - done : 0, 1);
      if (onProgress && (done == total || done % 16 == 0))
        onProgress(done, total);
    }
    updatePendingSnapshot(0, 0);
    return;
  }

  updatePendingSnapshot(total, 0);

  std::thread producer([&]() {
    try {
      for (const auto &file : files) {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueNotFull.wait(lock,
                          [&]() { return queue.size() < queueCapacity; });
        queue.push_back(file);
        queueNotEmpty.notify_one();
      }
    } catch (...) {
    }
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      producerDone = true;
    }
    queueNotEmpty.notify_all();
  });

  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (size_t worker = 0; worker < workerCount; worker++) {
    workers.emplace_back([&]() {
      while (true) {
        fs::path filePath;
        {
          std::unique_lock<std::mutex> lock(queueMutex);
          queueNotEmpty.wait(lock,
                             [&]() { return producerDone || !queue.empty(); });
          if (queue.empty()) {
            if (producerDone)
              return;
            continue;
          }
          filePath = std::move(queue.front());
          queue.pop_front();
          activeWorkers.fetch_add(1);
          queueNotFull.notify_one();
        }

        std::string key;
        FileMeta meta;
        bool parsed = false;
        try {
          parsed = parseFileToMeta(filePath, extSet, workspaceFolders,
                                   includePaths, extensions, key, meta);
        } catch (...) {
          parsed = false;
        }
        if (parsed) {
          const size_t shard = std::hash<std::string>{}(key) % shardCount;
          std::lock_guard<std::mutex> shardLock(shardMutexes[shard]);
          shards[shard][key] = std::move(meta);
        }

        const size_t done = ++processed;
        const size_t runningNow = activeWorkers.fetch_sub(1) - 1;
        if (done == total || done % 16 == 0) {
          size_t queuedNow = 0;
          {
            std::lock_guard<std::mutex> lock(queueMutex);
            queuedNow = queue.size();
          }
          updatePendingSnapshot(queuedNow, runningNow);
        }
        if (onProgress && (done == total || done % 16 == 0))
          onProgress(done, total);
      }
    });
  }

  producer.join();
  for (auto &worker : workers)
    worker.join();
  updatePendingSnapshot(0, 0);

  size_t reserveSize = 0;
  for (const auto &shard : shards)
    reserveSize += shard.size();
  outByPath.reserve(reserveSize);
  for (auto &shard : shards) {
    for (auto &entry : shard)
      outByPath[entry.first] = std::move(entry.second);
  }
}

void WorkspaceIndex::run() {
  while (true) {
    std::string localKey;
    std::vector<std::string> workspaceFolders;
    std::vector<std::string> roots;
    std::vector<std::string> includePaths;
    std::vector<std::string> exts;
    bool doRebuild = false;
    bool rebuildClearDiskCache = false;
    std::string rebuildReason;
    std::vector<std::string> changed;
    {
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait(lock, [&]() {
        return stopping || pendingRebuild || !pendingChangedUris.empty();
      });
      if (stopping)
        break;
      doRebuild = pendingRebuild;
      pendingRebuild = false;
      rebuildClearDiskCache = pendingRebuildClearDiskCache;
      pendingRebuildClearDiskCache = false;
      rebuildReason = pendingRebuildReason;
      changed.swap(pendingChangedUris);
      pendingQueuedTasks = pendingChangedUris.size();
      localKey = configuredKey;
      workspaceFolders = configuredWorkspaceFolders;
      roots = configuredRoots;
      includePaths = configuredIncludePaths;
      exts = configuredExtensions;
    }

    if (doRebuild) {
      buildAll(localKey, workspaceFolders, includePaths, roots, exts,
               rebuildReason, rebuildClearDiskCache);
      continue;
    }
    if (!changed.empty()) {
      applyFileChanges(changed);
      continue;
    }
  }
}

void WorkspaceIndex::buildAll(const std::string &key,
                              const std::vector<std::string> &workspaceFolders,
                              const std::vector<std::string> &includePaths,
                              const std::vector<std::string> &roots,
                              const std::vector<std::string> &extensions,
                              const std::string &reason,
                              bool clearDiskCache) {
  const int token = ++indexToken;
  sendIndexingEvent("begin", token, "backgroundIndex", 0, 0);
  {
    std::lock_guard<std::mutex> lock(mutex);
    pendingRunningWorkers = limitWorkerCount;
    pendingQueuedTasks = 0;
    pendingDirtyFiles = 0;
    pendingProbeRemainingBudget = 0;
    indexingState = reason == "gitStorm" ? "Validating" : "Reindexing";
    indexingReason = reason.empty() ? "manual" : reason;
    progressPhase = reason == "gitStorm" ? "Validating" : "Building";
    progressVisited = 0;
    progressTotal = 0;
    indexingUpdatedAtMs = nowUnixMs();
  }
  publishIndexingStateChanged(true);

  std::unordered_set<std::string> extSet;
  for (const auto &ext : extensions) {
    extSet.insert(toLowerCopy(ext));
  }

  IndexStore newStore;
  newStore.key = key;
  bool cacheHit = false;

  if (clearDiskCache) {
    sendIndexingEvent("update", token, "backgroundIndex", 0, 0, "clearCache");
    {
      std::lock_guard<std::mutex> lock(mutex);
      indexingState = "Reindexing";
      progressPhase = "ClearingCache";
      progressVisited = 0;
      progressTotal = 0;
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(false);
    clearIndexCacheStorage(workspaceFolders);
  } else {
    IndexStore cached;
    if (loadIndexStoreFromDisk(key, workspaceFolders, cached)) {
      newStore = std::move(cached);
      cacheHit = true;
    }
  }

  std::unordered_set<std::string> indexedPaths;

  const std::string unitPath = getActiveUnitPath();
  std::vector<std::string> includeClosure;
  collectIncludeClosureFiles(unitPath, workspaceFolders, includePaths,
                             extensions, includeClosure);
  if (!includeClosure.empty()) {
    sendIndexingEvent("update", token, "backgroundIndex", 0,
                      includeClosure.size(), "unitIncludeClosure");
    {
      std::lock_guard<std::mutex> lock(mutex);
      indexingState = "Reindexing";
      progressPhase = "Reindexing";
      progressVisited = 0;
      progressTotal = includeClosure.size();
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(false);
  }
  std::vector<fs::path> includeClosureFiles;
  includeClosureFiles.reserve(includeClosure.size());
  for (const auto &path : includeClosure) {
    fs::path filePath(path);
    if (!hasExtension(filePath, extSet))
      continue;
    const std::string fileKey = normalizePathForCompare(filePath.string());
    if (indexedPaths.find(fileKey) != indexedPaths.end())
      continue;
    includeClosureFiles.push_back(filePath);
    indexedPaths.insert(fileKey);
  }
  std::unordered_map<std::string, FileMeta> includeClosureResults;
  indexFilesParallel(
      includeClosureFiles, extSet, workspaceFolders, includePaths, extensions,
      includeClosureResults, [&](size_t visited, size_t total) {
        sendIndexingEvent("update", token, "backgroundIndex", visited, total,
                          "unitIncludeClosure");
        std::lock_guard<std::mutex> lock(mutex);
        indexingState = "Reindexing";
        progressPhase = "Reindexing";
        progressVisited = visited;
        progressTotal = total;
        indexingUpdatedAtMs = nowUnixMs();
      });
  for (auto &entry : includeClosureResults) {
    newStore.filesByPath[entry.first] = std::move(entry.second);
  }
  publishIndexingStateChanged(false);

  std::vector<std::string> orderedRoots;
  if (!unitPath.empty()) {
    addUnique(orderedRoots, unitPath);
  }
  for (const auto &root : roots) {
    if (!root.empty())
      addUnique(orderedRoots, root);
  }

  if (cacheHit) {
    const size_t total = newStore.filesByPath.size();
    sendIndexingEvent("update", token, "backgroundIndex", 0, total,
                      "cacheValidate");
    {
      std::lock_guard<std::mutex> lock(mutex);
      indexingState = "Validating";
      progressPhase = "Validating";
      progressVisited = 0;
      progressTotal = total;
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(false);
    size_t visited = 0;
    std::vector<fs::path> dirtyPaths;
    std::vector<std::string> keys;
    keys.reserve(newStore.filesByPath.size());
    for (const auto &pair : newStore.filesByPath)
      keys.push_back(pair.first);
    for (const auto &pathKey : keys) {
      std::error_code ec;
      fs::path path(pathKey);
      if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec)) {
        newStore.filesByPath.erase(pathKey);
        visited++;
        sendIndexingEvent("update", token, "backgroundIndex", visited, total,
                          "cacheValidate");
        {
          std::lock_guard<std::mutex> lock(mutex);
          indexingState = "Validating";
          progressPhase = "Validating";
          progressVisited = visited;
          progressTotal = total;
          indexingUpdatedAtMs = nowUnixMs();
        }
        publishIndexingStateChanged(false);
        continue;
      }
      if (!hasExtension(path, extSet)) {
        visited++;
        sendIndexingEvent("update", token, "backgroundIndex", visited, total,
                          "cacheValidate");
        {
          std::lock_guard<std::mutex> lock(mutex);
          indexingState = "Validating";
          progressPhase = "Validating";
          progressVisited = visited;
          progressTotal = total;
          indexingUpdatedAtMs = nowUnixMs();
        }
        publishIndexingStateChanged(false);
        continue;
      }
      uint64_t mtime = 0;
      uint64_t size = 0;
      auto t = fs::last_write_time(path, ec);
      if (!ec)
        mtime = fileTimeToUInt(t);
      ec.clear();
      size = static_cast<uint64_t>(fs::file_size(path, ec));
      auto itMeta = newStore.filesByPath.find(pathKey);
      if (itMeta != newStore.filesByPath.end()) {
        if (itMeta->second.mtime != mtime || itMeta->second.size != size) {
          dirtyPaths.push_back(path);
        }
      }
      visited++;
      sendIndexingEvent("update", token, "backgroundIndex", visited, total,
                        "cacheValidate");
      {
        std::lock_guard<std::mutex> lock(mutex);
        indexingState = "Validating";
        progressPhase = "Validating";
        progressVisited = visited;
        progressTotal = total;
        indexingUpdatedAtMs = nowUnixMs();
      }
      publishIndexingStateChanged(false);
    }

    if (!dirtyPaths.empty()) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        indexingState = "Reindexing";
        progressPhase = "Reindexing";
        progressVisited = 0;
        progressTotal = dirtyPaths.size();
        pendingDirtyFiles = dirtyPaths.size();
        indexingUpdatedAtMs = nowUnixMs();
      }
      publishIndexingStateChanged(false);
      std::unordered_map<std::string, FileMeta> dirtyResults;
      indexFilesParallel(
          dirtyPaths, extSet, workspaceFolders, includePaths, extensions,
          dirtyResults, [&](size_t done, size_t totalDirty) {
            sendIndexingEvent("update", token, "backgroundIndex", done,
                              totalDirty, "cacheReindex");
            std::lock_guard<std::mutex> lock(mutex);
            indexingState = "Reindexing";
            progressPhase = "Reindexing";
            progressVisited = done;
            progressTotal = totalDirty;
            pendingDirtyFiles = totalDirty > done ? totalDirty - done
                                                  : static_cast<size_t>(0);
            indexingUpdatedAtMs = nowUnixMs();
          });
      for (auto &entry : dirtyResults) {
        newStore.filesByPath[entry.first] = std::move(entry.second);
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        pendingDirtyFiles = 0;
      }
    }

    const auto probeTimeBudget = std::chrono::milliseconds(150);
    const size_t probeFileBudget = 400;
    const auto probeStart = std::chrono::steady_clock::now();
    size_t probed = 0;
    sendIndexingEvent("update", token, "backgroundIndex", 0, probeFileBudget,
                      "newFileProbe");
    {
      std::lock_guard<std::mutex> lock(mutex);
      indexingState = "Probing";
      progressPhase = "Probing";
      progressVisited = 0;
      progressTotal = probeFileBudget;
      pendingProbeRemainingBudget = probeFileBudget;
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(false);
    for (const auto &root : orderedRoots) {
      if (root.empty())
        continue;
      if (std::chrono::steady_clock::now() - probeStart > probeTimeBudget)
        break;
      if (probed >= probeFileBudget)
        break;
      std::error_code ec;
      fs::path base(root);
      if (!fs::exists(base, ec))
        continue;
      if (fs::is_regular_file(base, ec)) {
        if (!hasExtension(base, extSet))
          continue;
        const std::string fileKey = normalizePathForCompare(base.string());
        if (indexedPaths.find(fileKey) != indexedPaths.end())
          continue;
        indexOneFile(base, extSet, newStore);
        indexedPaths.insert(fileKey);
        continue;
      }
      if (!fs::is_directory(base, ec))
        continue;

      fs::recursive_directory_iterator it(
          base, fs::directory_options::skip_permission_denied, ec);
      fs::recursive_directory_iterator endIt;
      for (; it != endIt && !ec; it.increment(ec)) {
        if (std::chrono::steady_clock::now() - probeStart > probeTimeBudget)
          break;
        if (probed >= probeFileBudget)
          break;
        if (!it->is_regular_file(ec))
          continue;
        const fs::path filePath = it->path();
        if (!hasExtension(filePath, extSet))
          continue;
        probed++;
        const std::string fileKey = normalizePathForCompare(filePath.string());
        if (!fileKey.empty() &&
            newStore.filesByPath.find(fileKey) == newStore.filesByPath.end()) {
          indexOneFile(filePath, extSet, newStore);
        }
        sendIndexingEvent("update", token, "backgroundIndex", probed,
                          probeFileBudget, "newFileProbe");
        {
          std::lock_guard<std::mutex> lock(mutex);
          indexingState = "Probing";
          progressPhase = "Probing";
          progressVisited = probed;
          progressTotal = probeFileBudget;
          pendingProbeRemainingBudget =
              probeFileBudget > probed ? probeFileBudget - probed : 0;
          indexingUpdatedAtMs = nowUnixMs();
        }
        publishIndexingStateChanged(false);
      }
    }

    for (const auto &root : orderedRoots) {
      if (root.empty())
        continue;
      std::error_code ec;
      fs::path base(root);
      if (!fs::exists(base, ec) || !fs::is_regular_file(base, ec))
        continue;
      if (!hasExtension(base, extSet))
        continue;
      const std::string fileKey = normalizePathForCompare(base.string());
      if (indexedPaths.find(fileKey) != indexedPaths.end())
        continue;
      indexOneFile(base, extSet, newStore);
      indexedPaths.insert(fileKey);
    }
  } else {
    std::vector<fs::path> toIndexFiles;
    {
      std::lock_guard<std::mutex> lock(mutex);
      indexingState = "Reindexing";
      progressPhase = "Reindexing";
      progressVisited = 0;
      progressTotal = 0;
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(false);
    std::unordered_set<std::string> foundPaths;
    for (const auto &root : orderedRoots) {
      if (root.empty())
        continue;
      std::error_code ec;
      fs::path base(root);
      if (!fs::exists(base, ec))
        continue;
      if (fs::is_regular_file(base, ec)) {
        if (!hasExtension(base, extSet))
          continue;
        const std::string fileKey = normalizePathForCompare(base.string());
        if (foundPaths.find(fileKey) != foundPaths.end())
          continue;
        toIndexFiles.push_back(base);
        foundPaths.insert(fileKey);
        continue;
      }
      if (!fs::is_directory(base, ec))
        continue;

      fs::recursive_directory_iterator it(
          base, fs::directory_options::skip_permission_denied, ec);
      fs::recursive_directory_iterator endIt;
      for (; it != endIt && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec))
          continue;
        const fs::path filePath = it->path();
        if (!hasExtension(filePath, extSet))
          continue;
        const std::string fileKey = normalizePathForCompare(filePath.string());
        if (foundPaths.find(fileKey) != foundPaths.end())
          continue;
        toIndexFiles.push_back(filePath);
        foundPaths.insert(fileKey);
      }
    }

    std::unordered_map<std::string, FileMeta> fullResults;
    indexFilesParallel(toIndexFiles, extSet, workspaceFolders, includePaths,
                       extensions, fullResults,
                       [&](size_t done, size_t totalFiles) {
                         sendIndexingEvent("update", token, "backgroundIndex",
                                           done, totalFiles);
                         std::lock_guard<std::mutex> lock(mutex);
                         indexingState = "Reindexing";
                         progressPhase = "Reindexing";
                         progressVisited = done;
                         progressTotal = totalFiles;
                         indexingUpdatedAtMs = nowUnixMs();
                       });
    for (auto &entry : fullResults) {
      newStore.filesByPath[entry.first] = std::move(entry.second);
    }
    publishIndexingStateChanged(false);

    for (auto it = newStore.filesByPath.begin();
         it != newStore.filesByPath.end();) {
      if (foundPaths.find(normalizePathForCompare(it->first)) ==
          foundPaths.end()) {
        it = newStore.filesByPath.erase(it);
      } else {
        ++it;
      }
    }
  }

  rebuildGlobals(newStore);
  std::unordered_map<std::string, std::vector<std::string>> reverse;
  buildReverseIncludes(newStore, reverse);
  saveIndexStoreToDisk(newStore, workspaceFolders);
  {
    std::lock_guard<std::mutex> lock(mutex);
    store = std::move(newStore);
    reverseIncludeByTarget = std::move(reverse);
    ready = true;
    pendingQueuedTasks = pendingChangedUris.size();
    pendingRunningWorkers = 0;
    pendingDirtyFiles = 0;
    pendingProbeRemainingBudget = 0;
    indexingState = "Idle";
    indexingReason = reason.empty() ? "manual" : reason;
    progressPhase = "Idle";
    progressVisited = 0;
    progressTotal = 0;
    indexingUpdatedAtMs = nowUnixMs();
  }
  publishIndexingStateChanged(true);

  sendIndexingEvent("end", token, "backgroundIndex", 0, 0);
}

void WorkspaceIndex::applyFileChanges(const std::vector<std::string> &uris) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    pendingRunningWorkers =
        std::max(static_cast<size_t>(1),
                 std::min(limitWorkerCount,
                          std::max(static_cast<size_t>(1), uris.size())));
    pendingQueuedTasks = 0;
    indexingState = "Updating";
    indexingReason = "fileWatch";
    progressPhase = "Updating";
    progressVisited = 0;
    progressTotal = uris.size();
    indexingUpdatedAtMs = nowUnixMs();
  }
  publishIndexingStateChanged(false);

  IndexStore snapshot;
  std::vector<std::string> workspaceFoldersSnapshot;
  std::vector<std::string> includePathsSnapshot;
  std::vector<std::string> extensionsSnapshot;
  {
    std::lock_guard<std::mutex> lock(mutex);
    snapshot = store;
    workspaceFoldersSnapshot = configuredWorkspaceFolders;
    includePathsSnapshot = configuredIncludePaths;
    extensionsSnapshot = configuredExtensions;
  }

  std::unordered_set<std::string> extSet;
  for (const auto &ext : extensionsSnapshot)
    extSet.insert(toLowerCopy(ext));

  bool changed = false;
  std::vector<fs::path> reindexPaths;
  std::unordered_set<std::string> reindexKeys;
  size_t visited = 0;
  for (const auto &uri : uris) {
    std::string path = uriToPath(uri);
    if (path.empty())
      path = uri;
    std::error_code ec;
    fs::path filePath(path);
    const std::string pathKey = normalizePathForCompare(filePath.string());
    if (!fs::exists(filePath, ec)) {
      auto it = snapshot.filesByPath.find(pathKey);
      if (it != snapshot.filesByPath.end()) {
        snapshot.filesByPath.erase(it);
        changed = true;
      }
      visited++;
      {
        std::lock_guard<std::mutex> lock(mutex);
        progressVisited = visited;
        progressTotal = uris.size();
        indexingUpdatedAtMs = nowUnixMs();
      }
      publishIndexingStateChanged(false);
      continue;
    }
    if (!fs::is_regular_file(filePath, ec))
      continue;
    if (!hasExtension(filePath, extSet))
      continue;
    if (reindexKeys.insert(pathKey).second)
      reindexPaths.push_back(filePath);
    changed = true;
    visited++;
    {
      std::lock_guard<std::mutex> lock(mutex);
      progressVisited = visited;
      progressTotal = uris.size();
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(false);
  }

  if (!reindexPaths.empty()) {
    std::unordered_map<std::string, FileMeta> reindexed;
    indexFilesParallel(reindexPaths, extSet, workspaceFoldersSnapshot,
                       includePathsSnapshot, extensionsSnapshot, reindexed,
                       [&](size_t done, size_t total) {
                         sendIndexingEvent("update", 0, "backgroundIndex", done,
                                           total, "fileWatch");
                         std::lock_guard<std::mutex> lock(mutex);
                         progressVisited = done;
                         progressTotal = total;
                         indexingUpdatedAtMs = nowUnixMs();
                       });
    for (auto &entry : reindexed)
      snapshot.filesByPath[entry.first] = std::move(entry.second);
  }

  if (!changed) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      pendingRunningWorkers = 0;
      pendingQueuedTasks = pendingChangedUris.size();
      indexingState = pendingQueuedTasks > 0 ? "Updating" : "Idle";
      progressVisited = 0;
      progressTotal = 0;
      progressPhase = indexingState == "Idle" ? "Idle" : "Updating";
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(false);
    return;
  }

  rebuildGlobals(snapshot);
  std::unordered_map<std::string, std::vector<std::string>> reverse;
  buildReverseIncludes(snapshot, reverse);
  saveIndexStoreToDisk(snapshot, workspaceFoldersSnapshot);
  {
    std::lock_guard<std::mutex> lock(mutex);
    store = std::move(snapshot);
    reverseIncludeByTarget = std::move(reverse);
    ready = true;
    pendingRunningWorkers = 0;
    pendingQueuedTasks = pendingChangedUris.size();
    pendingDirtyFiles = 0;
    pendingProbeRemainingBudget = 0;
    indexingState = pendingQueuedTasks > 0 ? "Updating" : "Idle";
    indexingReason = "fileWatch";
    progressPhase = indexingState == "Idle" ? "Idle" : "Updating";
    progressVisited = 0;
    progressTotal = 0;
    indexingUpdatedAtMs = nowUnixMs();
  }
  publishIndexingStateChanged(true);
}

void WorkspaceIndex::indexOneFile(const fs::path &path,
                                  const std::unordered_set<std::string> &extSet,
                                  IndexStore &target) {
  const std::string rawPath = path.lexically_normal().string();
  const std::string normalizedKey = normalizePathForCompare(rawPath);
  auto existing = target.filesByPath.find(normalizedKey);
  if (existing != target.filesByPath.end()) {
    std::error_code ec;
    uint64_t mtime = 0;
    uint64_t size = 0;
    auto t = fs::last_write_time(path, ec);
    if (!ec)
      mtime = fileTimeToUInt(t);
    size = static_cast<uint64_t>(fs::file_size(path, ec));
    if (existing->second.mtime == mtime && existing->second.size == size)
      return;
  }
  std::string key;
  FileMeta meta;
  if (!parseFileToMeta(path, extSet, configuredWorkspaceFolders,
                       configuredIncludePaths, configuredExtensions, key,
                       meta)) {
    return;
  }
  target.filesByPath[key] = std::move(meta);
}
