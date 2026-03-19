#include "hlsl_builtin_docs.hpp"
#include "json.hpp"
#include "resource_registry.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

static std::string toLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

struct BuiltinEntry {
  std::string name;
  std::string summary;
  std::vector<std::string> docUrls;
  std::vector<HlslBuiltinSignature> signatures;
};

enum class BuiltinRegistryState { Uninitialized, Loaded, Unavailable };

std::once_flag gBuiltinRegistryOnce;
BuiltinRegistryState gBuiltinRegistryState = BuiltinRegistryState::Uninitialized;
std::string gBuiltinRegistryError;
std::unordered_map<std::string, std::string> gBuiltinDocs;
std::unordered_map<std::string, std::vector<HlslBuiltinSignature>>
    gBuiltinSignatures;
std::vector<std::string> gBuiltinNames;

const std::unordered_set<std::string> &builtinFallbackNames() {
  static const std::unordered_set<std::string> kNames = {
      "saturate",     "clamp",        "lerp",         "min",
      "max",          "step",         "smoothstep",   "dot",
      "length",       "distance",     "normalize",    "reflect",
      "refract",      "mul",          "pow",          "atan2",
      "asfloat",      "asint",        "asuint",       "asdouble",
      "f16tof32",     "f32tof16",     "countbits",    "firstbithigh",
      "firstbitlow",  "reversebits",  "rcp",          "mad",
      "fma",          "cross",        "abs",          "sin",
      "cos",          "tan",          "asin",         "acos",
      "atan",         "exp",          "exp2",         "floor",
      "ceil",         "frac",         "fmod",         "rsqrt",
      "sqrt",         "sign",
  };
  return kNames;
}

const std::unordered_set<std::string> &builtinTypeCheckedFallbackNames() {
  static const std::unordered_set<std::string> kNames = {
      "length",      "distance",    "dot",         "normalize",
      "saturate",    "abs",         "sin",         "cos",
      "tan",         "asin",        "acos",        "atan",
      "exp",         "exp2",        "floor",       "ceil",
      "frac",        "fmod",        "rsqrt",       "sqrt",
      "sign",        "pow",         "atan2",       "reflect",
      "refract",     "clamp",       "lerp",        "step",
      "smoothstep",  "min",         "max",         "mul",
      "cross",       "asfloat",     "asint",       "asuint",
      "countbits",   "firstbithigh","firstbitlow", "reversebits",
      "f16tof32",    "f32tof16",    "rcp",         "mad",
  };
  return kNames;
}

