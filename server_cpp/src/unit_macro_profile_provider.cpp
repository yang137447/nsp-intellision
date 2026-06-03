#include "unit_macro_profile_provider.hpp"

#include "json.hpp"
#include "server_documents.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace {

namespace fs = std::filesystem;

struct ProviderFileCacheEntry {
  struct VariantRowEntry {
    std::unordered_map<std::string, int> defines;
    std::string signature;
  };

  struct ShaderResolutionEntry {
    std::vector<VariantRowEntry> variantRows;
    std::unordered_map<std::string, int> unanimousDefines;
    std::vector<std::string> unresolvedMacroNames;
  };

  std::string normalizedPath;
  bool exists = false;
  uintmax_t fileSize = 0;
  fs::file_time_type lastWriteTime{};
  std::unordered_map<std::string, ShaderResolutionEntry> byShaderKey;
  std::unordered_map<std::string, std::unordered_map<std::string, int>>
      unitSelectionHintsByStem;
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
                          const std::string &shaderCompilerPath,
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
  if (!shaderCompilerPath.empty()) {
    addUniquePath(rootsOut, shaderCompilerPath);
    const fs::path compilerPath(shaderCompilerPath);
    const fs::path parent = compilerPath.parent_path();
    if (!parent.empty())
      addUniquePath(rootsOut, parent.string());
  }
}

void collectCandidateProviderPaths(
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::string &shaderCompilerPath,
    std::vector<std::string> &pathsOut) {
  pathsOut.clear();
  std::vector<std::string> roots;
  collectProviderRoots(workspaceFolders, includePaths, shaderCompilerPath,
                       roots);
  static const std::vector<fs::path> kRelativeCandidates = {
      fs::path("gimlocalvariants.json"),
      fs::path("used_shader_variants.csv"),
      fs::path("active_unit_variant_selection.csv"),
      fs::path("check") / "check_used_shader_variants" / "trunk" /
          "gimlocalvariants.json",
      fs::path("check") / "check_used_shader_variants" / "trunk" /
          "used_shader_variants.csv",
      fs::path("check") / "check_used_shader_variants" / "trunk" /
          "active_unit_variant_selection.csv",
      fs::path("shadercompiler") / "check" / "check_used_shader_variants" /
          "trunk" / "gimlocalvariants.json",
      fs::path("shadercompiler") / "check" / "check_used_shader_variants" /
          "trunk" / "used_shader_variants.csv",
      fs::path("shadercompiler") / "check" / "check_used_shader_variants" /
          "trunk" / "active_unit_variant_selection.csv",
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

std::string buildRowSignatureFromDefines(
    const std::unordered_map<std::string, int> &defines) {
  std::vector<std::pair<std::string, int>> ordered(defines.begin(),
                                                   defines.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.first < rhs.first;
            });
  std::ostringstream oss;
  bool first = true;
  for (const auto &entry : ordered) {
    if (!first)
      oss << ";";
    first = false;
    oss << entry.first << "=" << entry.second;
  }
  return oss.str();
}

ProviderFileCacheEntry::ShaderResolutionEntry buildResolutionFromRows(
    const std::vector<ProviderFileCacheEntry::VariantRowEntry> &rows) {
  ProviderFileCacheEntry::ShaderResolutionEntry result;
  result.variantRows = rows;
  if (rows.empty())
    return result;

  result.unanimousDefines = rows.front().defines;
  std::unordered_map<std::string, int> defineOccurrenceCount;
  for (const auto &row : rows) {
    for (const auto &entry : row.defines)
      defineOccurrenceCount[entry.first]++;
    for (auto it = result.unanimousDefines.begin();
         it != result.unanimousDefines.end();) {
      auto rowIt = row.defines.find(it->first);
      if (rowIt == row.defines.end() || rowIt->second != it->second) {
        it = result.unanimousDefines.erase(it);
        continue;
      }
      ++it;
    }
  }

  const int rowCount = static_cast<int>(rows.size());
  result.unresolvedMacroNames.reserve(defineOccurrenceCount.size());
  for (const auto &entry : defineOccurrenceCount) {
    const bool appearsInAll = entry.second == rowCount;
    const bool isUnanimous =
        result.unanimousDefines.find(entry.first) !=
        result.unanimousDefines.end();
    if (!appearsInAll || !isUnanimous)
      result.unresolvedMacroNames.push_back(entry.first);
  }
  std::sort(result.unresolvedMacroNames.begin(),
            result.unresolvedMacroNames.end());
  return result;
}

ProviderFileCacheEntry::ShaderResolutionEntry
extractShaderResolutionEntry(const Json &shaderEntry) {
  if (shaderEntry.type != Json::Type::Object)
    return ProviderFileCacheEntry::ShaderResolutionEntry{};
  std::vector<ProviderFileCacheEntry::VariantRowEntry> rows;
  for (const auto &variantEntry : shaderEntry.o) {
    if (variantEntry.second.type != Json::Type::Object)
      continue;
    ProviderFileCacheEntry::VariantRowEntry row;
    for (const auto &defineEntry : variantEntry.second.o) {
      int value = 0;
      if (!tryReadIntegralValue(defineEntry.second, value))
        continue;
      row.defines[defineEntry.first] = value;
    }
    row.signature = variantEntry.first;
    if (row.signature.empty())
      row.signature = buildRowSignatureFromDefines(row.defines);
    rows.push_back(std::move(row));
  }
  return buildResolutionFromRows(rows);
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

  for (const auto &shaderEntry : root.o)
    entry.byShaderKey[shaderEntry.first] =
        extractShaderResolutionEntry(shaderEntry.second);
  return entry;
}

std::string trimCopy(const std::string &value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start]))) {
    start++;
  }
  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }
  return value.substr(start, end - start);
}

