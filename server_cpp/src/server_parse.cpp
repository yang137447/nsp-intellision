#include "server_parse.hpp"

#include "nsf_lexer.hpp"
#include "text_utils.hpp"

#include <cctype>

bool extractIncludePath(const std::string &lineText, std::string &includePath) {
  includePath.clear();
  size_t pos = lineText.find("#include");
  if (pos == std::string::npos)
    return false;
  size_t start = lineText.find('"', pos);
  size_t end = start != std::string::npos ? lineText.find('"', start + 1)
                                          : std::string::npos;
  if (start != std::string::npos && end != std::string::npos &&
      end > start + 1) {
    includePath = lineText.substr(start + 1, end - start - 1);
    return true;
  }
  start = lineText.find('<', pos);
  end = start != std::string::npos ? lineText.find('>', start + 1)
                                   : std::string::npos;
  if (start != std::string::npos && end != std::string::npos &&
      end > start + 1) {
    includePath = lineText.substr(start + 1, end - start - 1);
    return true;
  }
  return false;
}

bool extractMemberAccessBase(const std::string &lineText, int character,
                             std::string &base) {
  if (character <= 0)
    return false;
  size_t index =
      static_cast<size_t>(utf16ToByteOffsetInLine(lineText, character));
  if (index > lineText.size())
    index = lineText.size();

  size_t wordStart = index;
  size_t wordEnd = index;
  if (wordStart > 0 && isIdentifierChar(lineText[wordStart - 1])) {
    while (wordStart > 0 && isIdentifierChar(lineText[wordStart - 1])) {
      wordStart--;
    }
    wordEnd = index;
    while (wordEnd < lineText.size() && isIdentifierChar(lineText[wordEnd])) {
      wordEnd++;
    }
  }

  size_t scan = wordStart;
  if (scan == index) {
    while (scan > 0 &&
           std::isspace(static_cast<unsigned char>(lineText[scan - 1]))) {
      scan--;
    }
  }

  size_t dotPos = std::string::npos;
  if (scan > 0 && lineText[scan - 1] == '.') {
    dotPos = scan - 1;
  } else if (scan < lineText.size() && lineText[scan] == '.') {
    dotPos = scan;
  } else if (wordEnd < lineText.size() && lineText[wordEnd] == '.') {
    dotPos = wordEnd;
  } else {
    return false;
  }

  size_t end = dotPos;
  while (end > 0 && std::isspace(static_cast<unsigned char>(lineText[end - 1]))) {
    end--;
  }
  if (end == 0)
    return false;
  size_t start = end;
  while (start > 0 && isIdentifierChar(lineText[start - 1])) {
    start--;
  }
  if (start == end)
    return false;
  base = lineText.substr(start, end - start);
  return !base.empty();
}

bool extractStructNameInLine(const std::string &line,
                             std::string &structNameOut) {
  std::string trimmed = trimLeftCopy(line);
  if (trimmed.rfind("struct ", 0) != 0)
    return false;
  size_t nameStart = line.find("struct ");
  if (nameStart == std::string::npos)
    return false;
  nameStart += std::string("struct ").size();
  while (nameStart < line.size() &&
         std::isspace(static_cast<unsigned char>(line[nameStart]))) {
    nameStart++;
  }
  size_t nameEnd = nameStart;
  while (nameEnd < line.size() && isIdentifierChar(line[nameEnd])) {
    nameEnd++;
  }
  if (nameEnd <= nameStart)
    return false;
  structNameOut = line.substr(nameStart, nameEnd - nameStart);
  return !structNameOut.empty();
}

bool extractCBufferNameInLineShared(const std::string &line,
                                    std::string &nameOut) {
  const std::string trimmed = trimLeftCopy(line);
  if (trimmed.rfind("cbuffer ", 0) != 0)
    return false;
  const auto tokens = lexLineTokens(trimmed);
  if (tokens.size() < 2)
    return false;
  for (size_t i = 1; i < tokens.size(); i++) {
    if (tokens[i].kind == LexToken::Kind::Identifier) {
      nameOut = tokens[i].text;
      return !nameOut.empty();
    }
  }
  return false;
}

