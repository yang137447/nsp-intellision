#include "diagnostics_semantic_common.hpp"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

std::string formatTypeList(const std::vector<std::string> &types) {
  std::ostringstream oss;
  oss << "(";
  for (size_t i = 0; i < types.size(); i++) {
    if (i > 0)
      oss << ", ";
    oss << types[i];
  }
  oss << ")";
  return oss.str();
}

std::vector<char> buildCodeMaskForLine(const std::string &lineText,
                                       bool &inBlockCommentInOut) {
  std::vector<char> mask(lineText.size(), 1);
  bool inString = false;
  bool inLineComment = false;
  for (size_t i = 0; i < lineText.size(); i++) {
    char ch = lineText[i];
    char next = (i + 1 < lineText.size()) ? lineText[i + 1] : '\0';

    if (inLineComment) {
      mask[i] = 0;
      continue;
    }
    if (inBlockCommentInOut) {
      mask[i] = 0;
      if (ch == '*' && next == '/') {
        if (i + 1 < mask.size())
          mask[i + 1] = 0;
        inBlockCommentInOut = false;
        i++;
      }
      continue;
    }
    if (inString) {
      mask[i] = 0;
      if (ch == '"' && (i == 0 || lineText[i - 1] != '\\'))
        inString = false;
      continue;
    }
    if (ch == '"') {
      mask[i] = 0;
      inString = true;
      continue;
    }
    if (ch == '/' && next == '/') {
      mask[i] = 0;
      if (i + 1 < mask.size())
        mask[i + 1] = 0;
      inLineComment = true;
      i++;
      continue;
    }
    if (ch == '/' && next == '*') {
      mask[i] = 0;
      if (i + 1 < mask.size())
        mask[i + 1] = 0;
      inBlockCommentInOut = true;
      i++;
      continue;
    }
  }
  return mask;
}

bool isPreprocessorDirectiveLine(const std::string &lineText,
                                 const std::vector<char> &mask) {
  for (size_t i = 0; i < lineText.size() && i < mask.size(); i++) {
    if (!mask[i])
      continue;
    if (std::isspace(static_cast<unsigned char>(lineText[i])))
      continue;
    return lineText[i] == '#';
  }
  return false;
}

bool preprocBranchSigsOverlap(const PreprocBranchSig &a,
                              const PreprocBranchSig &b) {
  size_t i = 0;
  size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i].first < b[j].first) {
      i++;
      continue;
    }
    if (b[j].first < a[i].first) {
      j++;
      continue;
    }
    if (a[i].second != b[j].second)
      return false;
    i++;
    j++;
  }
  return true;
}
