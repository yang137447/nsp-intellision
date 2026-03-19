#pragma once

#include <string>
#include <unordered_set>
#include <vector>

struct LexToken {
  enum class Kind { Identifier, Punct };
  Kind kind = Kind::Punct;
  std::string text;
  size_t start = 0;
  size_t end = 0;
};

bool isIdentifierChar(char ch);
std::vector<LexToken> lexLineTokens(const std::string &line);
bool isQualifierToken(const std::string &token);

std::string trimLeftCopy(const std::string &value);
std::string trimRightCopy(const std::string &value);
bool isBlankLine(const std::string &value);
