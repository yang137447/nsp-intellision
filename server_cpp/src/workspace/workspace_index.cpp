#include "workspace_index_owner.hpp"

#include "executable_path.hpp"
#include "json.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

static bool normalizeJsonTextForHash(const std::string &text,
                                     std::string &normalizedOut) {
  Json parsed;
  if (!parseJson(text, parsed))
    return false;
  normalizedOut.clear();
  normalizedOut.reserve(text.size());
  bool inString = false;
  bool escaped = false;
  for (char ch : text) {
    if (inString) {
      normalizedOut.push_back(ch);
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        inString = false;
      }
      continue;
    }
    if (ch == '"') {
      inString = true;
      escaped = false;
      normalizedOut.push_back(ch);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)))
      continue;
    normalizedOut.push_back(ch);
  }
  return true;
}

static bool computeModelsHash(std::string &hashOut, std::string &errorOut) {
  const fs::path exeDir = getExecutableDir();
  fs::path resourcesDir = exeDir / "resources";
  std::error_code ec;
  if (!fs::exists(resourcesDir, ec) || !fs::is_directory(resourcesDir, ec)) {
    resourcesDir = fs::current_path() / "server_cpp" / "resources";
  }
  if (!fs::exists(resourcesDir, ec) || !fs::is_directory(resourcesDir, ec)) {
    errorOut = "model hash resources dir not found";
    return false;
  }

  std::vector<std::string> files;
  fs::recursive_directory_iterator it(resourcesDir,
                                      fs::directory_options::skip_permission_denied,
                                      ec);
  fs::recursive_directory_iterator endIt;
  for (; it != endIt && !ec; it.increment(ec)) {
    if (!it->is_regular_file(ec)) {
      ec.clear();
      continue;
    }
    const fs::path path = it->path();
    const std::string filename = path.filename().string();
    if (filename == "base.json" || filename == "override.json") {
      files.push_back(path.lexically_normal().string());
    }
  }
  if (ec) {
    errorOut = "model hash scan failed";
    return false;
  }
  std::sort(files.begin(), files.end());

  uint64_t hash = 1469598103934665603ull;
  for (const auto &pathStr : files) {
    fs::path path(pathStr);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      errorOut = "model hash open failed: " + path.string();
      return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::string normalized;
    if (!normalizeJsonTextForHash(text, normalized)) {
      errorOut = "model hash parse failed: " + path.string();
      return false;
    }
    std::string rel = fs::relative(path, resourcesDir, ec).generic_string();
    if (ec)
      rel = path.filename().generic_string();
    hash = fnv1a64Append(hash, rel);
    hash = fnv1a64Append(hash, "\n");
    hash = fnv1a64Append(hash, normalized);
    hash = fnv1a64Append(hash, "\n");
  }

  hashOut = hex16(hash);
  errorOut.clear();
  return true;
}

bool getModelsHash(std::string &hashOut, std::string &errorOut) {
  static std::once_flag once;
  static bool ok = false;
  static std::string cachedHash;
  static std::string cachedError;
  std::call_once(once, []() { ok = computeModelsHash(cachedHash, cachedError); });
  if (!ok) {
    hashOut.clear();
    errorOut = cachedError;
    return false;
  }
  hashOut = cachedHash;
  errorOut.clear();
  return true;
}

static int64_t nowUnixMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