std::string shaderStemFromCsvShaderKey(const std::string &shaderKey) {
  if (shaderKey.empty())
    return std::string();
  const size_t lastSlash = shaderKey.find_last_of("\\/");
  const size_t fileStart = lastSlash == std::string::npos ? 0 : lastSlash + 1;
  size_t fileEnd = shaderKey.find("::", fileStart);
  if (fileEnd == std::string::npos)
    fileEnd = shaderKey.size();
  const std::string fileName = shaderKey.substr(fileStart, fileEnd - fileStart);
  const size_t dot = fileName.find_last_of('.');
  return dot == std::string::npos ? fileName : fileName.substr(0, dot);
}

std::string unitStemFromSelectionToken(const std::string &unitToken) {
  const std::string trimmed = trimCopy(unitToken);
  if (trimmed.empty())
    return std::string();
  const fs::path path(trimmed);
  std::string stem = path.stem().string();
  if (stem.empty())
    stem = path.filename().string();
  return stem;
}

bool parseCsvDefineToken(const std::string &token, std::string &nameOut,
                         int &valueOut) {
  nameOut.clear();
  valueOut = 0;
  const size_t equals = token.find('=');
  if (equals == std::string::npos || equals == 0 || equals + 1 >= token.size())
    return false;
  const std::string name = trimCopy(token.substr(0, equals));
  const std::string valueText = trimCopy(token.substr(equals + 1));
  if (name.empty() || valueText.empty())
    return false;
  try {
    size_t consumed = 0;
    const int parsed = std::stoi(valueText, &consumed, 0);
    if (consumed != valueText.size())
      return false;
    nameOut = name;
    valueOut = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

void mergeUnitSelectionHintRow(std::unordered_map<std::string, int> &target,
                               const std::unordered_map<std::string, int> &row,
                               bool &initialized) {
  if (!initialized) {
    target = row;
    initialized = true;
    return;
  }
  for (auto it = target.begin(); it != target.end();) {
    auto rowIt = row.find(it->first);
    if (rowIt == row.end() || rowIt->second != it->second) {
      it = target.erase(it);
      continue;
    }
    ++it;
  }
}

ProviderFileCacheEntry buildUsedVariantsCsvCacheEntry(const fs::path &path) {
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

  std::unordered_map<std::string,
                     std::vector<ProviderFileCacheEntry::VariantRowEntry>>
      rowsByStem;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    line = trimCopy(line);
    if (line.empty())
      continue;
    size_t cursor = line.find(',');
    const std::string shaderKey =
        cursor == std::string::npos ? line : line.substr(0, cursor);
    const std::string stem = shaderStemFromCsvShaderKey(shaderKey);
    if (stem.empty() || cursor == std::string::npos)
      continue;

    ProviderFileCacheEntry::VariantRowEntry row;
    std::string signature;
    while (cursor < line.size()) {
      const size_t next = line.find(',', cursor + 1);
      const std::string token = line.substr(
          cursor + 1,
          next == std::string::npos ? std::string::npos : next - cursor - 1);
      if (!signature.empty())
        signature.push_back(',');
      signature += trimCopy(token);
      std::string name;
      int value = 0;
      if (parseCsvDefineToken(token, name, value))
        row.defines[name] = value;
      if (next == std::string::npos)
        break;
      cursor = next;
    }
    row.signature = signature.empty() ? buildRowSignatureFromDefines(row.defines)
                                      : signature;
    rowsByStem[stem].push_back(std::move(row));
  }

  for (auto &entryByStem : rowsByStem) {
    entry.byShaderKey[entryByStem.first] =
        buildResolutionFromRows(entryByStem.second);
  }
  return entry;
}

ProviderFileCacheEntry
buildActiveUnitVariantSelectionCacheEntry(const fs::path &path) {
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

  std::unordered_map<std::string, std::unordered_map<std::string, int>>
      hintsByStem;
  std::unordered_map<std::string, bool> initializedByStem;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    line = trimCopy(line);
    if (line.empty())
      continue;
    size_t cursor = line.find(',');
    const std::string unitToken =
        cursor == std::string::npos ? line : line.substr(0, cursor);
    const std::string stem = unitStemFromSelectionToken(unitToken);
    if (stem.empty() || cursor == std::string::npos)
      continue;

    std::unordered_map<std::string, int> rowHints;
    while (cursor < line.size()) {
      const size_t next = line.find(',', cursor + 1);
      const std::string token = line.substr(
          cursor + 1,
          next == std::string::npos ? std::string::npos : next - cursor - 1);
      std::string name;
      int value = 0;
      if (parseCsvDefineToken(token, name, value))
        rowHints[name] = value;
      if (next == std::string::npos)
        break;
      cursor = next;
    }
    bool &initialized = initializedByStem[stem];
    mergeUnitSelectionHintRow(hintsByStem[stem], rowHints, initialized);
  }

  entry.unitSelectionHintsByStem = std::move(hintsByStem);
  return entry;
}