static std::vector<HlslBuiltinSignature>
parseSignatureArray(const Json *signaturesValue, const std::string &name) {
  std::vector<HlslBuiltinSignature> overloads;
  if (signaturesValue && signaturesValue->type == Json::Type::Array) {
    overloads.reserve(signaturesValue->a.size());
    for (const auto &sigValue : signaturesValue->a) {
      const Json *displayValue = getObjectValue(sigValue, "display");
      std::string display = displayValue ? getStringValue(*displayValue) : "";
      if (display.empty())
        continue;
      std::vector<std::string> params;
      const Json *paramsValue = getObjectValue(sigValue, "params");
      if (paramsValue && paramsValue->type == Json::Type::Array) {
        params.reserve(paramsValue->a.size());
        for (const auto &p : paramsValue->a) {
          std::string item = getStringValue(p);
          if (!item.empty())
            params.push_back(item);
        }
      }
      const Json *kindValue = getObjectValue(sigValue, "kind");
      std::string kind = kindValue ? getStringValue(*kindValue) : "";
      HlslBuiltinSignature sig;
      sig.label = display;
      sig.kind = kind;
      sig.parameters = std::move(params);
      overloads.push_back(std::move(sig));
    }
  }
  if (overloads.empty()) {
    HlslBuiltinSignature sig;
    sig.label = name + "(...)";
    sig.kind = "ret";
    overloads.push_back(std::move(sig));
  }
  return overloads;
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

static bool parseManifestEntries(
    const Json &manifestRoot, std::unordered_map<std::string, BuiltinEntry> &out,
    std::string &errorOut) {
  const Json *entries = getObjectValue(manifestRoot, "entries");
  if (!entries || entries->type != Json::Type::Array) {
    errorOut = "manifest.entries missing or invalid";
    return false;
  }
  for (const auto &entryValue : entries->a) {
    const Json *nameValue = getObjectValue(entryValue, "name");
    std::string name = nameValue ? getStringValue(*nameValue) : "";
    if (name.empty())
      continue;
    BuiltinEntry entry;
    entry.name = name;
    const Json *summaryValue = getObjectValue(entryValue, "summary");
    if (summaryValue)
      entry.summary = getStringValue(*summaryValue);
    entry.docUrls = parseStringArray(getObjectValue(entryValue, "docUrls"));
    entry.signatures =
        parseSignatureArray(getObjectValue(entryValue, "signatures"), name);
    out[toLowerAscii(name)] = std::move(entry);
  }
  if (out.empty()) {
    errorOut = "manifest entries empty";
    return false;
  }
  return true;
}

static bool applyOverrides(const Json &overridesRoot,
                           std::unordered_map<std::string, BuiltinEntry> &entries,
                           std::string &errorOut) {
  const Json *overrides = getObjectValue(overridesRoot, "entries");
  if (!overrides || overrides->type != Json::Type::Array) {
    errorOut = "overrides.entries missing or invalid";
    return false;
  }
  for (const auto &overrideValue : overrides->a) {
    const Json *nameValue = getObjectValue(overrideValue, "name");
    std::string name = nameValue ? getStringValue(*nameValue) : "";
    if (name.empty())
      continue;
    const std::string key = toLowerAscii(name);
    const Json *disabledValue = getObjectValue(overrideValue, "disabled");
    if (disabledValue && getBoolValue(*disabledValue, false)) {
      entries.erase(key);
      continue;
    }
    auto it = entries.find(key);
    if (it == entries.end()) {
      BuiltinEntry entry;
      entry.name = name;
      it = entries.emplace(key, std::move(entry)).first;
    }
    BuiltinEntry &entry = it->second;
    entry.name = name;
    const Json *summaryValue = getObjectValue(overrideValue, "summary");
    if (summaryValue)
      entry.summary = getStringValue(*summaryValue);
    const Json *docUrlsValue = getObjectValue(overrideValue, "docUrls");
    if (docUrlsValue)
      entry.docUrls = parseStringArray(docUrlsValue);
    const Json *signaturesValue = getObjectValue(overrideValue, "signatures");
    if (signaturesValue)
      entry.signatures = parseSignatureArray(signaturesValue, entry.name);
  }
  return true;
}

static std::string buildBuiltinDoc(const BuiltinEntry &entry,
                                   const std::vector<HlslBuiltinSignature> &sigs) {
  std::string summary = entry.summary;
  const std::string summaryLower = toLowerAscii(summary);
  if (summaryLower == "skip to main content" ||
      summaryLower.rfind("access to this page requires authorization", 0) == 0) {
    summary.clear();
  }

  std::string doc;
  if (!summary.empty()) {
    doc += summary;
    doc += "\n\n";
  }
  for (const auto &sig : sigs) {
    std::string ret = sig.kind;
    if (ret.empty() || ret == "bare")
      ret = "ret";
    doc += "`" + ret + " " + sig.label + "`\n";
  }
  if (!entry.docUrls.empty()) {
    doc.push_back('\n');
    doc += entry.docUrls.front();
  }
  return doc;
}

static void ensureBuiltinRegistryLoaded() {
  std::call_once(gBuiltinRegistryOnce, []() {
    Json manifestRoot;
    Json overridesRoot;
    std::string error;
    if (!loadResourceBundleJson("builtins/intrinsics", manifestRoot,
                                overridesRoot, error)) {
      gBuiltinRegistryError = error;
      gBuiltinRegistryState = BuiltinRegistryState::Unavailable;
      return;
    }

    std::unordered_map<std::string, BuiltinEntry> mergedEntries;
    if (!parseManifestEntries(manifestRoot, mergedEntries, error)) {
      gBuiltinRegistryError = error;
      gBuiltinRegistryState = BuiltinRegistryState::Unavailable;
      return;
    }
    if (!applyOverrides(overridesRoot, mergedEntries, error)) {
      gBuiltinRegistryError = error;
      gBuiltinRegistryState = BuiltinRegistryState::Unavailable;
      return;
    }

    std::unordered_map<std::string, std::string> docs;
    std::unordered_map<std::string, std::vector<HlslBuiltinSignature>>
        signatures;
    std::vector<std::string> names;
    names.reserve(mergedEntries.size());
    for (auto &it : mergedEntries) {
      BuiltinEntry &entry = it.second;
      if (entry.name.empty())
        continue;
      std::vector<HlslBuiltinSignature> sigs = entry.signatures;
      if (sigs.empty())
        sigs = parseSignatureArray(nullptr, entry.name);
      const std::string doc = buildBuiltinDoc(entry, sigs);
      for (auto &sig : sigs)
        sig.documentation = doc;
      docs.emplace(it.first, doc);
      signatures.emplace(it.first, std::move(sigs));
      names.push_back(entry.name);
    }
    if (docs.empty() || signatures.empty() || names.empty()) {
      gBuiltinRegistryError = "merged builtin registry empty";
      gBuiltinRegistryState = BuiltinRegistryState::Unavailable;
      return;
    }
    std::sort(names.begin(), names.end());
    gBuiltinDocs = std::move(docs);
    gBuiltinSignatures = std::move(signatures);
    gBuiltinNames = std::move(names);
    gBuiltinRegistryError.clear();
    gBuiltinRegistryState = BuiltinRegistryState::Loaded;
  });
}

} // namespace

