#include "language_registry.hpp"

#include "resource_registry.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

enum class LanguageRegistryState { Uninitialized, Loaded, Unavailable };

std::once_flag gLanguageRegistryOnce;
LanguageRegistryState gLanguageRegistryState =
    LanguageRegistryState::Uninitialized;
std::string gLanguageRegistryError;

std::unordered_set<std::string> gKeywordLookup;
std::unordered_map<std::string, std::string> gKeywordDocs;
std::vector<std::string> gKeywordNames;

std::unordered_set<std::string> gDirectiveLookup;
std::unordered_map<std::string, HlslDirectiveInfo> gDirectiveInfoByName;
std::vector<std::string> gDirectiveNames;
std::vector<std::string> gDirectiveCompletionItems;

std::unordered_set<std::string> gSystemSemanticLookup;
std::unordered_map<std::string, HlslSystemSemanticInfo> gSemanticInfoByName;
std::vector<std::string> gSystemSemanticNames;

static std::string normalizeDirectiveName(const std::string &directiveToken) {
  if (!directiveToken.empty() && directiveToken[0] == '#')
    return directiveToken.substr(1);
  return directiveToken;
}

static bool applyKeywordEntries(
    const Json &root, std::unordered_set<std::string> &keywordNames,
    std::unordered_map<std::string, std::string> &keywordDocs,
    std::string &errorOut) {
  const Json *entries = getObjectValue(root, "entries");
  if (!entries || entries->type != Json::Type::Array) {
    errorOut = "keyword entries missing or invalid";
    return false;
  }
  for (const auto &entryValue : entries->a) {
    const Json *nameValue = getObjectValue(entryValue, "name");
    const std::string name = nameValue ? getStringValue(*nameValue) : "";
    if (name.empty())
      continue;
    const Json *disabledValue = getObjectValue(entryValue, "disabled");
    if (disabledValue && getBoolValue(*disabledValue, false)) {
      keywordNames.erase(name);
      keywordDocs.erase(name);
      continue;
    }
    keywordNames.insert(name);
    const Json *docValue = getObjectValue(entryValue, "documentation");
    if (docValue)
      keywordDocs[name] = getStringValue(*docValue);
    else if (keywordDocs.find(name) == keywordDocs.end())
      keywordDocs.emplace(name, "");
  }
  return true;
}

static bool applyDirectiveEntries(
    const Json &root, std::unordered_set<std::string> &directiveNames,
    std::unordered_map<std::string, HlslDirectiveInfo> &directiveInfo,
    std::string &errorOut) {
  const Json *entries = getObjectValue(root, "entries");
  if (!entries || entries->type != Json::Type::Array) {
    errorOut = "directive entries missing or invalid";
    return false;
  }
  for (const auto &entryValue : entries->a) {
    const Json *nameValue = getObjectValue(entryValue, "name");
    const std::string name = normalizeDirectiveName(
        nameValue ? getStringValue(*nameValue) : "");
    if (name.empty())
      continue;
    const Json *disabledValue = getObjectValue(entryValue, "disabled");
    if (disabledValue && getBoolValue(*disabledValue, false)) {
      directiveNames.erase(name);
      directiveInfo.erase(name);
      continue;
    }
    directiveNames.insert(name);
    HlslDirectiveInfo info;
    const Json *docValue = getObjectValue(entryValue, "documentation");
    if (docValue)
      info.documentation = getStringValue(*docValue);
    const Json *syntaxValue = getObjectValue(entryValue, "syntax");
    if (syntaxValue)
      info.syntax = getStringValue(*syntaxValue);
    directiveInfo[name] = std::move(info);
  }
  return true;
}

static bool applySemanticEntries(
    const Json &root, std::unordered_set<std::string> &semanticNames,
    std::unordered_map<std::string, HlslSystemSemanticInfo> &semanticInfo,
    std::string &errorOut) {
  const Json *entries = getObjectValue(root, "entries");
  if (!entries || entries->type != Json::Type::Array) {
    errorOut = "semantic entries missing or invalid";
    return false;
  }
  for (const auto &entryValue : entries->a) {
    const Json *nameValue = getObjectValue(entryValue, "name");
    const std::string name = nameValue ? getStringValue(*nameValue) : "";
    if (name.empty())
      continue;
    const Json *disabledValue = getObjectValue(entryValue, "disabled");
    if (disabledValue && getBoolValue(*disabledValue, false)) {
      semanticNames.erase(name);
      semanticInfo.erase(name);
      continue;
    }
    semanticNames.insert(name);
    HlslSystemSemanticInfo info;
    const Json *docValue = getObjectValue(entryValue, "documentation");
    if (docValue)
      info.documentation = getStringValue(*docValue);
    const Json *typeValue = getObjectValue(entryValue, "typicalType");
    if (typeValue)
      info.typicalType = getStringValue(*typeValue);
    semanticInfo[name] = std::move(info);
  }
  return true;
}