ProviderFileCacheEntry::ShaderResolutionEntry selectResolutionByHints(
    const ProviderFileCacheEntry::ShaderResolutionEntry &entry,
    const std::unordered_map<std::string, int> &selectionHints) {
  if (selectionHints.empty() || entry.variantRows.empty())
    return entry;

  std::vector<ProviderFileCacheEntry::VariantRowEntry> filteredRows =
      entry.variantRows;
  for (const auto &hint : selectionHints) {
    bool macroAppearsInCurrentRows = false;
    std::vector<ProviderFileCacheEntry::VariantRowEntry> candidateRows;
    candidateRows.reserve(filteredRows.size());
    for (const auto &row : filteredRows) {
      auto rowIt = row.defines.find(hint.first);
      if (rowIt == row.defines.end())
        continue;
      macroAppearsInCurrentRows = true;
      if (rowIt->second == hint.second)
        candidateRows.push_back(row);
    }
    if (macroAppearsInCurrentRows && !candidateRows.empty())
      filteredRows = std::move(candidateRows);
  }
  return buildResolutionFromRows(filteredRows);
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

  ProviderFileCacheEntry rebuilt;
  const std::string lowered = normalizeExistingPathCopy(path);
  if (lowered.size() >= 4 &&
      lowered.substr(lowered.size() - 4) == ".csv") {
    static const std::string kActiveUnitSelectionSuffix =
        "/active_unit_variant_selection.csv";
    if (lowered.size() >= kActiveUnitSelectionSuffix.size() &&
        lowered.compare(lowered.size() - kActiveUnitSelectionSuffix.size(),
                        kActiveUnitSelectionSuffix.size(),
                        kActiveUnitSelectionSuffix) == 0) {
      rebuilt = buildActiveUnitVariantSelectionCacheEntry(path);
    } else {
      rebuilt = buildUsedVariantsCsvCacheEntry(path);
    }
  } else {
    rebuilt = buildProviderFileCacheEntry(path);
  }
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
    const std::string &shaderCompilerPath,
    const std::unordered_map<std::string, int> &selectionHints,
    UnitMacroProfileSnapshot &snapshotOut) {
  snapshotOut = UnitMacroProfileSnapshot{};
  snapshotOut.shaderKey = buildShaderKeyFromActiveUnitPath(activeUnitPath);
  if (snapshotOut.shaderKey.empty())
    return false;
  const std::string activeUnitStem = fs::path(activeUnitPath).stem().string();

  std::vector<std::string> candidatePaths;
  collectCandidateProviderPaths(workspaceFolders, includePaths,
                                shaderCompilerPath, candidatePaths);

  std::unordered_map<std::string, int> mergedSelectionHints = selectionHints;
  for (const auto &candidate : candidatePaths) {
    const fs::path path(candidate);
    const ProviderFileCacheEntry &entry = getProviderFileCacheEntry(path);
    if (!entry.exists || activeUnitStem.empty())
      continue;
    auto unitHintIt = entry.unitSelectionHintsByStem.find(activeUnitStem);
    if (unitHintIt == entry.unitSelectionHintsByStem.end())
      continue;
    if (snapshotOut.profileSelectionHintSourcePath.empty())
      snapshotOut.profileSelectionHintSourcePath = path.lexically_normal().string();
    for (const auto &hint : unitHintIt->second) {
      if (mergedSelectionHints.find(hint.first) == mergedSelectionHints.end())
        mergedSelectionHints[hint.first] = hint.second;
    }
  }

  for (const auto &candidate : candidatePaths) {
    const fs::path path(candidate);
    const ProviderFileCacheEntry &entry = getProviderFileCacheEntry(path);
    if (!entry.exists)
      continue;
    auto shaderIt = entry.byShaderKey.find(snapshotOut.shaderKey);
    std::string resolvedShaderKey = snapshotOut.shaderKey;
    std::string sourceKind = "gimlocalvariants";
    if (shaderIt == entry.byShaderKey.end() &&
        !activeUnitStem.empty()) {
      shaderIt = entry.byShaderKey.find(activeUnitStem);
      if (shaderIt != entry.byShaderKey.end()) {
        resolvedShaderKey = activeUnitStem;
        sourceKind = "used_shader_variants";
      }
    }
    if (shaderIt == entry.byShaderKey.end())
      continue;
    const ProviderFileCacheEntry::ShaderResolutionEntry resolved =
        selectResolutionByHints(shaderIt->second, mergedSelectionHints);
    snapshotOut.foundShaderEntry = true;
    snapshotOut.shaderKey = resolvedShaderKey;
    snapshotOut.sourcePath = path.lexically_normal().string();
    snapshotOut.sourceKind = sourceKind;
    snapshotOut.profileTotalRowCount =
        static_cast<int>(shaderIt->second.variantRows.size());
    snapshotOut.profileSelectedRowCount =
        static_cast<int>(resolved.variantRows.size());
    if (resolved.variantRows.size() == 1)
      snapshotOut.profileSelectedRowSignature = resolved.variantRows.front().signature;
    snapshotOut.defines = resolved.unanimousDefines;
    snapshotOut.unresolvedMacroNames = resolved.unresolvedMacroNames;
    return true;
  }
  return false;
}

bool unitMacroProfileProviderOwnsPath(
    const std::string &pathOrUri,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::string &shaderCompilerPath) {
  std::string path = uriToPath(pathOrUri);
  if (path.empty())
    path = pathOrUri;
  if (path.empty())
    return false;

  const std::string normalized = normalizePathForCompareCopy(path);
  std::vector<std::string> candidatePaths;
  collectCandidateProviderPaths(workspaceFolders, includePaths,
                                shaderCompilerPath, candidatePaths);
  for (const auto &candidate : candidatePaths) {
    if (normalizePathForCompareCopy(candidate) == normalized)
      return true;
  }
  return false;
}
