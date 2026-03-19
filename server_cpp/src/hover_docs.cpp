#include "hover_docs.hpp"

#include "nsf_lexer.hpp"
#include "text_utils.hpp"

#include <cctype>
#include <sstream>
#include <vector>

static std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  return lines;
}

static std::string stripLineCommentPrefix(const std::string &line) {
  size_t start = 0;
  while (start < line.size() &&
         std::isspace(static_cast<unsigned char>(line[start]))) {
    start++;
  }
  if (start + 1 < line.size() && line[start] == '/' && line[start + 1] == '/') {
    start += 2;
    if (start < line.size() && line[start] == '/')
      start++;
    if (start < line.size() && line[start] == ' ')
      start++;
    return line.substr(start);
  }
  return "";
}

static std::string stripBlockCommentDecorations(const std::string &line) {
  std::string trimmed = trimLeftCopy(line);
  if (!trimmed.empty() && trimmed[0] == '*') {
    size_t start = 1;
    if (start < trimmed.size() && trimmed[start] == ' ')
      start++;
    return trimmed.substr(start);
  }
  return trimmed;
}

std::string extractLeadingDocumentationAtLine(const std::string &text,
                                              int lineIndex) {
  auto lines = splitLines(text);
  if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size()))
    return "";

  std::vector<std::string> docLines;
  int i = lineIndex - 1;
  while (i >= 0) {
    if (isBlankLine(lines[i]))
      break;
    std::string stripped = stripLineCommentPrefix(lines[i]);
    if (!stripped.empty()) {
      docLines.push_back(stripped);
      i--;
      continue;
    }
    break;
  }
  if (!docLines.empty()) {
    std::string result;
    for (auto it = docLines.rbegin(); it != docLines.rend(); ++it) {
      if (!result.empty())
        result.push_back('\n');
      result += *it;
    }
    return trimRightCopy(result);
  }

  int j = lineIndex - 1;
  while (j >= 0 && isBlankLine(lines[j])) {
    j--;
  }
  if (j < 0)
    return "";
  std::string candidate = trimRightCopy(lines[j]);
  size_t endPos = candidate.rfind("*/");
  if (endPos == std::string::npos)
    return "";

  std::vector<std::string> blockLines;
  bool started = false;
  for (int k = j; k >= 0; k--) {
    std::string current = lines[k];
    blockLines.push_back(current);
    if (current.find("/*") != std::string::npos) {
      started = true;
      break;
    }
  }
  if (!started)
    return "";

  std::string result;
  for (auto it = blockLines.rbegin(); it != blockLines.rend(); ++it) {
    std::string lineText = *it;
    size_t openPos = lineText.find("/*");
    if (openPos != std::string::npos) {
      lineText = lineText.substr(openPos + 2);
    }
    size_t closePos = lineText.find("*/");
    if (closePos != std::string::npos) {
      lineText = lineText.substr(0, closePos);
    }
    lineText = stripBlockCommentDecorations(lineText);
    lineText = trimRightCopy(lineText);
    if (isBlankLine(lineText))
      continue;
    if (!result.empty())
      result.push_back('\n');
    result += lineText;
  }
  return trimRightCopy(result);
}

std::string extractTrailingInlineCommentAtLine(const std::string &text,
                                               int lineIndex,
                                               int minCharacter) {
  auto lines = splitLines(text);
  if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size()))
    return "";
  const std::string &line = lines[static_cast<size_t>(lineIndex)];
  if (line.empty())
    return "";

  size_t start = 0;
  if (minCharacter > 0) {
    int byteStart = utf16ToByteOffsetInLine(line, minCharacter);
    if (byteStart > 0)
      start = std::min(static_cast<size_t>(byteStart), line.size());
  }

  bool inString = false;
  for (size_t i = start; i + 1 < line.size(); i++) {
    char ch = line[i];
    char next = line[i + 1];
    if (ch == '"' && (i == 0 || line[i - 1] != '\\')) {
      inString = !inString;
      continue;
    }
    if (inString)
      continue;
    if (ch == '/' && next == '/') {
      size_t commentStart = i + 2;
      if (commentStart < line.size() && line[commentStart] == '/')
        commentStart++;
      if (commentStart < line.size() && line[commentStart] == ' ')
        commentStart++;
      std::string comment = line.substr(commentStart);
      return trimRightCopy(comment);
    }
  }
  return "";
}