const std::string *lookupHlslBuiltinDoc(const std::string &word) {
  ensureBuiltinRegistryLoaded();
  if (gBuiltinRegistryState != BuiltinRegistryState::Loaded)
    return nullptr;
  auto it = gBuiltinDocs.find(toLowerAscii(word));
  if (it == gBuiltinDocs.end())
    return nullptr;
  return &it->second;
}

const std::vector<HlslBuiltinSignature> *
lookupHlslBuiltinSignatures(const std::string &name) {
  ensureBuiltinRegistryLoaded();
  if (gBuiltinRegistryState != BuiltinRegistryState::Loaded)
    return nullptr;
  auto it = gBuiltinSignatures.find(toLowerAscii(name));
  if (it == gBuiltinSignatures.end())
    return nullptr;
  return &it->second;
}

const HlslBuiltinSignature *
lookupHlslBuiltinSignature(const std::string &name) {
  const std::vector<HlslBuiltinSignature> *all = lookupHlslBuiltinSignatures(name);
  if (!all || all->empty())
    return nullptr;
  return &all->front();
}

bool queryHlslBuiltinRuleView(const std::string &name,
                              HlslBuiltinRuleView &outRule) {
  outRule = HlslBuiltinRuleView{};
  const std::string lower = toLowerAscii(name);
  ensureBuiltinRegistryLoaded();

  if (gBuiltinRegistryState == BuiltinRegistryState::Loaded) {
    auto itSig = gBuiltinSignatures.find(lower);
    auto itDoc = gBuiltinDocs.find(lower);
    outRule.hasSignatures = itSig != gBuiltinSignatures.end();
    outRule.hasDocumentation = itDoc != gBuiltinDocs.end();
    outRule.exists = outRule.hasSignatures || outRule.hasDocumentation;
  }

  if (!outRule.exists)
    outRule.exists = builtinFallbackNames().count(lower) > 0;
  outRule.typeChecked = builtinTypeCheckedFallbackNames().count(lower) > 0;
  return outRule.exists;
}

bool isHlslBuiltinFunction(const std::string &name) {
  HlslBuiltinRuleView rule;
  return queryHlslBuiltinRuleView(name, rule);
}

bool isHlslBuiltinTypeCheckedFunction(const std::string &name) {
  HlslBuiltinRuleView rule;
  queryHlslBuiltinRuleView(name, rule);
  return rule.typeChecked;
}

const std::vector<std::string> &getHlslBuiltinNames() {
  ensureBuiltinRegistryLoaded();
  static const std::vector<std::string> empty;
  if (gBuiltinRegistryState != BuiltinRegistryState::Loaded)
    return empty;
  return gBuiltinNames;
}

bool isHlslBuiltinRegistryAvailable() {
  ensureBuiltinRegistryLoaded();
  return gBuiltinRegistryState == BuiltinRegistryState::Loaded;
}

const std::string &getHlslBuiltinRegistryError() {
  ensureBuiltinRegistryLoaded();
  static const std::string ok;
  if (gBuiltinRegistryState == BuiltinRegistryState::Loaded)
    return ok;
  return gBuiltinRegistryError;
}
