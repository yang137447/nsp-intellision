#pragma once

#include <string>
#include <vector>

const std::string *lookupHlslBuiltinDoc(const std::string &word);

struct HlslBuiltinSignature {
  std::string label;
  std::string kind;
  std::vector<std::string> parameters;
  std::string documentation;
};

struct HlslBuiltinRuleView {
  bool exists = false;
  bool typeChecked = false;
  bool hasSignatures = false;
  bool hasDocumentation = false;
};

const std::vector<HlslBuiltinSignature> *
lookupHlslBuiltinSignatures(const std::string &name);
const HlslBuiltinSignature *lookupHlslBuiltinSignature(const std::string &name);
bool queryHlslBuiltinRuleView(const std::string &name,
                              HlslBuiltinRuleView &outRule);
bool isHlslBuiltinFunction(const std::string &name);
bool isHlslBuiltinTypeCheckedFunction(const std::string &name);
const std::vector<std::string> &getHlslBuiltinNames();
bool isHlslBuiltinRegistryAvailable();
const std::string &getHlslBuiltinRegistryError();