static void ensureLanguageRegistryLoaded() {
  std::call_once(gLanguageRegistryOnce, []() {
    Json keywordBaseRoot;
    Json keywordOverrideRoot;
    Json directiveBaseRoot;
    Json directiveOverrideRoot;
    Json semanticBaseRoot;
    Json semanticOverrideRoot;
    std::string error;

    if (!loadResourceBundleJson("language/keywords", keywordBaseRoot,
                                keywordOverrideRoot, error) ||
        !loadResourceBundleJson("language/directives", directiveBaseRoot,
                                directiveOverrideRoot, error) ||
        !loadResourceBundleJson("language/semantics", semanticBaseRoot,
                                semanticOverrideRoot, error)) {
      gLanguageRegistryError = error;
      gLanguageRegistryState = LanguageRegistryState::Unavailable;
      return;
    }

    std::unordered_set<std::string> keywordNames;
    std::unordered_map<std::string, std::string> keywordDocs;
    std::unordered_set<std::string> directiveNames;
    std::unordered_map<std::string, HlslDirectiveInfo> directiveInfo;
    std::unordered_set<std::string> semanticNames;
    std::unordered_map<std::string, HlslSystemSemanticInfo> semanticInfo;

    if (!applyKeywordEntries(keywordBaseRoot, keywordNames, keywordDocs,
                             error) ||
        !applyKeywordEntries(keywordOverrideRoot, keywordNames, keywordDocs,
                             error) ||
        !applyDirectiveEntries(directiveBaseRoot, directiveNames, directiveInfo,
                               error) ||
        !applyDirectiveEntries(directiveOverrideRoot, directiveNames,
                               directiveInfo, error) ||
        !applySemanticEntries(semanticBaseRoot, semanticNames, semanticInfo,
                              error) ||
        !applySemanticEntries(semanticOverrideRoot, semanticNames, semanticInfo,
                              error)) {
      gLanguageRegistryError = error;
      gLanguageRegistryState = LanguageRegistryState::Unavailable;
      return;
    }

    if (keywordNames.empty() || directiveNames.empty() || semanticNames.empty()) {
      gLanguageRegistryError = "merged language registry empty";
      gLanguageRegistryState = LanguageRegistryState::Unavailable;
      return;
    }

    gKeywordLookup = std::move(keywordNames);
    gKeywordNames.assign(gKeywordLookup.begin(), gKeywordLookup.end());
    std::sort(gKeywordNames.begin(), gKeywordNames.end());
    gKeywordDocs.clear();
    for (const auto &name : gKeywordNames) {
      auto it = keywordDocs.find(name);
      if (it != keywordDocs.end() && !it->second.empty())
        gKeywordDocs.emplace(name, it->second);
    }

    gDirectiveLookup = std::move(directiveNames);
    gDirectiveNames.assign(gDirectiveLookup.begin(), gDirectiveLookup.end());
    std::sort(gDirectiveNames.begin(), gDirectiveNames.end());
    gDirectiveInfoByName = std::move(directiveInfo);
    gDirectiveCompletionItems.clear();
    gDirectiveCompletionItems.reserve(gDirectiveNames.size());
    for (const auto &name : gDirectiveNames)
      gDirectiveCompletionItems.push_back("#" + name);

    gSystemSemanticLookup = std::move(semanticNames);
    gSystemSemanticNames.assign(gSystemSemanticLookup.begin(),
                                gSystemSemanticLookup.end());
    std::sort(gSystemSemanticNames.begin(), gSystemSemanticNames.end());
    gSemanticInfoByName = std::move(semanticInfo);

    gLanguageRegistryError.clear();
    gLanguageRegistryState = LanguageRegistryState::Loaded;
  });
}

} // namespace

