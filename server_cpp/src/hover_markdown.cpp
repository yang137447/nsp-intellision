#include "hover_markdown.hpp"

#include "language_registry.hpp"
#include "resource_registry.hpp"
#include "type_model.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {

struct MethodSignatureTemplate {
  std::string labelTemplate;
  std::vector<std::string> parameterTemplates;
  std::string hoverSignatureTemplate;
};

struct MethodEntry {
  std::string name;
  std::vector<std::string> targetFamilies;
  int minArgs = 0;
  int maxArgs = 0;
  std::string returnType;
  std::string documentation;
  std::vector<MethodSignatureTemplate> signatures;
};

enum class MethodRegistryState { Uninitialized, Loaded, Unavailable };

std::once_flag gMethodRegistryOnce;
MethodRegistryState gMethodRegistryState = MethodRegistryState::Uninitialized;
std::string gMethodRegistryError;
std::unordered_map<std::string, MethodEntry> gMethodEntries;

static std::string toLowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
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

static std::string normalizeBaseType(std::string baseType) {
  baseType = trimCopy(baseType);
  const size_t generic = baseType.find('<');
  if (generic != std::string::npos) {
    baseType = baseType.substr(0, generic);
  }
  return trimCopy(baseType);
}

static std::string coordType(int dim, bool integer) {
  const std::string base = integer ? "int" : "float";
  if (dim <= 1)
    return base;
  return base + std::to_string(dim);
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

static bool parseMethodEntry(const Json &value, MethodEntry &out) {
  const Json *nameValue = getObjectValue(value, "name");
  if (!nameValue)
    return false;
  out.name = getStringValue(*nameValue);
  if (out.name.empty())
    return false;

  const Json *familiesValue = getObjectValue(value, "targetFamilies");
  out.targetFamilies = parseStringArray(familiesValue);
  if (out.targetFamilies.empty())
    return false;

  const Json *minArgsValue = getObjectValue(value, "minArgs");
  const Json *maxArgsValue = getObjectValue(value, "maxArgs");
  out.minArgs = minArgsValue ? static_cast<int>(getNumberValue(*minArgsValue, 0))
                             : 0;
  out.maxArgs = maxArgsValue ? static_cast<int>(getNumberValue(*maxArgsValue, 0))
                             : 0;

  const Json *retValue = getObjectValue(value, "returnType");
  out.returnType = retValue ? getStringValue(*retValue) : "";
  const Json *docValue = getObjectValue(value, "documentation");
  out.documentation = docValue ? getStringValue(*docValue) : "";

  const Json *sigsValue = getObjectValue(value, "signatures");
  if (sigsValue && sigsValue->type == Json::Type::Array) {
    out.signatures.reserve(sigsValue->a.size());
    for (const auto &sigValue : sigsValue->a) {
      MethodSignatureTemplate sig;
      const Json *labelValue = getObjectValue(sigValue, "labelTemplate");
      const Json *paramsValue = getObjectValue(sigValue, "parameterTemplates");
      const Json *hoverSigValue =
          getObjectValue(sigValue, "hoverSignatureTemplate");
      if (labelValue)
        sig.labelTemplate = getStringValue(*labelValue);
      if (paramsValue)
        sig.parameterTemplates = parseStringArray(paramsValue);
      if (hoverSigValue)
        sig.hoverSignatureTemplate = getStringValue(*hoverSigValue);
      if (!sig.labelTemplate.empty())
        out.signatures.push_back(std::move(sig));
    }
  }
  return !out.signatures.empty();
}

static std::string applyTemplate(std::string input,
                                 const std::string &baseType,
                                 int textureCoordDim) {
  const std::unordered_map<std::string, std::string> replacements = {
      {"{baseType}", normalizeBaseType(baseType)},
      {"{floatCoord}", coordType(textureCoordDim, false)},
      {"{intCoord}", coordType(textureCoordDim, true)},
      {"{intCoordPlus1}", coordType(textureCoordDim + 1, true)},
  };
  for (const auto &entry : replacements) {
    size_t pos = 0;
    while ((pos = input.find(entry.first, pos)) != std::string::npos) {
      input.replace(pos, entry.first.size(), entry.second);
      pos += entry.second.size();
    }
  }
  return input;
}

static bool methodSupportsFamily(const MethodEntry &entry,
                                 const std::string &family) {
  for (const auto &f : entry.targetFamilies) {
    if (f == family)
      return true;
  }
  return false;
}

static bool lookupMethodEntry(const std::string &method,
                              const std::string &baseType,
                              MethodEntry &outEntry) {
  if (gMethodRegistryState != MethodRegistryState::Loaded)
    return false;
  const std::string key = toLowerAscii(method);
  auto it = gMethodEntries.find(key);
  if (it == gMethodEntries.end())
    return false;
  std::string family;
  if (!getTypeModelObjectFamily(baseType, family))
    return false;
  if (!methodSupportsFamily(it->second, family))
    return false;
  outEntry = it->second;
  return true;
}

static void ensureMethodRegistryLoaded() {
  std::call_once(gMethodRegistryOnce, []() {
    Json baseRoot;
    Json overridesRoot;
    std::string error;
    if (!loadResourceBundleJson("methods/object_methods", baseRoot,
                                overridesRoot, error)) {
      gMethodRegistryError = error;
      gMethodRegistryState = MethodRegistryState::Unavailable;
      return;
    }

    std::unordered_map<std::string, MethodEntry> merged;
    const Json *baseEntries = getObjectValue(baseRoot, "entries");
    if (!baseEntries || baseEntries->type != Json::Type::Array) {
      gMethodRegistryError = "methods.entries missing";
      gMethodRegistryState = MethodRegistryState::Unavailable;
      return;
    }
    for (const auto &entryValue : baseEntries->a) {
      MethodEntry entry;
      if (!parseMethodEntry(entryValue, entry))
        continue;
      merged[toLowerAscii(entry.name)] = std::move(entry);
    }

    const Json *overrideEntries = getObjectValue(overridesRoot, "entries");
    if (!overrideEntries || overrideEntries->type != Json::Type::Array) {
      gMethodRegistryError = "methods_overrides.entries missing";
      gMethodRegistryState = MethodRegistryState::Unavailable;
      return;
    }
    for (const auto &entryValue : overrideEntries->a) {
      const Json *nameValue = getObjectValue(entryValue, "name");
      std::string name = nameValue ? getStringValue(*nameValue) : "";
      if (name.empty())
        continue;
      const std::string key = toLowerAscii(name);
      const Json *disabledValue = getObjectValue(entryValue, "disabled");
      if (disabledValue && getBoolValue(*disabledValue, false)) {
        merged.erase(key);
        continue;
      }
      MethodEntry next;
      if (!parseMethodEntry(entryValue, next))
        continue;
      merged[key] = std::move(next);
    }

    if (merged.empty()) {
      gMethodRegistryError = "merged object methods empty";
      gMethodRegistryState = MethodRegistryState::Unavailable;
      return;
    }
    gMethodEntries = std::move(merged);
    gMethodRegistryError.clear();
    gMethodRegistryState = MethodRegistryState::Loaded;
  });
}

} // namespace

