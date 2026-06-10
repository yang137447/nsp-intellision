#include "scalar_type_model.hpp"

#include "json.hpp"
#include "resource_registry.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

struct ScalarTypeEntry {
  std::string name;
  HlslScalarTypeKind kind = HlslScalarTypeKind::Unknown;
  bool generateVector = false;
  bool generateMatrix = false;
  std::vector<int> vectorDimensions;
  std::vector<int> matrixRows;
  std::vector<int> matrixColumns;
  bool minPrecision = false;
  bool legacy = false;
};

enum class ScalarTypeModelState { Uninitialized, Loaded, Unavailable };

std::once_flag gScalarTypeModelOnce;
ScalarTypeModelState gScalarTypeModelState =
    ScalarTypeModelState::Uninitialized;
std::string gScalarTypeModelError;
std::vector<std::string> gBaseNames;
std::vector<std::string> gGeneratedNames;
std::unordered_map<std::string, ScalarTypeEntry> gEntriesByLower;
std::unordered_set<std::string> gBaseNameLookup;
std::unordered_set<std::string> gGeneratedNameLookup;

std::string trimCopy(std::string value) {
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin]))) {
    begin++;
  }
  size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }
  return value.substr(begin, end - begin);
}

std::string toLowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string normalizeTypeName(const std::string &value) {
  return toLowerAscii(trimCopy(value));
}

HlslScalarTypeKind parseKind(const std::string &value) {
  if (value == "bool")
    return HlslScalarTypeKind::Bool;
  if (value == "integer")
    return HlslScalarTypeKind::Integer;
  if (value == "floating")
    return HlslScalarTypeKind::Floating;
  if (value == "special")
    return HlslScalarTypeKind::Special;
  return HlslScalarTypeKind::Unknown;
}

std::vector<int> parseIntArray(const Json *value) {
  std::vector<int> out;
  if (!value || value->type != Json::Type::Array)
    return out;
  for (const auto &item : value->a) {
    if (item.type != Json::Type::Number)
      continue;
    out.push_back(static_cast<int>(getNumberValue(item, 0)));
  }
  return out;
}

bool containsDimension(const std::vector<int> &dimensions, int value) {
  return std::find(dimensions.begin(), dimensions.end(), value) !=
         dimensions.end();
}

bool applyEntries(const Json &root,
                  std::unordered_map<std::string, ScalarTypeEntry> &entries,
                  std::string &errorOut) {
  const Json *entriesValue = getObjectValue(root, "entries");
  if (!entriesValue || entriesValue->type != Json::Type::Array) {
    errorOut = "scalar type entries missing or invalid";
    return false;
  }
  for (const auto &entryValue : entriesValue->a) {
    const Json *nameValue = getObjectValue(entryValue, "name");
    const std::string name = nameValue ? getStringValue(*nameValue) : "";
    if (name.empty())
      continue;
    const std::string key = normalizeTypeName(name);
    const Json *disabledValue = getObjectValue(entryValue, "disabled");
    if (disabledValue && getBoolValue(*disabledValue, false)) {
      entries.erase(key);
      continue;
    }

    ScalarTypeEntry entry;
    entry.name = key;
    const Json *kindValue = getObjectValue(entryValue, "kind");
    entry.kind = parseKind(kindValue ? getStringValue(*kindValue) : "");
    const Json *generateVectorValue =
        getObjectValue(entryValue, "generateVector");
    entry.generateVector =
        generateVectorValue ? getBoolValue(*generateVectorValue, false) : false;
    const Json *generateMatrixValue =
        getObjectValue(entryValue, "generateMatrix");
    entry.generateMatrix =
        generateMatrixValue ? getBoolValue(*generateMatrixValue, false) : false;
    entry.vectorDimensions =
        parseIntArray(getObjectValue(entryValue, "vectorDimensions"));
    entry.matrixRows = parseIntArray(getObjectValue(entryValue, "matrixRows"));
    entry.matrixColumns =
        parseIntArray(getObjectValue(entryValue, "matrixColumns"));
    const Json *minPrecisionValue =
        getObjectValue(entryValue, "minPrecision");
    entry.minPrecision =
        minPrecisionValue ? getBoolValue(*minPrecisionValue, false) : false;
    const Json *legacyValue = getObjectValue(entryValue, "legacy");
    entry.legacy = legacyValue ? getBoolValue(*legacyValue, false) : false;
    entries[key] = std::move(entry);
  }
  return true;
}

