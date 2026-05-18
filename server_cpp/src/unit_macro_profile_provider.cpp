#include "unit_macro_profile_provider.hpp"

#include "json.hpp"
#include "server_documents.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace {

namespace fs = std::filesystem;

struct ProviderFileCacheEntry {
  std::string normalizedPath;
  bool exists = false;
  uintmax_t fileSize = 0;
  fs::file_time_type lastWriteTime{};
  std::unordered_map<std::string, std::unordered_map<std::string, int>>
      unanimousDefinesByShaderKey;
};

std::mutex gUnitMacroProfileProviderMutex;
std::unordered_map<std::string, ProviderFileCacheEntry> gProviderFileCache;

std::string normalizePathForCompareCopy(const std::string &value) {
  fs::path path(value);
  std::string normalized = path.lexically_normal().string();
  if (normalized.empty())
    normalized = value;
  std::string out;
  out.reserve(normalized.size());
  for (unsigned char ch : normalized) {
    char c = static_cast<char>(std::tolower(ch));
    if (c == '\\')
      c = '/';
    out.push_back(c);
  }
  while (out.size() > 1 && out.back() == '/')
    out.pop_back();
  return out;
}

std::string normalizeExistingPathCopy(const fs::path &path) {
  return normalizePathForCompareCopy(path.lexically_normal().string());
}

void addUniquePath(std::vector<std::string> &paths, const std::string &path) {
  if (path.empty())
    return;
  const std::string normalized = normalizePathForCompareCopy(path);
  for (const auto &existing : paths) {
    if (normalizePathForCompareCopy(existing) == normalized)
      return;
  }
  paths.push_back(path);
}

void collectProviderRoots(const std::vector<std::string> &workspaceFolders,
                          const std::vector<std::string> &includePaths,
                          std::vector<std::string> &rootsOut) {
  rootsOut.clear();
  for (const auto &folder : workspaceFolders)
    addUniquePath(rootsOut, folder);
  for (const auto &includePath : includePaths) {
    addUniquePath(rootsOut, includePath);
    const fs::path parent = fs::path(includePath).parent_path();
    if (!parent.empty())
      addUniquePath(rootsOut, parent.string());
  }
}

void collectCandidateProviderPaths(
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    std::vector<std::string> &pathsOut) {
  pathsOut.clear();
  std::vector<std::string> roots;
  collectProviderRoots(workspaceFolders, includePaths, roots);
  static const std::vector<fs::path> kRelativeCandidates = {
      fs::path("gimlocalvariants.json"),
      fs::path("shadercompiler") / "check" / "check_used_shader_variants" /
          "trunk" / "gimlocalvariants.json",
  };
  for (const auto &root : roots) {
    if (root.empty())
      continue;
    for (const auto &relative : kRelativeCandidates) {
      addUniquePath(pathsOut, (fs::path(root) / relative).string());
    }
  }
}

