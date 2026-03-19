#pragma once

#include <string>
#include <vector>

#include "hlsl_builtin_docs.hpp"

std::string formatCppCodeBlock(const std::string &code);

bool formatHlslSystemSemanticMarkdown(const std::string &semantic,
                                      std::string &outMarkdown);

bool formatHlslDirectiveMarkdown(const std::string &directive,
                                 std::string &outMarkdown);

bool formatHlslKeywordMarkdown(const std::string &keyword,
                               std::string &outMarkdown);

bool formatHlslBuiltinMethodMarkdown(const std::string &method,
                                     const std::string &baseType,
                                     std::string &outMarkdown);

bool lookupHlslBuiltinMethodSignatures(const std::string &method,
                                       const std::string &baseType,
                                       std::vector<HlslBuiltinSignature> &out);

struct HlslBuiltinMethodRule {
  int minArgs = 0;
  int maxArgs = 0;
  std::string returnType;
};

bool lookupHlslBuiltinMethodRule(const std::string &method,
                                 const std::string &baseType,
                                 HlslBuiltinMethodRule &outRule);

bool listHlslBuiltinMethodsForType(const std::string &baseType,
                                   std::vector<std::string> &outMethods);