std::string formatCppCodeBlock(const std::string &code) {
  std::string out;
  out.reserve(code.size() + 16);
  out += "```cpp\n";
  out += code;
  out += "\n```";
  return out;
}

static std::string formatGenericSemantic(const std::string &semantic) {
  std::string md;
  md += formatCppCodeBlock(semantic);
  md += "\n\n(HLSL system-value semantic)";
  return md;
}

bool formatHlslSystemSemanticMarkdown(const std::string &semantic,
                                      std::string &outMarkdown) {
  HlslSystemSemanticInfo info;
  if (!lookupHlslSystemSemanticInfo(semantic, info))
    return false;
  std::string md;
  md += formatCppCodeBlock(semantic);
  md += "\n\n(HLSL system-value semantic)\n\n";
  md += info.documentation.empty() ? "HLSL system-value semantic."
                                   : info.documentation;
  if (!info.typicalType.empty()) {
    md += "\n\nTypical type: ";
    md += info.typicalType;
  }
  outMarkdown = std::move(md);
  return true;
}

bool formatHlslDirectiveMarkdown(const std::string &directive,
                                 std::string &outMarkdown) {
  HlslDirectiveInfo info;
  if (!lookupHlslDirectiveInfo(directive, info))
    return false;
  std::string md;
  md += formatCppCodeBlock("#" + directive);
  md += "\n\n(HLSL preprocessor directive)";
  if (!info.documentation.empty()) {
    md += "\n\n";
    md += info.documentation;
  }
  if (!info.syntax.empty()) {
    md += "\n\nSyntax:\n";
    md += formatCppCodeBlock(info.syntax);
  }
  outMarkdown = std::move(md);
  return true;
}

