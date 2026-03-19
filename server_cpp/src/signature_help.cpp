#include "signature_help.hpp"

#include "callsite_parser.hpp"
#include "nsf_lexer.hpp"
#include "text_utils.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

static std::string collapseWhitespace(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  bool inSpace = false;
  for (char ch : value) {
    if (ch == '\r' || ch == '\n' || ch == '\t' ||
        std::isspace(static_cast<unsigned char>(ch))) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
      continue;
    }
    inSpace = false;
    out.push_back(ch);
  }
  while (!out.empty() && out.front() == ' ')
    out.erase(out.begin());
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

static std::string trimCopy(const std::string &value) {
  return trimRightCopy(trimLeftCopy(value));
}

static std::vector<std::string> splitParamsTopLevel(const std::string &params) {
  std::vector<std::string> result;
  int paren = 0;
  int angle = 0;
  int bracket = 0;
  size_t start = 0;
  for (size_t i = 0; i < params.size(); i++) {
    char ch = params[i];
    if (ch == '(')
      paren++;
    else if (ch == ')' && paren > 0)
      paren--;
    else if (ch == '<')
      angle++;
    else if (ch == '>' && angle > 0)
      angle--;
    else if (ch == '[')
      bracket++;
    else if (ch == ']' && bracket > 0)
      bracket--;
    if (ch == ',' && paren == 0 && angle == 0 && bracket == 0) {
      result.push_back(trimCopy(params.substr(start, i - start)));
      start = i + 1;
    }
  }
  std::string last = trimCopy(params.substr(start));
  if (!last.empty())
    result.push_back(last);
  return result;
}

bool parseCallAtOffset(const std::string &text, size_t cursorOffset,
                       std::string &functionNameOut,
                       int &activeParameterOut) {
  return parseCallSiteAtOffset(text, cursorOffset, functionNameOut,
                               activeParameterOut);
}

bool extractFunctionSignatureAt(const std::string &text, int lineIndex,
                                int nameCharacter, const std::string &name,
                                std::string &labelOut,
                                std::vector<std::string> &parametersOut) {
  labelOut.clear();
  parametersOut.clear();
  if (name.empty())
    return false;
  size_t nameOffset = positionToOffsetUtf16(text, lineIndex, nameCharacter);
  if (nameOffset >= text.size())
    return false;
  size_t afterName = nameOffset + name.size();
  if (afterName > text.size())
    return false;

  size_t scan = afterName;
  while (scan < text.size() &&
         std::isspace(static_cast<unsigned char>(text[scan]))) {
    scan++;
  }
  if (scan >= text.size() || text[scan] != '(')
    return false;
  size_t openParen = scan;

  int depth = 0;
  size_t closeParen = std::string::npos;
  for (size_t i = openParen; i < text.size(); i++) {
    char ch = text[i];
    if (ch == '(') {
      depth++;
      continue;
    }
    if (ch == ')') {
      depth--;
      if (depth == 0) {
        closeParen = i;
        break;
      }
      continue;
    }
  }
  if (closeParen == std::string::npos)
    return false;

  size_t lineStart = nameOffset;
  while (lineStart > 0 && text[lineStart - 1] != '\n')
    lineStart--;
  while (lineStart < text.size() &&
         std::isspace(static_cast<unsigned char>(text[lineStart])) &&
         text[lineStart] != '\n') {
    lineStart++;
  }

  std::string rawLabel =
      text.substr(lineStart, (closeParen + 1) - lineStart);
  labelOut = collapseWhitespace(rawLabel);

  if (closeParen > openParen + 1) {
    std::string params = text.substr(openParen + 1, closeParen - openParen - 1);
    params = collapseWhitespace(params);
    params = trimCopy(params);
    if (!params.empty() && params != "void") {
      parametersOut = splitParamsTopLevel(params);
    }
  }
  return !labelOut.empty();
}
