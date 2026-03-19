#include "declaration_query.hpp"

#include "nsf_lexer.hpp"
#include "server_request_handlers.hpp"
#include "text_utils.hpp"

#include <cctype>
#include <sstream>
#include <string>

bool findDeclaredIdentifierInDeclarationLine(const std::string &line,
                                             const std::string &word,
                                             size_t &posOut);

namespace {

bool findUiMetadataDeclarationHeaderPos(const std::string &line,
                                        const std::string &word,
                                        size_t &posOut) {
  std::string code = line;
  size_t lineComment = code.find("//");
  if (lineComment != std::string::npos) {
    code = code.substr(0, lineComment);
  }
  auto tokens = lexLineTokens(code);
  if (tokens.size() < 2) {
    return false;
  }
  if (tokens[0].text == "#") {
    return false;
  }
  for (const auto &tok : tokens) {
    if (tok.kind != LexToken::Kind::Punct) {
      continue;
    }
    const std::string &text = tok.text;
    if (text == "(" || text == ")" || text == ";" || text == "," ||
        text == "=" || text == ":" || text == "<" || text == ">" ||
        text == "[" || text == "]" || text == "{" || text == "}") {
      return false;
    }
  }
  const LexToken *lastId = nullptr;
  int identCount = 0;
  for (const auto &tok : tokens) {
    if (tok.kind != LexToken::Kind::Identifier) {
      continue;
    }
    if (isQualifierToken(tok.text)) {
      continue;
    }
    lastId = &tok;
    identCount++;
  }
  if (identCount < 2 || !lastId) {
    return false;
  }
  if (lastId->text != word) {
    return false;
  }
  posOut = lastId->start;
  return true;
}

} // namespace

bool findBestDeclarationUpTo(const std::string &text, const std::string &word,
                             size_t maxOffset, DeclCandidate &out) {
  out = DeclCandidate{};
  bool found = false;
  int bestDepth = -1;
  int bestLine = -1;
  int currentDepth = 0;
  bool inBlockComment = false;
  bool inString = false;
  size_t lineStartOffset = 0;
  bool pendingUi = false;
  int pendingUiLine = -1;
  int pendingUiDepth = -1;
  size_t pendingUiPos = 0;
  std::string pendingUiHeaderLine;

  std::istringstream stream(text);
  std::string line;
  int lineIndex = 0;
  while (std::getline(stream, line)) {
    if (lineStartOffset >= maxOffset) {
      break;
    }
    int lineDepth = currentDepth;
    if (pendingUi) {
      std::string code = line;
      size_t lineComment = code.find("//");
      if (lineComment != std::string::npos) {
        code = code.substr(0, lineComment);
      }
      size_t start = 0;
      while (start < code.size() &&
             std::isspace(static_cast<unsigned char>(code[start]))) {
        start++;
      }
      if (start < code.size()) {
        if (code[start] == '<') {
          if (!found || pendingUiDepth > bestDepth ||
              (pendingUiDepth == bestDepth && pendingUiLine > bestLine)) {
            found = true;
            bestDepth = pendingUiDepth;
            bestLine = pendingUiLine;
            out.found = true;
            out.line = pendingUiLine;
            out.braceDepth = pendingUiDepth;
            out.nameBytePos = pendingUiPos;
            out.lineText = pendingUiHeaderLine;
          }
          pendingUi = false;
        } else {
          pendingUi = false;
        }
      }
    }
    size_t pos = 0;
    if (findDeclaredIdentifierInDeclarationLine(line, word, pos)) {
      if (!found || lineDepth > bestDepth ||
          (lineDepth == bestDepth && lineIndex > bestLine)) {
        found = true;
        bestDepth = lineDepth;
        bestLine = lineIndex;
        out.found = true;
        out.line = lineIndex;
        out.braceDepth = lineDepth;
        out.nameBytePos = pos;
        out.lineText = line;
      }
    }
    if (!pendingUi) {
      size_t uiPos = 0;
      if (findUiMetadataDeclarationHeaderPos(line, word, uiPos)) {
        pendingUi = true;
        pendingUiLine = lineIndex;
        pendingUiDepth = lineDepth;
        pendingUiPos = uiPos;
        pendingUiHeaderLine = line;
      }
    }

    bool inLineComment = false;
    for (size_t i = 0; i < line.size(); i++) {
      char ch = line[i];
      char next = i + 1 < line.size() ? line[i + 1] : '\0';
      if (inLineComment) {
        break;
      }
      if (inBlockComment) {
        if (ch == '*' && next == '/') {
          inBlockComment = false;
          i++;
        }
        continue;
      }
      if (inString) {
        if (ch == '"' && (i == 0 || line[i - 1] != '\\')) {
          inString = false;
        }
        continue;
      }
      if (ch == '/' && next == '*') {
        inBlockComment = true;
        i++;
        continue;
      }
      if (ch == '/' && next == '/') {
        inLineComment = true;
        i++;
        continue;
      }
      if (ch == '"') {
        inString = true;
        continue;
      }
      if (ch == '{') {
        currentDepth++;
      } else if (ch == '}' && currentDepth > 0) {
        currentDepth--;
      }
    }

    lineStartOffset += line.size() + 1;
    lineIndex++;
  }
  return found;
}