bool tryReadIntegralValue(const Json &value, int &out) {
  if (value.type == Json::Type::Number) {
    const double number = getNumberValue(value);
    const int asInt = static_cast<int>(number);
    if (number != static_cast<double>(asInt))
      return false;
    out = asInt;
    return true;
  }
  if (value.type == Json::Type::Bool) {
    out = value.b ? 1 : 0;
    return true;
  }
  if (value.type == Json::Type::String) {
    try {
      size_t consumed = 0;
      const int parsed = std::stoi(value.s, &consumed, 0);
      if (consumed != value.s.size())
        return false;
      out = parsed;
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}

std::unordered_map<std::string, int>
extractUnanimousDefines(const Json &shaderEntry) {
  std::unordered_map<std::string, int> unanimous;
  if (shaderEntry.type != Json::Type::Object)
    return unanimous;

  bool sawVariant = false;
  for (const auto &variantEntry : shaderEntry.o) {
    if (variantEntry.second.type != Json::Type::Object)
      continue;

    std::unordered_map<std::string, int> variantDefines;
    for (const auto &defineEntry : variantEntry.second.o) {
      int value = 0;
      if (!tryReadIntegralValue(defineEntry.second, value))
        continue;
      variantDefines[defineEntry.first] = value;
    }

    if (!sawVariant) {
      unanimous = std::move(variantDefines);
      sawVariant = true;
      continue;
    }

    for (auto it = unanimous.begin(); it != unanimous.end();) {
      auto variantIt = variantDefines.find(it->first);
      if (variantIt == variantDefines.end() || variantIt->second != it->second) {
        it = unanimous.erase(it);
        continue;
      }
      ++it;
    }
  }

  if (!sawVariant)
    unanimous.clear();
  return unanimous;
}

ProviderFileCacheEntry buildProviderFileCacheEntry(const fs::path &path) {
  ProviderFileCacheEntry entry;
  entry.normalizedPath = normalizeExistingPathCopy(path);

  std::error_code ec;
  entry.exists = fs::exists(path, ec) && fs::is_regular_file(path, ec);
  if (ec || !entry.exists)
    return entry;

  entry.fileSize = fs::file_size(path, ec);
  if (ec)
    entry.fileSize = 0;
  entry.lastWriteTime = fs::last_write_time(path, ec);
  if (ec)
    entry.lastWriteTime = fs::file_time_type{};

  std::string text;
  if (!readFileText(path.string(), text))
    return entry;

  Json root;
  if (!parseJson(text, root) || root.type != Json::Type::Object)
    return entry;

  for (const auto &shaderEntry : root.o) {
    entry.unanimousDefinesByShaderKey[shaderEntry.first] =
        extractUnanimousDefines(shaderEntry.second);
  }
  return entry;
}

const ProviderFileCacheEntry &getProviderFileCacheEntry(const fs::path &path) {
  const std::string key = normalizeExistingPathCopy(path);
  std::lock_guard<std::mutex> lock(gUnitMacroProfileProviderMutex);

  auto it = gProviderFileCache.find(key);
  if (it != gProviderFileCache.end()) {
    std::error_code ec;
    const bool existsNow = fs::exists(path, ec) && fs::is_regular_file(path, ec);
    uintmax_t sizeNow = 0;
    fs::file_time_type writeTimeNow{};
    if (!ec && existsNow) {
      sizeNow = fs::file_size(path, ec);
      if (ec)
        sizeNow = 0;
      writeTimeNow = fs::last_write_time(path, ec);
      if (ec)
        writeTimeNow = fs::file_time_type{};
    }
    if (it->second.exists == existsNow && it->second.fileSize == sizeNow &&
        it->second.lastWriteTime == writeTimeNow) {
      return it->second;
    }
  }

  auto rebuilt = buildProviderFileCacheEntry(path);
  auto result = gProviderFileCache.insert_or_assign(key, std::move(rebuilt));
  return result.first->second;
}

std::string buildShaderKeyFromActiveUnitPath(const std::string &activeUnitPath) {
  if (activeUnitPath.empty())
    return std::string();
  const std::string stem = fs::path(activeUnitPath).stem().string();
  if (stem.empty())
    return std::string();
  return "shader\\" + stem + ".nfx2";
}

} // namespace

bool resolveUnitMacroProfileSnapshot(
    const std::string &activeUnitPath,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    UnitMacroProfileSnapshot &snapshotOut) {
  snapshotOut = UnitMacroProfileSnapshot{};
  snapshotOut.shaderKey = buildShaderKeyFromActiveUnitPath(activeUnitPath);
  if (snapshotOut.shaderKey.empty())
    return false;

  std::vector<std::string> candidatePaths;
  collectCandidateProviderPaths(workspaceFolders, includePaths, candidatePaths);
  for (const auto &candidate : candidatePaths) {
    const fs::path path(candidate);
    const ProviderFileCacheEntry &entry = getProviderFileCacheEntry(path);
    if (!entry.exists)
      continue;
    auto shaderIt = entry.unanimousDefinesByShaderKey.find(snapshotOut.shaderKey);
    if (shaderIt == entry.unanimousDefinesByShaderKey.end())
      continue;
    snapshotOut.foundShaderEntry = true;
    snapshotOut.sourcePath = path.lexically_normal().string();
    snapshotOut.defines = shaderIt->second;
    return true;
  }
  return false;
}

bool unitMacroProfileProviderOwnsPath(
    const std::string &pathOrUri,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths) {
  std::string path = uriToPath(pathOrUri);
  if (path.empty())
    path = pathOrUri;
  if (path.empty())
    return false;

  const std::string normalized = normalizePathForCompareCopy(path);
  std::vector<std::string> candidatePaths;
  collectCandidateProviderPaths(workspaceFolders, includePaths, candidatePaths);
  for (const auto &candidate : candidatePaths) {
    if (normalizePathForCompareCopy(candidate) == normalized)
      return true;
  }
  return false;
}