void WorkspaceIndex::configure(
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions) {
  std::string modelsHash;
  std::string modelsHashError;
  if (!getModelsHash(modelsHash, modelsHashError)) {
    bool shouldNotify = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      configured = false;
      configuredKey.clear();
      configuredWorkspaceFolders.clear();
      configuredRoots.clear();
      configuredIncludePaths.clear();
      configuredExtensions.clear();
      pendingRebuild = false;
      pendingRebuildClearDiskCache = false;
      pendingChangedUris.clear();
      ready = false;
      indexingEpoch++;
      indexingState = "Error";
      indexingReason = "modelsHashUnavailable";
      progressPhase = "Error";
      progressVisited = 0;
      progressTotal = 0;
      pendingQueuedTasks = 0;
      pendingRunningWorkers = 0;
      pendingDirtyFiles = 0;
      pendingProbeRemainingBudget = 0;
      indexingUpdatedAtMs = nowUnixMs();
      activeModelsHash.clear();
      activeModelsHashError = modelsHashError;
      shouldNotify = true;
    }
    if (shouldNotify)
      publishIndexingStateChanged(true);
    return;
  }

  std::vector<std::string> roots;
  for (const auto &inc : includePaths) {
    if (inc.empty())
      continue;
    if (isAbsolutePath(inc)) {
      addUnique(roots, inc);
    } else {
      for (const auto &folder : workspaceFolders) {
        if (!folder.empty())
          addUnique(roots, joinPath(folder, inc));
      }
    }
  }
  std::vector<std::string> exts = shaderExtensions;
  addUnique(exts, ".hlsli");
  addUnique(exts, ".h");

  std::string newKey = computeIndexKey(roots, includePaths, exts, modelsHash);
  bool shouldNotify = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (configuredKey == newKey && configured)
      return;
    const bool hadConfigured = configured;
    configured = true;
    configuredKey = newKey;
    configuredWorkspaceFolders = workspaceFolders;
    configuredRoots = std::move(roots);
    configuredIncludePaths = includePaths;
    configuredExtensions = std::move(exts);
    pendingRebuild = true;
    pendingRebuildClearDiskCache = false;
    pendingRebuildReason = hadConfigured ? "configChange" : "startup";
    pendingChangedUris.clear();
    ready = false;
    indexingEpoch++;
    indexingState = "Reindexing";
    indexingReason = pendingRebuildReason;
    progressPhase = "Building";
    progressVisited = 0;
    progressTotal = 0;
    pendingQueuedTasks = 1;
    pendingRunningWorkers = 0;
    pendingDirtyFiles = 0;
    pendingProbeRemainingBudget = 0;
    indexingUpdatedAtMs = nowUnixMs();
    activeModelsHash = modelsHash;
    activeModelsHashError.clear();
    shouldNotify = true;
  }
  cv.notify_one();
  ensureThread();
  if (shouldNotify)
    publishIndexingStateChanged(true);
}

void WorkspaceIndex::handleFileChanges(const std::vector<std::string> &uris) {
  bool shouldNotify = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    size_t appended = 0;
    for (const auto &uri : uris) {
      if (!uri.empty()) {
        pendingChangedUris.push_back(uri);
        appended++;
      }
    }
    if (appended > 0) {
      pendingQueuedTasks = pendingChangedUris.size();
      if (indexingState == "Idle")
        indexingState = "Updating";
      indexingReason = "fileWatch";
      progressPhase = "Updating";
      progressVisited = 0;
      progressTotal = appended;
      indexingUpdatedAtMs = nowUnixMs();
      shouldNotify = true;
    }
  }
  cv.notify_one();
  ensureThread();
  if (shouldNotify)
    publishIndexingStateChanged(false);
}

bool WorkspaceIndex::findDefinition(const std::string &symbol,
                                    DefinitionLocation &out) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = store.bestDefBySymbol.find(symbol);
  if (it == store.bestDefBySymbol.end())
    return false;
  out = it->second;
  return true;
}

bool WorkspaceIndex::findStructDefinition(const std::string &symbol,
                                          DefinitionLocation &out) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = store.bestStructDefByName.find(symbol);
  if (it == store.bestStructDefByName.end())
    return false;
  out = it->second;
  return true;
}

bool WorkspaceIndex::findDefinitions(const std::string &symbol,
                                     std::vector<IndexedDefinition> &outDefs,
                                     size_t limit) {
  outDefs.clear();
  if (limit == 0)
    return false;
  std::lock_guard<std::mutex> lock(mutex);
  auto it = store.defsBySymbol.find(symbol);
  if (it == store.defsBySymbol.end())
    return false;
  outDefs.reserve(std::min(limit, it->second.size()));
  for (size_t i = 0; i < it->second.size() && outDefs.size() < limit; i++) {
    outDefs.push_back(it->second[i]);
  }
  return !outDefs.empty();
}