const std::string *lookupHlslKeywordDoc(const std::string &keyword) {
  ensureLanguageRegistryLoaded();
  if (gLanguageRegistryState != LanguageRegistryState::Loaded)
    return nullptr;
  auto it = gKeywordDocs.find(keyword);
  if (it == gKeywordDocs.end())
    return nullptr;
  return &it->second;
}

bool isHlslKeyword(const std::string &keyword) {
  ensureLanguageRegistryLoaded();
  if (gLanguageRegistryState != LanguageRegistryState::Loaded)
    return false;
  return gKeywordLookup.find(keyword) != gKeywordLookup.end();
}

const std::vector<std::string> &getHlslKeywordNames() {
  ensureLanguageRegistryLoaded();
  static const std::vector<std::string> empty;
  if (gLanguageRegistryState != LanguageRegistryState::Loaded)
    return empty;
  return gKeywordNames;
}

bool lookupHlslDirectiveInfo(const std::string &directiveToken,
                             HlslDirectiveInfo &outInfo) {
  outInfo = HlslDirectiveInfo{};
  ensureLanguageRegistryLoaded();
  if (gLanguageRegistryState != LanguageRegistryState::Loaded)
    return false;
  const std::string key = normalizeDirectiveName(directiveToken);
  auto it = gDirectiveInfoByName.find(key);
  if (it == gDirectiveInfoByName.end())
    return false;
  outInfo = it->second;
  return true;
}

bool isHlslDirective(const std::string &directiveToken) {
  ensureLanguageRegistryLoaded();
  if (gLanguageRegistryState != LanguageRegistryState::Loaded)
    return false;
  return gDirectiveLookup.find(normalizeDirectiveName(directiveToken)) !=
         gDirectiveLookup.end();
}

const std::vector<std::string> &getHlslDirectiveNames() {
  ensureLanguageRegistryLoaded();
  static const std::vector<std::string> empty;
  if (gLanguageRegistryState != LanguageRegistryState::Loaded)
    return empty;
  return gDirectiveNames;
}

const std::vector<std::string> &getHlslDirectiveCompletionItems() {
  ensureLanguageRegistryLoaded();
  static const std::vector<std::string> empty;
  if (gLanguageRegistryState != LanguageRegistryState::Loaded)
    return empty;
  return gDirectiveCompletionItems;
}

bool lookupHlslSystemSemanticInfo(const std::string &semantic,
                                  HlslSystemSemanticInfo &outInfo) {
  outInfo = HlslSystemSemanticInfo{};
  ensureLanguageRegistryLoaded();
  if (gLanguageRegistryState != LanguageRegistryState::Loaded)
    return false;
  auto it = gSemanticInfoByName.find(semantic);
  if (it != gSemanticInfoByName.end()) {
    outInfo = it->second;
    return true;
  }
  if (semantic.rfind("SV_", 0) != 0)
    return false;
  outInfo.documentation = "HLSL system-value semantic.";
  return true;
}

bool isHlslSystemSemantic(const std::string &semantic) {
  ensureLanguageRegistryLoaded();
  if (gLanguageRegistryState != LanguageRegistryState::Loaded)
    return false;
  return gSystemSemanticLookup.find(semantic) != gSystemSemanticLookup.end() ||
         semantic.rfind("SV_", 0) == 0;
}

const std::vector<std::string> &getHlslSystemSemanticNames() {
  ensureLanguageRegistryLoaded();
  static const std::vector<std::string> empty;
  if (gLanguageRegistryState != LanguageRegistryState::Loaded)
    return empty;
  return gSystemSemanticNames;
}

bool isHlslKeywordRegistryAvailable() { return isHlslLanguageRegistryAvailable(); }

const std::string &getHlslKeywordRegistryError() {
  return getHlslLanguageRegistryError();
}

bool isHlslLanguageRegistryAvailable() {
  ensureLanguageRegistryLoaded();
  return gLanguageRegistryState == LanguageRegistryState::Loaded;
}

const std::string &getHlslLanguageRegistryError() {
  ensureLanguageRegistryLoaded();
  static const std::string ok;
  if (gLanguageRegistryState == LanguageRegistryState::Loaded)
    return ok;
  return gLanguageRegistryError;
}
