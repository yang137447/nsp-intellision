#include "nsf_lexer.hpp"

#include <cctype>

bool isIdentifierChar(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

std::vector<LexToken> lexLineTokens(const std::string &line) {
  std::vector<LexToken> tokens;
  size_t i = 0;
  while (i < line.size()) {
    unsigned char ch = static_cast<unsigned char>(line[i]);
    if (std::isspace(ch)) {
      i++;
      continue;
    }
    if (isIdentifierChar(static_cast<char>(ch))) {
      size_t start = i;
      i++;
      while (i < line.size() && isIdentifierChar(line[i]))
        i++;
      tokens.push_back(LexToken{LexToken::Kind::Identifier,
                                line.substr(start, i - start), start, i});
      continue;
    }
    if (i + 1 < line.size()) {
      std::string two = line.substr(i, 2);
      if (two == "::" || two == "->" || two == "&&" || two == "||" ||
          two == "<=" || two == ">=" || two == "==" || two == "!=" ||
          two == "<<" || two == ">>") {
        tokens.push_back(LexToken{LexToken::Kind::Punct, two, i, i + 2});
        i += 2;
        continue;
      }
    }
    tokens.push_back(
        LexToken{LexToken::Kind::Punct, std::string(1, line[i]), i, i + 1});
    i++;
  }
  return tokens;
}

bool isQualifierToken(const std::string &token) {
  static const std::unordered_set<std::string> qualifiers = {
      "const",        "static",        "uniform",         "volatile",
      "in",           "out",           "inout",           "row_major",
      "column_major", "precise",       "nointerpolation", "linear",
      "centroid",     "noperspective", "sample",          "struct"};
  return qualifiers.find(token) != qualifiers.end();
}

std::string trimLeftCopy(const std::string &value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start]))) {
    start++;
  }
  return value.substr(start);
}

std::string trimRightCopy(const std::string &value) {
  size_t end = value.size();
  while (end > 0 && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }
  return value.substr(0, end);
}

bool isBlankLine(const std::string &value) {
  for (char ch : value) {
    if (!std::isspace(static_cast<unsigned char>(ch)))
      return false;
  }
  return true;
}