bool WorkspaceIndex::querySymbols(const std::string &query,
                                  std::vector<IndexedDefinition> &outDefs,
                                  size_t limit) {
  outDefs.clear();
  if (limit == 0)
    return false;
  struct Candidate {
    int score = 0;
    IndexedDefinition def;
  };
  std::vector<Candidate> matches;
  std::string queryLower = toLowerCopy(query);
  std::lock_guard<std::mutex> lock(mutex);
  const size_t candidateBudget =
      std::max(static_cast<size_t>(64), limit * static_cast<size_t>(8));
  for (const auto &entry : store.defsBySymbol) {
    const std::string &name = entry.first;
    if (name.empty() || entry.second.empty())
      continue;

    int score = 3;
    if (!queryLower.empty()) {
      const std::string nameLower = toLowerCopy(name);
      if (nameLower == queryLower) {
        score = 0;
      } else if (nameLower.rfind(queryLower, 0) == 0) {
        score = 1;
      } else if (nameLower.find(queryLower) != std::string::npos) {
        score = 2;
      } else {
        continue;
      }
    }

    for (const auto &def : entry.second) {
      matches.push_back(Candidate{score, def});
      if (matches.size() >= candidateBudget)
        break;
    }
    if (matches.size() >= candidateBudget)
      break;
  }

  if (matches.empty())
    return false;

  std::sort(matches.begin(), matches.end(),
            [](const Candidate &lhs, const Candidate &rhs) {
              if (lhs.score != rhs.score)
                return lhs.score < rhs.score;
              if (lhs.def.name != rhs.def.name)
                return lhs.def.name < rhs.def.name;
              if (lhs.def.uri != rhs.def.uri)
                return lhs.def.uri < rhs.def.uri;
              if (lhs.def.line != rhs.def.line)
                return lhs.def.line < rhs.def.line;
              return lhs.def.start < rhs.def.start;
            });
  outDefs.reserve(std::min(limit, matches.size()));
  for (size_t i = 0; i < matches.size() && outDefs.size() < limit; i++) {
    outDefs.push_back(matches[i].def);
  }
  return !outDefs.empty();
}

bool WorkspaceIndex::getStructFields(const std::string &structName,
                                     std::vector<std::string> &outFields) {
  std::lock_guard<std::mutex> lock(mutex);
  outFields.clear();
  auto itOrdered = store.structMembersOrdered.find(structName);
  if (itOrdered == store.structMembersOrdered.end())
    return false;
  outFields.reserve(itOrdered->second.size());
  for (const auto &member : itOrdered->second) {
    if (!member.name.empty())
      outFields.push_back(member.name);
  }
  return !outFields.empty();
}

bool WorkspaceIndex::getStructMemberType(const std::string &structName,
                                         const std::string &memberName,
                                         std::string &outType) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = store.structMemberType.find(structName);
  if (it == store.structMemberType.end())
    return false;
  auto jt = it->second.find(memberName);
  if (jt == it->second.end())
    return false;
  outType = jt->second;
  return !outType.empty();
}

bool WorkspaceIndex::getSymbolType(const std::string &symbol,
                                   std::string &outType) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = store.bestTypeBySymbol.find(symbol);
  if (it == store.bestTypeBySymbol.end())
    return false;
  outType = it->second;
  return !outType.empty();
}

bool WorkspaceIndex::isReady() const {
  std::lock_guard<std::mutex> lock(mutex);
  return ready;
}

Json WorkspaceIndex::getIndexingStateSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex);
  return makeIndexingStateJsonLocked();
}

void WorkspaceIndex::setConcurrencyLimits(size_t workerCount,
                                          size_t queueCapacity) {
  std::lock_guard<std::mutex> lock(mutex);
  limitWorkerCount = std::max(static_cast<size_t>(1), workerCount);
  if (queueCapacity == 0)
    limitQueueCapacity = static_cast<size_t>(4096);
  else
    limitQueueCapacity = std::max(static_cast<size_t>(64), queueCapacity);
  indexingUpdatedAtMs = nowUnixMs();
}

void WorkspaceIndex::kickIndexing(const std::string &reason) {
  rebuildIndex(reason, false);
}

void WorkspaceIndex::rebuildIndex(const std::string &reason,
                                  bool clearDiskCache) {
  bool shouldNotify = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!configured)
      return;
    pendingRebuild = true;
    pendingRebuildClearDiskCache = clearDiskCache;
    pendingChangedUris.clear();
    pendingRebuildReason = reason.empty()
                               ? (clearDiskCache ? "manualClearCache" : "manual")
                               : reason;
    ready = false;
    indexingEpoch++;
    indexingState = clearDiskCache ? "Reindexing" : "Validating";
    indexingReason = pendingRebuildReason;
    progressPhase = clearDiskCache ? "ClearingCache" : "Validating";
    progressVisited = 0;
    progressTotal = clearDiskCache ? 0 : store.filesByPath.size();
    pendingQueuedTasks = 1;
    pendingRunningWorkers = 0;
    pendingDirtyFiles = 0;
    pendingProbeRemainingBudget = 0;
    indexingUpdatedAtMs = nowUnixMs();
    shouldNotify = true;
  }
  cv.notify_one();
  ensureThread();
  if (shouldNotify)
    publishIndexingStateChanged(true);
}