bool formatHlslKeywordMarkdown(const std::string &keyword,
                               std::string &outMarkdown) {
  if (!isHlslKeyword(keyword))
    return false;
  std::string md;
  md += formatCppCodeBlock(keyword);
  md += "\n\n(HLSL keyword)";
  if (const std::string *doc = lookupHlslKeywordDoc(keyword)) {
    md += "\n\n";
    md += *doc;
  }
  outMarkdown = std::move(md);
  return true;
}

bool lookupHlslBuiltinMethodSignatures(const std::string &method,
                                       const std::string &baseType,
                                       std::vector<HlslBuiltinSignature> &out) {
  out.clear();
  ensureMethodRegistryLoaded();
  MethodEntry entry;
  if (!lookupMethodEntry(method, baseType, entry))
    return false;
  int dim = getTypeModelCoordDim(baseType);
  if (dim < 1)
    dim = 2;
  for (const auto &templateSig : entry.signatures) {
    HlslBuiltinSignature sig;
    sig.label = applyTemplate(templateSig.labelTemplate, baseType, dim);
    for (const auto &paramTemplate : templateSig.parameterTemplates) {
      sig.parameters.push_back(applyTemplate(paramTemplate, baseType, dim));
    }
    std::string md;
    std::string hoverSignature = templateSig.hoverSignatureTemplate;
    if (hoverSignature.empty()) {
      hoverSignature = entry.returnType + " " + normalizeBaseType(baseType) +
                       "<float4>::" + sig.label;
    } else {
      hoverSignature = applyTemplate(hoverSignature, baseType, dim);
    }
    md += formatCppCodeBlock(hoverSignature);
    md += "\n\n(HLSL built-in method)\n\n";
    md += entry.documentation;
    sig.documentation = std::move(md);
    out.push_back(std::move(sig));
  }
  return !out.empty();
}

bool formatHlslBuiltinMethodMarkdown(const std::string &method,
                                     const std::string &baseType,
                                     std::string &outMarkdown) {
  std::vector<HlslBuiltinSignature> signatures;
  if (!lookupHlslBuiltinMethodSignatures(method, baseType, signatures) ||
      signatures.empty()) {
    return false;
  }
  outMarkdown = signatures.front().documentation;
  return true;
}

bool lookupHlslBuiltinMethodRule(const std::string &method,
                                 const std::string &baseType,
                                 HlslBuiltinMethodRule &outRule) {
  ensureMethodRegistryLoaded();
  MethodEntry entry;
  if (!lookupMethodEntry(method, baseType, entry))
    return false;
  outRule.minArgs = entry.minArgs;
  outRule.maxArgs = entry.maxArgs;
  outRule.returnType = entry.returnType;
  return true;
}

bool listHlslBuiltinMethodsForType(const std::string &baseType,
                                   std::vector<std::string> &outMethods) {
  outMethods.clear();
  ensureMethodRegistryLoaded();
  if (gMethodRegistryState != MethodRegistryState::Loaded)
    return false;
  std::string family;
  if (!getTypeModelObjectFamily(baseType, family))
    return false;
  for (const auto &entry : gMethodEntries) {
    if (methodSupportsFamily(entry.second, family))
      outMethods.push_back(entry.second.name);
  }
  std::sort(outMethods.begin(), outMethods.end());
  outMethods.erase(std::unique(outMethods.begin(), outMethods.end()),
                   outMethods.end());
  return !outMethods.empty();
}