namespace {
struct TokenRange {
  size_t start = 0;
  size_t end = 0;
};

static std::string collapseWhitespace(std::string value) {
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

static std::string trimCopy(std::string value) {
  value = trimLeftCopy(value);
  value = trimRightCopy(value);
  return value;
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

static int lineIndexForOffset(const std::string &text, size_t offset) {
  offset = std::min(offset, text.size());
  int line = 0;
  for (size_t i = 0; i < offset; i++) {
    if (text[i] == '\n')
      line++;
  }
  return line;
}

static std::vector<TokenRange>
splitTopLevelByToken(const std::vector<LexToken> &tokens, size_t start,
                     size_t end, const std::string &separator) {
  std::vector<TokenRange> ranges;
  int angleDepth = 0;
  int parenDepth = 0;
  int bracketDepth = 0;
  size_t segStart = start;
  for (size_t idx = start; idx < end; idx++) {
    const std::string &t = tokens[idx].text;
    if (t == "<")
      angleDepth++;
    else if (t == ">" && angleDepth > 0)
      angleDepth--;
    else if (t == "(")
      parenDepth++;
    else if (t == ")" && parenDepth > 0)
      parenDepth--;
    else if (t == "[")
      bracketDepth++;
    else if (t == "]" && bracketDepth > 0)
      bracketDepth--;
    if (angleDepth == 0 && parenDepth == 0 && bracketDepth == 0 &&
        t == separator) {
      ranges.push_back(TokenRange{segStart, idx});
      segStart = idx + 1;
    }
  }
  if (segStart < end)
    ranges.push_back(TokenRange{segStart, end});
  return ranges;
}

static bool isBlockedDeclarationStart(const std::vector<LexToken> &tokens,
                                      const TokenRange &range) {
  for (size_t idx = range.start; idx < range.end; idx++) {
    if (tokens[idx].kind != LexToken::Kind::Identifier)
      continue;
    const std::string &t = tokens[idx].text;
    return t == "return" || t == "if" || t == "for" || t == "while" ||
           t == "switch" || t == "case" || t == "else" || t == "do";
  }
  return false;
}

static const LexToken *
findDeclaratorTokenInRange(const std::vector<LexToken> &tokens,
                           const TokenRange &range, bool requireTypePrefix) {
  int a = 0;
  int p = 0;
  int b = 0;
  const LexToken *candidate = nullptr;
  bool sawParenAtTop = false;
  bool sawEqualAtTop = false;
  for (size_t idx = range.start; idx < range.end; idx++) {
    const auto &tok = tokens[idx];
    const std::string &t = tok.text;
    if (t == "<")
      a++;
    else if (t == ">" && a > 0)
      a--;
    else if (t == "(")
      p++;
    else if (t == ")" && p > 0)
      p--;
    else if (t == "[")
      b++;
    else if (t == "]" && b > 0)
      b--;

    bool top = a == 0 && p == 0 && b == 0;
    if (!top)
      continue;

    if (t == ":" || t == "=") {
      if (t == "=")
        sawEqualAtTop = true;
      break;
    }
    if (t == "(" && !sawEqualAtTop)
      sawParenAtTop = true;

    if (tok.kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(t))
      continue;

    candidate = &tok;
  }

  if (!candidate)
    return nullptr;
  if (requireTypePrefix) {
    bool hasIdentifierBefore = false;
    int aa = 0, pp = 0, bb = 0;
    for (size_t idx = range.start; idx < range.end; idx++) {
      const auto &tok = tokens[idx];
      const std::string &t = tok.text;
      if (t == "<")
        aa++;
      else if (t == ">" && aa > 0)
        aa--;
      else if (t == "(")
        pp++;
      else if (t == ")" && pp > 0)
        pp--;
      else if (t == "[")
        bb++;
      else if (t == "]" && bb > 0)
        bb--;
      if (!(aa == 0 && pp == 0 && bb == 0))
        continue;
      if (&tok == candidate)
        break;
      if (tok.kind == LexToken::Kind::Identifier && !isQualifierToken(tok.text))
        hasIdentifierBefore = true;
    }
    if (!hasIdentifierBefore)
      return nullptr;
  }
  if (sawParenAtTop)
    return nullptr;

  const size_t candidateIndex = static_cast<size_t>(candidate - &tokens[0]);
  if (candidateIndex > range.start) {
    const std::string &prev = tokens[candidateIndex - 1].text;
    if (prev == "." || prev == "->" || prev == "::")
      return nullptr;
  }
  return candidate;
}

static bool findBaseTypeBeforeToken(const std::vector<LexToken> &tokens,
                                    const TokenRange &range,
                                    size_t declaratorIndex,
                                    std::string &typeOut) {
  typeOut.clear();
  int a = 0;
  int p = 0;
  int b = 0;
  for (size_t idx = range.start; idx < range.end; idx++) {
    if (idx >= declaratorIndex)
      break;
    const auto &tok = tokens[idx];
    const std::string &t = tok.text;
    if (t == "<")
      a++;
    else if (t == ">" && a > 0)
      a--;
    else if (t == "(")
      p++;
    else if (t == ")" && p > 0)
      p--;
    else if (t == "[")
      b++;
    else if (t == "]" && b > 0)
      b--;
    if (!(a == 0 && p == 0 && b == 0))
      continue;
    if (tok.kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(t))
      continue;
    if (idx > range.start && tokens[idx - 1].kind == LexToken::Kind::Punct &&
        (tokens[idx - 1].text == "." || tokens[idx - 1].text == "->" ||
         tokens[idx - 1].text == "::")) {
      continue;
    }
    typeOut = t;
    return true;
  }
  return false;
}

static std::string extractTrailingIdentifierBeforePunct(
    const std::vector<LexToken> &tokens, const std::string &punct) {
  if (tokens.empty())
    return std::string();
  size_t end = tokens.size();
  for (size_t i = 0; i < tokens.size(); i++) {
    if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == punct) {
      end = i;
      break;
    }
  }
  for (size_t i = end; i > 0; i--) {
    const auto &token = tokens[i - 1];
    if (token.kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(token.text))
      continue;
    return token.text;
  }
  return std::string();
}

static std::vector<ParsedDeclarationInfo>
parseDeclarationsInLine(const std::string &line) {
  std::string code = line;
  const size_t lineComment = code.find("//");
  if (lineComment != std::string::npos)
    code = code.substr(0, lineComment);

  const auto tokens = lexLineTokens(code);
  if (tokens.empty() || tokens[0].text == "#")
    return {};

  const auto statementRanges =
      splitTopLevelByToken(tokens, 0, tokens.size(), ";");
  if (statementRanges.empty())
    return {};

  std::vector<ParsedDeclarationInfo> parsed;
  for (const auto &statement : statementRanges) {
    if (statement.start >= statement.end)
      continue;
    if (isBlockedDeclarationStart(tokens, statement))
      continue;
    const auto segments =
        splitTopLevelByToken(tokens, statement.start, statement.end, ",");
    if (segments.empty())
      continue;

    const LexToken *firstDecl = findDeclaratorTokenInRange(tokens, segments[0], true);
    if (!firstDecl)
      continue;
    const size_t firstDeclIndex = static_cast<size_t>(firstDecl - &tokens[0]);
    std::string baseType;
    if (!findBaseTypeBeforeToken(tokens, segments[0], firstDeclIndex, baseType) ||
        baseType.empty()) {
      continue;
    }

    parsed.push_back(ParsedDeclarationInfo{
        baseType, firstDecl->text, firstDecl->start, firstDecl->end});
    for (size_t i = 1; i < segments.size(); i++) {
      const LexToken *decl = findDeclaratorTokenInRange(tokens, segments[i], false);
      if (!decl)
        continue;
      parsed.push_back(ParsedDeclarationInfo{baseType, decl->text, decl->start,
                                             decl->end});
    }
  }
  return parsed;
}
} // namespace

bool extractTypedefDeclInLineShared(const std::string &line,
                                    std::string &aliasOut,
                                    std::string &underlyingTypeOut) {
  aliasOut.clear();
  underlyingTypeOut.clear();
  const std::string trimmed = trimLeftCopy(line);
  if (trimmed.rfind("typedef ", 0) != 0)
    return false;
  const auto tokens = lexLineTokens(trimmed);
  if (tokens.empty() || tokens[0].text != "typedef")
    return false;

  aliasOut = extractTrailingIdentifierBeforePunct(tokens, ";");
  for (size_t i = 1; i < tokens.size(); i++) {
    if (tokens[i].kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(tokens[i].text))
      continue;
    if (tokens[i].text == aliasOut)
      break;
    underlyingTypeOut = tokens[i].text;
    break;
  }
  return !aliasOut.empty();
}

bool extractUiMetadataDeclarationHeaderShared(const std::string &line,
                                              std::string &typeOut,
                                              std::string &nameOut) {
  typeOut.clear();
  nameOut.clear();

  std::string code = line;
  const size_t lineComment = code.find("//");
  if (lineComment != std::string::npos)
    code = code.substr(0, lineComment);

  const auto tokens = lexLineTokens(code);
  if (tokens.size() < 2)
    return false;
  if (tokens[0].text == "#")
    return false;
  for (const auto &token : tokens) {
    if (token.kind != LexToken::Kind::Punct)
      continue;
    const std::string &text = token.text;
    if (text == "(" || text == ")" || text == ";" || text == "," ||
        text == "=" || text == ":" || text == "<" || text == ">" ||
        text == "[" || text == "]" || text == "{" || text == "}") {
      return false;
    }
  }

  int identifierCount = 0;
  for (const auto &token : tokens) {
    if (token.kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(token.text))
      continue;
    if (typeOut.empty())
      typeOut = token.text;
    nameOut = token.text;
    identifierCount++;
  }
  return identifierCount >= 2 && !typeOut.empty() && !nameOut.empty();
}

bool extractFunctionSignatureFromTextShared(
    const std::string &text, int lineIndex, int nameCharacter,
    const std::string &name, ParsedFunctionSignatureTextInfo &resultOut) {
  resultOut = ParsedFunctionSignatureTextInfo{};
  if (name.empty())
    return false;

  const size_t nameOffset = positionToOffsetUtf16(text, lineIndex, nameCharacter);
  if (nameOffset >= text.size())
    return false;
  const size_t afterName = nameOffset + name.size();
  if (afterName > text.size())
    return false;

  size_t scan = afterName;
  while (scan < text.size() &&
         std::isspace(static_cast<unsigned char>(text[scan]))) {
    scan++;
  }
  if (scan >= text.size() || text[scan] != '(')
    return false;
  const size_t openParen = scan;

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
    }
  }
  if (closeParen == std::string::npos)
    return false;

  resultOut.signatureEndLine = lineIndexForOffset(text, closeParen);

  size_t lineStart = nameOffset;
  while (lineStart > 0 && text[lineStart - 1] != '\n')
    lineStart--;
  while (lineStart < text.size() &&
         std::isspace(static_cast<unsigned char>(text[lineStart])) &&
         text[lineStart] != '\n') {
    lineStart++;
  }

  resultOut.label = collapseWhitespace(
      text.substr(lineStart, (closeParen + 1) - lineStart));
  if (resultOut.label.empty())
    return false;

  if (closeParen > openParen + 1) {
    std::string params =
        collapseWhitespace(text.substr(openParen + 1, closeParen - openParen - 1));
    params = trimCopy(params);
    if (!params.empty() && params != "void")
      resultOut.parameters = splitParamsTopLevel(params);
  }

  for (size_t i = closeParen + 1; i < text.size(); i++) {
    const char ch = text[i];
    if (ch == '{') {
      resultOut.hasBody = true;
      resultOut.bodyStartLine = lineIndexForOffset(text, i);
      int braceDepth = 0;
      for (size_t j = i; j < text.size(); j++) {
        if (text[j] == '{') {
          braceDepth++;
          continue;
        }
        if (text[j] == '}') {
          braceDepth--;
          if (braceDepth == 0) {
            resultOut.bodyEndLine = lineIndexForOffset(text, j);
            break;
          }
        }
      }
      break;
    }
    if (ch == ';')
      break;
  }

  return true;
}

std::vector<std::string> extractDeclaredNamesFromLine(const std::string &line) {
  const auto parsed = parseDeclarationsInLine(line);
  std::vector<std::string> names;
  names.reserve(parsed.size());
  for (const auto &item : parsed) {
    names.push_back(item.name);
  }
  return names;
}

std::vector<ParsedDeclarationInfo>
extractDeclarationsInLineShared(const std::string &line) {
  return parseDeclarationsInLine(line);
}

bool findTypeOfIdentifierInDeclarationLineShared(const std::string &line,
                                                 const std::string &identifier,
                                                 std::string &typeNameOut) {
  typeNameOut.clear();
  const auto parsed = parseDeclarationsInLine(line);
  for (const auto &item : parsed) {
    if (item.name == identifier) {
      typeNameOut = item.type;
      return true;
    }
  }
  return false;
}