void addUnique(std::vector<std::string> &items,
               std::unordered_set<std::string> &lookup,
               const std::string &value) {
  if (value.empty())
    return;
  if (lookup.insert(value).second)
    items.push_back(value);
}

bool loadScalarTypeModel() {
  Json baseRoot;
  Json overrideRoot;
  std::string error;
  if (!loadResourceBundleJson("types/scalar_types", baseRoot, overrideRoot,
                              error)) {
    gScalarTypeModelError = error;
    return false;
  }

  std::unordered_map<std::string, ScalarTypeEntry> entries;
  if (!applyEntries(baseRoot, entries, error) ||
      !applyEntries(overrideRoot, entries, error)) {
    gScalarTypeModelError = error;
    return false;
  }
  if (entries.empty()) {
    gScalarTypeModelError = "merged scalar type model empty";
    return false;
  }

  std::vector<std::string> baseNames;
  std::vector<std::string> generatedNames;
  std::unordered_set<std::string> baseLookup;
  std::unordered_set<std::string> generatedLookup;
  for (const auto &pair : entries) {
    const ScalarTypeEntry &entry = pair.second;
    addUnique(baseNames, baseLookup, entry.name);
    addUnique(generatedNames, generatedLookup, entry.name);
    if (entry.generateVector) {
      for (int dim : entry.vectorDimensions) {
        addUnique(generatedNames, generatedLookup,
                  entry.name + std::to_string(dim));
      }
    }
    if (entry.generateMatrix) {
      for (int rows : entry.matrixRows) {
        for (int cols : entry.matrixColumns) {
          addUnique(generatedNames, generatedLookup,
                    entry.name + std::to_string(rows) + "x" +
                        std::to_string(cols));
        }
      }
    }
  }
  std::sort(baseNames.begin(), baseNames.end());
  std::sort(generatedNames.begin(), generatedNames.end());

  gEntriesByLower = std::move(entries);
  gBaseNames = std::move(baseNames);
  gGeneratedNames = std::move(generatedNames);
  gBaseNameLookup = std::move(baseLookup);
  gGeneratedNameLookup = std::move(generatedLookup);
  gScalarTypeModelError.clear();
  return true;
}

void ensureScalarTypeModelLoaded() {
  std::call_once(gScalarTypeModelOnce, []() {
    if (loadScalarTypeModel()) {
      gScalarTypeModelState = ScalarTypeModelState::Loaded;
      return;
    }
    gScalarTypeModelState = ScalarTypeModelState::Unavailable;
  });
}

const ScalarTypeEntry *findEntry(const std::string &typeName) {
  ensureScalarTypeModelLoaded();
  if (gScalarTypeModelState != ScalarTypeModelState::Loaded)
    return nullptr;
  const std::string key = normalizeTypeName(typeName);
  auto it = gEntriesByLower.find(key);
  if (it == gEntriesByLower.end())
    return nullptr;
  return &it->second;
}

bool fillShapeForEntry(const ScalarTypeEntry &entry,
                       HlslScalarTypeShapeKind shapeKind, int rows, int cols,
                       HlslScalarTypeShape &outShape) {
  outShape = HlslScalarTypeShape{};
  outShape.shape = shapeKind;
  outShape.kind = entry.kind;
  outShape.baseName = entry.name;
  outShape.rows = rows;
  outShape.cols = cols;
  outShape.minPrecision = entry.minPrecision;
  outShape.legacy = entry.legacy;
  return true;
}

} // namespace

