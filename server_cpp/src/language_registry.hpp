#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct HlslDirectiveInfo {
  std::string documentation;
  std::string syntax;
};

struct HlslSystemSemanticInfo {
  std::string documentation;
  std::string typicalType;
};

struct HlslPreprocessorMacroInfo {
  // Object-like macro replacement used to prefill the user-visible
  // `nsf.preprocessorMacros` setting.
  std::string replacement;
};

const std::string *lookupHlslKeywordDoc(const std::string &keyword);
bool isHlslKeyword(const std::string &keyword);
const std::vector<std::string> &getHlslKeywordNames();

bool lookupHlslDirectiveInfo(const std::string &directiveToken,
                             HlslDirectiveInfo &outInfo);
bool isHlslDirective(const std::string &directiveToken);
const std::vector<std::string> &getHlslDirectiveNames();
const std::vector<std::string> &getHlslDirectiveCompletionItems();

bool lookupHlslSystemSemanticInfo(const std::string &semantic,
                                  HlslSystemSemanticInfo &outInfo);
bool isHlslSystemSemantic(const std::string &semantic);
const std::vector<std::string> &getHlslSystemSemanticNames();

// Returns shadercompiler-style builtin preprocessor macros. The server exposes
// this registry to the client so first-run workspace settings can be populated
// from the same resource source as other language facts.
const std::unordered_map<std::string, HlslPreprocessorMacroInfo> &
getHlslPreprocessorMacros();

bool isHlslKeywordRegistryAvailable();
const std::string &getHlslKeywordRegistryError();
bool isHlslLanguageRegistryAvailable();
const std::string &getHlslLanguageRegistryError();
