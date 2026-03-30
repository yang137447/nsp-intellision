#include "type_model.hpp"

#include "json.hpp"
#include "resource_registry.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct ObjectTypeEntry {
  std::string name;
  std::string family;
  int coordDim = -1;
  bool isRw = false;
  bool isArray = false;
};

struct FamilyEntry {
  std::string name;
  std::vector<std::string> members;
  std::vector<std::string> compatibleWith;
};

enum class TypeModelState { Uninitialized, Loaded, Unavailable };

std::once_flag gTypeModelOnce;
TypeModelState gTypeModelState = TypeModelState::Uninitialized;
std::string gTypeModelError;
std::vector<std::string> gObjectTypeNames;
std::unordered_map<std::string, ObjectTypeEntry> gTypesByLower;
std::unordered_map<std::string, FamilyEntry> gFamiliesByLower;

static std::string toLowerAscii(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

static std::string trimCopy(std::string value) {
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

static std::string normalizeTypeName(std::string typeName) {
  typeName = trimCopy(typeName);
  const size_t genericPos = typeName.find('<');
  if (genericPos != std::string::npos) {
    typeName = trimCopy(typeName.substr(0, genericPos));
  }
  return typeName;
}

static std::string toLookupKey(const std::string &rawTypeName) {
  const std::string key = toLowerAscii(normalizeTypeName(rawTypeName));
  if (key == "texture")
    return "texture2d";
  if (key == "sampler")
    return "samplerstate";
  return key;
}

static std::vector<std::string> parseStringArray(const Json *value) {
  std::vector<std::string> out;
  if (!value || value->type != Json::Type::Array)
    return out;
  out.reserve(value->a.size());
  for (const auto &item : value->a) {
    std::string text = getStringValue(item);
    if (!text.empty())
      out.push_back(text);
  }
  return out;
}

static void rebuildFamiliesFromMembers(
    std::unordered_map<std::string, FamilyEntry> &familiesByLower,
    const std::unordered_map<std::string, ObjectTypeEntry> &typesByLower) {
  for (auto &pair : familiesByLower)
    pair.second.members.clear();
  for (const auto &pair : typesByLower) {
    const ObjectTypeEntry &entry = pair.second;
    if (entry.family.empty())
      continue;
    const std::string familyKey = toLowerAscii(entry.family);
    auto it = familiesByLower.find(familyKey);
    if (it == familiesByLower.end()) {
      FamilyEntry family;
      family.name = entry.family;
      family.compatibleWith.push_back(entry.family);
      it = familiesByLower.emplace(familyKey, std::move(family)).first;
    }
    it->second.members.push_back(entry.name);
  }
  for (auto &pair : familiesByLower) {
    auto &members = pair.second.members;
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
  }
}

static bool loadTypeModel() {
  Json objectTypesRoot;
  Json objectTypeOverridesRoot;
  Json familiesRoot;
  Json familyOverridesRoot;
  Json typeOverridesBaseRoot;
  Json typeOverridesOverrideRoot;
  std::string error;
  if (!loadResourceBundleJson("types/object_types", objectTypesRoot,
                              objectTypeOverridesRoot, error)) {
    gTypeModelError = error;
    return false;
  }
  if (!loadResourceBundleJson("types/object_families", familiesRoot,
                              familyOverridesRoot, error)) {
    gTypeModelError = error;
    return false;
  }
  if (!loadResourceBundleJson("types/type_overrides", typeOverridesBaseRoot,
                              typeOverridesOverrideRoot, error)) {
    gTypeModelError = error;
    return false;
  }

  std::unordered_map<std::string, ObjectTypeEntry> typesByLower;
  std::unordered_map<std::string, FamilyEntry> familiesByLower;

  auto applyObjectTypeEntries = [&](const Json *entries,
                                    const std::string &errorLabel) {
    if (!entries || entries->type != Json::Type::Array) {
      gTypeModelError = errorLabel;
      return false;
    }
    for (const auto &entryValue : entries->a) {
      const Json *nameValue = getObjectValue(entryValue, "name");
      std::string name = nameValue ? getStringValue(*nameValue) : "";
      if (name.empty())
        continue;
      const std::string key = toLowerAscii(normalizeTypeName(name));
      const Json *disabledValue = getObjectValue(entryValue, "disabled");
      if (disabledValue && getBoolValue(*disabledValue, false)) {
        typesByLower.erase(key);
        continue;
      }
      auto it = typesByLower.find(key);
      if (it == typesByLower.end()) {
        ObjectTypeEntry next;
        next.name = normalizeTypeName(name);
        it = typesByLower.emplace(key, std::move(next)).first;
      }
      ObjectTypeEntry &entry = it->second;
      entry.name = normalizeTypeName(name);
      const Json *familyValue = getObjectValue(entryValue, "family");
      if (familyValue)
        entry.family = getStringValue(*familyValue);
      const Json *coordValue = getObjectValue(entryValue, "coordDim");
      if (coordValue)
        entry.coordDim = static_cast<int>(getNumberValue(*coordValue, -1));
      const Json *rwValue = getObjectValue(entryValue, "isRw");
      if (rwValue)
        entry.isRw = getBoolValue(*rwValue, entry.isRw);
      const Json *arrayValue = getObjectValue(entryValue, "isArray");
      if (arrayValue)
        entry.isArray = getBoolValue(*arrayValue, entry.isArray);
    }
    return true;
  };

  auto applyFamilyEntries = [&](const Json *entries,
                                const std::string &errorLabel) {
    if (!entries || entries->type != Json::Type::Array) {
      gTypeModelError = errorLabel;
      return false;
    }
    for (const auto &entryValue : entries->a) {
      const Json *nameValue = getObjectValue(entryValue, "name");
      std::string name = nameValue ? getStringValue(*nameValue) : "";
      if (name.empty())
        continue;
      const std::string key = toLowerAscii(name);
      const Json *disabledValue = getObjectValue(entryValue, "disabled");
      if (disabledValue && getBoolValue(*disabledValue, false)) {
        familiesByLower.erase(key);
        continue;
      }
      auto it = familiesByLower.find(key);
      if (it == familiesByLower.end()) {
        FamilyEntry next;
        next.name = name;
        it = familiesByLower.emplace(key, std::move(next)).first;
      }
      FamilyEntry &entry = it->second;
      entry.name = name;
      const Json *membersValue = getObjectValue(entryValue, "members");
      if (membersValue)
        entry.members = parseStringArray(membersValue);
      const Json *compatibleValue = getObjectValue(entryValue, "compatibleWith");
      if (compatibleValue)
        entry.compatibleWith = parseStringArray(compatibleValue);
      if (entry.compatibleWith.empty())
        entry.compatibleWith.push_back(entry.name);
    }
    return true;
  };

  if (!applyObjectTypeEntries(getObjectValue(objectTypesRoot, "entries"),
                              "object types entries missing") ||
      !applyFamilyEntries(getObjectValue(familiesRoot, "families"),
                          "object families entries missing") ||
      !applyObjectTypeEntries(getObjectValue(objectTypeOverridesRoot, "entries"),
                              "object type override entries missing") ||
      !applyFamilyEntries(getObjectValue(familyOverridesRoot, "families"),
                          "object family override entries missing") ||
      !applyObjectTypeEntries(getObjectValue(typeOverridesBaseRoot, "types"),
                              "type overrides base entries missing") ||
      !applyFamilyEntries(getObjectValue(typeOverridesBaseRoot, "families"),
                          "family overrides base entries missing") ||
      !applyObjectTypeEntries(getObjectValue(typeOverridesOverrideRoot, "types"),
                              "type overrides entries missing") ||
      !applyFamilyEntries(getObjectValue(typeOverridesOverrideRoot, "families"),
                          "family overrides entries missing")) {
    return false;
  }

  rebuildFamiliesFromMembers(familiesByLower, typesByLower);

  if (typesByLower.empty() || familiesByLower.empty()) {
    gTypeModelError = "merged type model empty";
    return false;
  }

  std::vector<std::string> objectTypeNames;
  objectTypeNames.reserve(typesByLower.size());
  for (const auto &entry : typesByLower) {
    if (!entry.second.name.empty())
      objectTypeNames.push_back(entry.second.name);
  }
  std::sort(objectTypeNames.begin(), objectTypeNames.end());
  objectTypeNames.erase(
      std::unique(objectTypeNames.begin(), objectTypeNames.end()),
      objectTypeNames.end());

  gObjectTypeNames = std::move(objectTypeNames);
  gTypesByLower = std::move(typesByLower);
  gFamiliesByLower = std::move(familiesByLower);
  gTypeModelError.clear();
  return true;
}

static void ensureTypeModelLoaded() {
  std::call_once(gTypeModelOnce, []() {
    if (loadTypeModel()) {
      gTypeModelState = TypeModelState::Loaded;
      return;
    }
    gTypeModelState = TypeModelState::Unavailable;
  });
}

static const ObjectTypeEntry *findObjectTypeEntry(const std::string &typeName) {
  ensureTypeModelLoaded();
  if (gTypeModelState != TypeModelState::Loaded)
    return nullptr;
  const std::string key = toLookupKey(typeName);
  auto it = gTypesByLower.find(key);
  if (it == gTypesByLower.end())
    return nullptr;
  return &it->second;
}

} // namespace

bool isTypeModelAvailable() {
  ensureTypeModelLoaded();
  return gTypeModelState == TypeModelState::Loaded;
}

const std::string &getTypeModelError() {
  ensureTypeModelLoaded();
  static const std::string ok;
  if (gTypeModelState == TypeModelState::Loaded)
    return ok;
  return gTypeModelError;
}

const std::vector<std::string> &getTypeModelObjectTypeNames() {
  ensureTypeModelLoaded();
  static const std::vector<std::string> empty;
  if (gTypeModelState != TypeModelState::Loaded)
    return empty;
  return gObjectTypeNames;
}

bool getTypeModelObjectFamily(const std::string &typeName,
                              std::string &outFamily) {
  outFamily.clear();
  ensureTypeModelLoaded();
  if (gTypeModelState != TypeModelState::Loaded)
    return false;
  const std::string key = toLookupKey(typeName);
  auto it = gTypesByLower.find(key);
  if (it == gTypesByLower.end() || it->second.family.empty())
    return false;
  outFamily = it->second.family;
  return true;
}

bool isTypeModelTextureLike(const std::string &typeName) {
  std::string family;
  if (!getTypeModelObjectFamily(typeName, family))
    return false;
  return family == "Texture" || family == "RWTexture";
}

bool isTypeModelSamplerLike(const std::string &typeName) {
  std::string family;
  if (!getTypeModelObjectFamily(typeName, family))
    return false;
  return family == "Sampler" || family == "SamplerComparisonState";
}

int getTypeModelCoordDim(const std::string &typeName) {
  const ObjectTypeEntry *entry = findObjectTypeEntry(typeName);
  if (!entry)
    return -1;
  return entry->coordDim;
}

int getTypeModelSampleCoordDim(const std::string &typeName) {
  const ObjectTypeEntry *entry = findObjectTypeEntry(typeName);
  if (!entry || entry->coordDim < 0)
    return -1;
  return entry->coordDim + (entry->isArray ? 1 : 0);
}

int getTypeModelLoadCoordDim(const std::string &typeName) {
  const int sampleCoordDim = getTypeModelSampleCoordDim(typeName);
  if (sampleCoordDim < 0)
    return -1;
  return sampleCoordDim + 1;
}

bool typeModelSameObjectFamily(const std::string &leftType,
                               const std::string &rightType) {
  std::string leftFamily;
  std::string rightFamily;
  if (!getTypeModelObjectFamily(leftType, leftFamily) ||
      !getTypeModelObjectFamily(rightType, rightFamily)) {
    return false;
  }
  if (leftFamily == rightFamily)
    return true;
  ensureTypeModelLoaded();
  if (gTypeModelState != TypeModelState::Loaded)
    return false;
  const std::string leftKey = toLowerAscii(leftFamily);
  auto it = gFamiliesByLower.find(leftKey);
  if (it == gFamiliesByLower.end())
    return false;
  const std::string rightLower = toLowerAscii(rightFamily);
  for (const auto &candidate : it->second.compatibleWith) {
    if (toLowerAscii(candidate) == rightLower)
      return true;
  }
  return false;
}