bool isScalarTypeModelAvailable() {
  ensureScalarTypeModelLoaded();
  return gScalarTypeModelState == ScalarTypeModelState::Loaded;
}

const std::string &getScalarTypeModelError() {
  ensureScalarTypeModelLoaded();
  static const std::string ok;
  if (gScalarTypeModelState == ScalarTypeModelState::Loaded)
    return ok;
  return gScalarTypeModelError;
}

const std::vector<std::string> &getHlslScalarTypeBaseNames() {
  ensureScalarTypeModelLoaded();
  static const std::vector<std::string> empty;
  if (gScalarTypeModelState != ScalarTypeModelState::Loaded)
    return empty;
  return gBaseNames;
}

const std::vector<std::string> &getHlslScalarVectorMatrixTypeNames() {
  ensureScalarTypeModelLoaded();
  static const std::vector<std::string> empty;
  if (gScalarTypeModelState != ScalarTypeModelState::Loaded)
    return empty;
  return gGeneratedNames;
}

bool isHlslScalarTypeBaseName(const std::string &typeName) {
  ensureScalarTypeModelLoaded();
  if (gScalarTypeModelState != ScalarTypeModelState::Loaded)
    return false;
  return gBaseNameLookup.find(normalizeTypeName(typeName)) !=
         gBaseNameLookup.end();
}

bool isHlslScalarVectorMatrixTypeName(const std::string &typeName) {
  ensureScalarTypeModelLoaded();
  if (gScalarTypeModelState != ScalarTypeModelState::Loaded)
    return false;
  return gGeneratedNameLookup.find(normalizeTypeName(typeName)) !=
         gGeneratedNameLookup.end();
}

bool parseHlslScalarVectorMatrixTypeShape(
    const std::string &typeName, HlslScalarTypeShape &outShape) {
  outShape = HlslScalarTypeShape{};
  const std::string key = normalizeTypeName(typeName);
  if (key.empty())
    return false;
  if (const ScalarTypeEntry *entry = findEntry(key)) {
    return fillShapeForEntry(*entry, HlslScalarTypeShapeKind::Scalar, 1, 1,
                             outShape);
  }

  ensureScalarTypeModelLoaded();
  if (gScalarTypeModelState != ScalarTypeModelState::Loaded)
    return false;

  const size_t xPos = key.find('x');
  if (xPos != std::string::npos && xPos > 0 && xPos + 2 == key.size() &&
      std::isdigit(static_cast<unsigned char>(key[xPos - 1])) &&
      std::isdigit(static_cast<unsigned char>(key[xPos + 1]))) {
    const int rows = key[xPos - 1] - '0';
    const int cols = key[xPos + 1] - '0';
    const std::string base = key.substr(0, xPos - 1);
    auto it = gEntriesByLower.find(base);
    if (it != gEntriesByLower.end() && it->second.generateMatrix &&
        containsDimension(it->second.matrixRows, rows) &&
        containsDimension(it->second.matrixColumns, cols)) {
      return fillShapeForEntry(it->second, HlslScalarTypeShapeKind::Matrix,
                               rows, cols, outShape);
    }
    return false;
  }

  if (key.size() >= 2 &&
      std::isdigit(static_cast<unsigned char>(key[key.size() - 1]))) {
    const int dim = key[key.size() - 1] - '0';
    const std::string base = key.substr(0, key.size() - 1);
    auto it = gEntriesByLower.find(base);
    if (it != gEntriesByLower.end() && it->second.generateVector &&
        containsDimension(it->second.vectorDimensions, dim)) {
      return fillShapeForEntry(it->second, HlslScalarTypeShapeKind::Vector, dim,
                               1, outShape);
    }
  }
  return false;
}

bool hlslScalarTypeKindIsNumeric(HlslScalarTypeKind kind) {
  return kind == HlslScalarTypeKind::Integer ||
         kind == HlslScalarTypeKind::Floating;
}

bool hlslScalarTypeKindIsBoolean(HlslScalarTypeKind kind) {
  return kind == HlslScalarTypeKind::Bool;
}
