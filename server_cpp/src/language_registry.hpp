#pragma once

#include <string>
#include <vector>

struct HlslDirectiveInfo {
  std::string documentation;
  std::string syntax;
};

struct HlslSystemSemanticInfo {
  std::string documentation;
  std::string typicalType;
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

bool isHlslKeywordRegistryAvailable();
const std::string &getHlslKeywordRegistryError();
bool isHlslLanguageRegistryAvailable();
const std::string &getHlslLanguageRegistryError();
