#include "workspace_index_internal.hpp"

#include "lsp_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

uint64_t fnv1a64(const std::string &text) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char ch : text) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string hex16(uint64_t value) {
  static const char *digits = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = digits[value & 0xF];
    value >>= 4;
  }
  return out;
}

uint64_t fnv1a64Append(uint64_t hash, const std::string &text) {
  for (unsigned char ch : text) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

namespace {

uint64_t fileTimeToUInt(const fs::file_time_type &t) {
  using namespace std::chrono;
  auto s = duration_cast<seconds>(t.time_since_epoch());
  return static_cast<uint64_t>(s.count());
}

fs::path indexCacheDir(const std::vector<std::string> &workspaceFolders) {
  if (!workspaceFolders.empty()) {
    fs::path p = fs::path(workspaceFolders[0]) / ".vscode" / "nsp";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
  }
  fs::path p = fs::current_path() / "server_cpp" / ".cache" / "nsf";
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

fs::path indexV1LegacyCachePath(
    const std::vector<std::string> &workspaceFolders) {
  return indexCacheDir(workspaceFolders) / "index_v1.json";
}

fs::path indexV1CachePathForKey(const std::vector<std::string> &workspaceFolders,
                                const std::string &key) {
  const std::string name = "index_v1_" + hex16(fnv1a64(key)) + ".json";
  return indexCacheDir(workspaceFolders) / name;
}

fs::path indexV2DirPath(const std::vector<std::string> &workspaceFolders,
                        const std::string &key) {
  return indexCacheDir(workspaceFolders) / ("index_v2_" + hex16(fnv1a64(key)));
}

fs::path indexV2FilesDir(const fs::path &dir) { return dir / "files"; }

fs::path indexV2ManifestPath(const fs::path &dir) { return dir / "manifest.json"; }

std::string indexV2FileShardNameForPath(const std::string &path) {
  const std::string key = normalizePathForCompare(path);
  return hex16(fnv1a64(key)) + ".json";
}

void ensureIndexV2Dirs(const fs::path &dir) {
  std::error_code ec;
  fs::create_directories(indexV2FilesDir(dir), ec);
}

} // namespace

void clearIndexCacheStorage(const std::vector<std::string> &workspaceFolders) {
  const fs::path baseDir = indexCacheDir(workspaceFolders);
  std::error_code ec;
  if (!fs::exists(baseDir, ec) || !fs::is_directory(baseDir, ec))
    return;

  std::vector<fs::path> directoriesToRemove;
  std::vector<fs::path> filesToRemove;
  fs::directory_iterator it(baseDir, ec);
  fs::directory_iterator endIt;
  for (; it != endIt && !ec; it.increment(ec)) {
    const fs::path entryPath = it->path();
    const std::string name = entryPath.filename().string();
    if (it->is_directory(ec)) {
      if (name.rfind("index_v2_", 0) == 0)
        directoriesToRemove.push_back(entryPath);
      continue;
    }
    if (!it->is_regular_file(ec))
      continue;
    if (name == "index_v1.json" || name.rfind("index_v1_", 0) == 0)
      filesToRemove.push_back(entryPath);
  }
  ec.clear();

  for (const auto &path : filesToRemove) {
    fs::remove(path, ec);
    ec.clear();
  }
  for (const auto &path : directoriesToRemove) {
    fs::remove_all(path, ec);
    ec.clear();
  }
}

bool loadIndexStoreFromDisk(const std::string &key,
                            const std::vector<std::string> &workspaceFolders,
                            IndexStore &out) {
  auto tryLoadIndexV2 = [&](IndexStore &loadedOut) -> bool {
    const fs::path dir = indexV2DirPath(workspaceFolders, key);
    const fs::path manifestPath = indexV2ManifestPath(dir);
    std::error_code ec;
    if (!fs::exists(manifestPath, ec))
      return false;
    std::string content;
    if (!readFileToString(manifestPath.string(), content))
      return false;
    Json root;
    if (!parseJson(content, root))
      return false;
    if (root.type != Json::Type::Object)
      return false;
    const Json *verV = getObjectValue(root, "version");
    const Json *keyV = getObjectValue(root, "key");
    const Json *filesV = getObjectValue(root, "files");
    const int ver = verV ? static_cast<int>(getNumberValue(*verV)) : 0;
    if (ver != 2)
      return false;
    if (!keyV || keyV->type != Json::Type::String || keyV->s != key)
      return false;
    if (!filesV || filesV->type != Json::Type::Array)
      return false;

    IndexStore loaded;
    loaded.key = key;
    loaded.filesByPath.clear();
    const fs::path filesDir = indexV2FilesDir(dir);
    for (const auto &entry : filesV->a) {
      if (entry.type != Json::Type::Object)
        continue;
      const Json *pathV = getObjectValue(entry, "path");
      const Json *shardV = getObjectValue(entry, "shard");
      if (!pathV || pathV->type != Json::Type::String)
        continue;
      if (!shardV || shardV->type != Json::Type::String)
        continue;
      const fs::path shardPath = filesDir / shardV->s;
      if (!fs::exists(shardPath, ec))
        continue;
      std::string shardContent;
      if (!readFileToString(shardPath.string(), shardContent))
        continue;
      Json shardRoot;
      if (!parseJson(shardContent, shardRoot))
        continue;
      std::string parsedPath;
      FileMeta meta;
      if (!deserializeFileEntry(shardRoot, parsedPath, meta))
        continue;
      const std::string normalizedPath = normalizePathForCompare(pathV->s);
      if (normalizedPath.empty())
        continue;
      loaded.filesByPath[normalizedPath] = std::move(meta);
    }
    rebuildGlobals(loaded);
    loadedOut = std::move(loaded);
    return true;
  };

  auto tryLoadIndexV1 = [&](const fs::path &path, IndexStore &loadedOut) -> bool {
    std::error_code ec;
    if (!fs::exists(path, ec))
      return false;
    std::string content;
    if (!readFileToString(path.string(), content))
      return false;
    Json root;
    if (!parseJson(content, root))
      return false;
    IndexStore loaded;
    if (!deserializeIndex(root, loaded))
      return false;
    if (loaded.key != key)
      return false;
    loadedOut = std::move(loaded);
    return true;
  };

  IndexStore loaded;
  if (tryLoadIndexV2(loaded)) {
    out = std::move(loaded);
    return true;
  }

  const fs::path shardV1Path = indexV1CachePathForKey(workspaceFolders, key);
  if (tryLoadIndexV1(shardV1Path, loaded)) {
    out = loaded;
    saveIndexStoreToDisk(out, workspaceFolders);
    return true;
  }

  const fs::path legacyV1Path = indexV1LegacyCachePath(workspaceFolders);
  if (tryLoadIndexV1(legacyV1Path, loaded)) {
    out = loaded;
    saveIndexStoreToDisk(out, workspaceFolders);
    return true;
  }

  return false;
}

void saveIndexStoreToDisk(const IndexStore &store,
                          const std::vector<std::string> &workspaceFolders) {
  const fs::path dir = indexV2DirPath(workspaceFolders, store.key);
  ensureIndexV2Dirs(dir);
  const fs::path filesDir = indexV2FilesDir(dir);
  const fs::path manifestPath = indexV2ManifestPath(dir);

  std::unordered_map<std::string, std::tuple<uint64_t, uint64_t, std::string>>
      oldIndex;
  {
    std::error_code ec;
    if (fs::exists(manifestPath, ec)) {
      std::string content;
      if (readFileToString(manifestPath.string(), content)) {
        Json root;
        if (parseJson(content, root) && root.type == Json::Type::Object) {
          const Json *verV = getObjectValue(root, "version");
          const Json *keyV = getObjectValue(root, "key");
          const Json *filesV = getObjectValue(root, "files");
          const int ver = verV ? static_cast<int>(getNumberValue(*verV)) : 0;
          if (ver == 2 && keyV && keyV->type == Json::Type::String &&
              keyV->s == store.key && filesV &&
              filesV->type == Json::Type::Array) {
            for (const auto &entry : filesV->a) {
              if (entry.type != Json::Type::Object)
                continue;
              const Json *pathV = getObjectValue(entry, "path");
              const Json *mtimeV = getObjectValue(entry, "mtime");
              const Json *sizeV = getObjectValue(entry, "size");
              const Json *shardV = getObjectValue(entry, "shard");
              if (!pathV || pathV->type != Json::Type::String)
                continue;
              if (!shardV || shardV->type != Json::Type::String)
                continue;
              const uint64_t mtime =
                  mtimeV ? static_cast<uint64_t>(getNumberValue(*mtimeV)) : 0;
              const uint64_t size =
                  sizeV ? static_cast<uint64_t>(getNumberValue(*sizeV)) : 0;
              oldIndex.emplace(pathV->s,
                               std::make_tuple(mtime, size, shardV->s));
            }
          }
        }
      }
    }
  }

  Json manifest = makeObject();
  manifest.o["version"] = makeNumber(2);
  manifest.o["key"] = makeString(store.key);
  Json files = makeArray();
  std::unordered_set<std::string> keptPaths;
  keptPaths.reserve(store.filesByPath.size());

  std::error_code ec;
  for (const auto &pair : store.filesByPath) {
    const std::string &path = pair.first;
    const FileMeta &meta = pair.second;
    const std::string shardName = indexV2FileShardNameForPath(path);
    const fs::path shardPath = filesDir / shardName;
    bool shouldWrite = true;
    auto itOld = oldIndex.find(path);
    if (itOld != oldIndex.end()) {
      const auto &[oldMtime, oldSize, oldShard] = itOld->second;
      if (oldMtime == meta.mtime && oldSize == meta.size) {
        if (fs::exists(shardPath, ec))
          shouldWrite = false;
        ec.clear();
      }
    }
    if (shouldWrite) {
      Json fileRoot = serializeFileEntry(path, meta);
      std::string content = serializeJson(fileRoot);
      std::ofstream out(shardPath.string(), std::ios::binary | std::ios::trunc);
      if (out) {
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
      }
    }

    Json entry = makeObject();
    entry.o["path"] = makeString(path);
    entry.o["mtime"] = makeNumber(static_cast<double>(meta.mtime));
    entry.o["size"] = makeNumber(static_cast<double>(meta.size));
    entry.o["shard"] = makeString(shardName);
    files.a.push_back(std::move(entry));
    keptPaths.insert(path);
  }

  for (const auto &pair : oldIndex) {
    if (keptPaths.find(pair.first) != keptPaths.end())
      continue;
    const std::string &oldShard = std::get<2>(pair.second);
    fs::remove(filesDir / oldShard, ec);
    ec.clear();
  }

  manifest.o["files"] = std::move(files);
  {
    std::string content = serializeJson(manifest);
    std::ofstream out(manifestPath.string(), std::ios::binary | std::ios::trunc);
    if (out) {
      out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
  }

  fs::path baseDir = indexCacheDir(workspaceFolders);
  std::vector<std::pair<fs::path, uint64_t>> dirs;
  fs::directory_iterator dit(baseDir, ec);
  fs::directory_iterator endIt;
  for (; dit != endIt && !ec; dit.increment(ec)) {
    if (!dit->is_directory(ec))
      continue;
    const fs::path dirPath = dit->path();
    const std::string name = dirPath.filename().string();
    if (name.rfind("index_v2_", 0) != 0)
      continue;
    std::error_code ec2;
    fs::path manifest = indexV2ManifestPath(dirPath);
    auto t = fs::exists(manifest, ec2) ? fs::last_write_time(manifest, ec2)
                                       : fs::last_write_time(dirPath, ec2);
    if (ec2) {
      ec2.clear();
      continue;
    }
    dirs.emplace_back(dirPath, fileTimeToUInt(t));
  }
  ec.clear();

  const size_t keep = 8;
  if (dirs.size() > keep) {
    std::sort(dirs.begin(), dirs.end(),
              [](const auto &lhs, const auto &rhs) { return lhs.second < rhs.second; });
    for (size_t i = 0; i + keep < dirs.size(); i++) {
      fs::remove_all(dirs[i].first, ec);
      ec.clear();
    }
  }
}