void WorkspaceIndex::collectReverseIncludeClosure(
    const std::vector<std::string> &uris, std::vector<std::string> &outPaths,
    size_t limit) const {
  outPaths.clear();
  if (limit == 0)
    return;
  std::unordered_set<std::string> visited;
  std::vector<std::string> stack;
  for (const auto &uri : uris) {
    std::string path = uriToPath(uri);
    if (path.empty())
      path = uri;
    const std::string key = normalizePathForCompare(path);
    if (key.empty())
      continue;
    if (visited.insert(key).second)
      stack.push_back(key);
  }
  if (stack.empty())
    return;

  std::lock_guard<std::mutex> lock(mutex);
  while (!stack.empty() && outPaths.size() < limit) {
    const std::string current = std::move(stack.back());
    stack.pop_back();
    auto it = reverseIncludeByTarget.find(current);
    if (it == reverseIncludeByTarget.end())
      continue;
    for (const auto &dep : it->second) {
      if (dep.empty())
        continue;
      if (!visited.insert(dep).second)
        continue;
      outPaths.push_back(dep);
      if (outPaths.size() >= limit)
        break;
      stack.push_back(dep);
    }
  }
}

void WorkspaceIndex::collectIncludingUnits(
    const std::vector<std::string> &uris, std::vector<std::string> &outPaths,
    size_t limit) const {
  outPaths.clear();
  if (limit == 0)
    return;
  std::vector<std::string> closure;
  collectReverseIncludeClosure(uris, closure, limit * 8);
  std::unordered_set<std::string> seen;
  for (const auto &path : closure) {
    if (path.empty())
      continue;
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (ext != ".nsf")
      continue;
    if (!seen.insert(path).second)
      continue;
    outPaths.push_back(path);
    if (outPaths.size() >= limit)
      break;
  }
}

void WorkspaceIndex::collectIncludeClosureForUnit(
    const std::string &unitPathOrUri, std::vector<std::string> &outPaths,
    size_t limit) const {
  outPaths.clear();
  if (limit == 0)
    return;

  std::string unitPath = uriToPath(unitPathOrUri);
  if (unitPath.empty())
    unitPath = unitPathOrUri;
  if (unitPath.empty())
    return;

  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!configured)
      return;
    workspaceFolders = configuredWorkspaceFolders;
    includePaths = configuredIncludePaths;
    shaderExtensions = configuredExtensions;
  }

  collectIncludeClosureFiles(unitPath, workspaceFolders, includePaths,
                             shaderExtensions, outPaths);
  if (outPaths.size() > limit)
    outPaths.resize(limit);
}

void WorkspaceIndex::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
    pendingRebuild = false;
    pendingRebuildClearDiskCache = false;
    pendingChangedUris.clear();
  }
  cv.notify_one();
  if (thread.joinable())
    thread.join();
}

Json WorkspaceIndex::makeIndexingStateJsonLocked() const {
  Json root = makeObject();
  root.o["epoch"] = makeNumber(static_cast<double>(indexingEpoch));
  root.o["state"] = makeString(indexingState);
  root.o["reason"] = makeString(indexingReason);
  if (!activeModelsHash.empty())
    root.o["modelsHash"] = makeString(activeModelsHash);
  if (!activeModelsHashError.empty())
    root.o["error"] = makeString(activeModelsHashError);
  root.o["updatedAtMs"] = makeNumber(static_cast<double>(indexingUpdatedAtMs));

  Json pending = makeObject();
  pending.o["queuedTasks"] = makeNumber(static_cast<double>(pendingQueuedTasks));
  pending.o["runningWorkers"] =
      makeNumber(static_cast<double>(pendingRunningWorkers));
  pending.o["dirtyFiles"] = makeNumber(static_cast<double>(pendingDirtyFiles));
  pending.o["probeRemainingBudget"] =
      makeNumber(static_cast<double>(pendingProbeRemainingBudget));
  root.o["pending"] = std::move(pending);

  Json progress = makeObject();
  progress.o["phase"] = makeString(progressPhase);
  progress.o["visited"] = makeNumber(static_cast<double>(progressVisited));
  progress.o["total"] = makeNumber(static_cast<double>(progressTotal));
  root.o["progress"] = std::move(progress);

  Json limits = makeObject();
  limits.o["workerCount"] = makeNumber(static_cast<double>(limitWorkerCount));
  limits.o["queueCapacity"] =
      makeNumber(static_cast<double>(limitQueueCapacity));
  root.o["limits"] = std::move(limits);
  return root;
}

void WorkspaceIndex::publishIndexingStateChanged(bool force) {
  Json payload;
  bool shouldSend = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    const int64_t now = nowUnixMs();
    if (force || now - lastStatePushAtMs >= 150) {
      lastStatePushAtMs = now;
      payload = makeIndexingStateJsonLocked();
      shouldSend = true;
    }
  }
  if (shouldSend)
    writeNotification("nsf/indexingStateChanged", payload);
}

static WorkspaceIndex &getIndex() {
  static WorkspaceIndex idx;
  return idx;
}

void workspaceIndexConfigure(const std::vector<std::string> &workspaceFolders,
                             const std::vector<std::string> &includePaths,
                             const std::vector<std::string> &shaderExtensions) {
  getIndex().configure(workspaceFolders, includePaths, shaderExtensions);
}

void workspaceIndexHandleFileChanges(const std::vector<std::string> &uris) {
  getIndex().handleFileChanges(uris);
}

bool workspaceIndexFindDefinition(const std::string &symbol,
                                  DefinitionLocation &outLocation) {
  return getIndex().findDefinition(symbol, outLocation);
}

bool workspaceIndexFindStructDefinition(const std::string &symbol,
                                        DefinitionLocation &outLocation) {
  return getIndex().findStructDefinition(symbol, outLocation);
}

bool workspaceIndexFindDefinitions(const std::string &symbol,
                                   std::vector<IndexedDefinition> &outDefs,
                                   size_t limit) {
  return getIndex().findDefinitions(symbol, outDefs, limit);
}

bool workspaceIndexQuerySymbols(const std::string &query,
                                std::vector<IndexedDefinition> &outDefs,
                                size_t limit) {
  return getIndex().querySymbols(query, outDefs, limit);
}

bool workspaceIndexGetStructFields(const std::string &structName,
                                   std::vector<std::string> &outFields) {
  return getIndex().getStructFields(structName, outFields);
}

bool workspaceIndexGetStructMemberType(const std::string &structName,
                                       const std::string &memberName,
                                       std::string &outType) {
  return getIndex().getStructMemberType(structName, memberName, outType);
}

bool workspaceIndexGetSymbolType(const std::string &symbol,
                                 std::string &outType) {
  return getIndex().getSymbolType(symbol, outType);
}

bool workspaceIndexIsReady() { return getIndex().isReady(); }

Json workspaceIndexGetIndexingState() {
  return getIndex().getIndexingStateSnapshot();
}

void workspaceIndexKickIndexing(const std::string &reason) {
  getIndex().kickIndexing(reason);
}

void workspaceIndexRebuild(const std::string &reason, bool clearDiskCache) {
  getIndex().rebuildIndex(reason, clearDiskCache);
}

void workspaceIndexSetConcurrencyLimits(size_t workerCount,
                                        size_t queueCapacity) {
  getIndex().setConcurrencyLimits(workerCount, queueCapacity);
}

void workspaceIndexCollectReverseIncludeClosure(
    const std::vector<std::string> &uris, std::vector<std::string> &outPaths,
    size_t limit) {
  getIndex().collectReverseIncludeClosure(uris, outPaths, limit);
}

void workspaceIndexCollectIncludingUnits(const std::vector<std::string> &uris,
                                         std::vector<std::string> &outPaths,
                                         size_t limit) {
  getIndex().collectIncludingUnits(uris, outPaths, limit);
}

void workspaceIndexCollectIncludeClosureForUnit(
    const std::string &unitPathOrUri, std::vector<std::string> &outPaths,
    size_t limit) {
  getIndex().collectIncludeClosureForUnit(unitPathOrUri, outPaths, limit);
}

void workspaceIndexShutdown() { getIndex().shutdown(); }
