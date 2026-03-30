#include "server_parse.hpp"

#include "nsf_lexer.hpp"
#include "text_utils.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

std::string sanitizeCodeLine(const std::string &line, bool &inBlockComment) {
  std::string out;
  out.reserve(line.size());
  bool inString = false;
  bool inLineComment = false;
  for (size_t i = 0; i < line.size(); i++) {
    const char ch = line[i];
    const char next = i + 1 < line.size() ? line[i + 1] : '\0';
    if (inLineComment) {
      out.push_back(' ');
      continue;
    }
    if (inBlockComment) {
      out.push_back(' ');
      if (ch == '*' && next == '/') {
        out.push_back(' ');
        inBlockComment = false;
        i++;
      }
      continue;
    }
    if (inString) {
      out.push_back(' ');
      if (ch == '"' && (i == 0 || line[i - 1] != '\\'))
        inString = false;
      continue;
    }
    if (ch == '/' && next == '/') {
      out.push_back(' ');
      out.push_back(' ');
      inLineComment = true;
      i++;
      continue;
    }
    if (ch == '/' && next == '*') {
      out.push_back(' ');
      out.push_back(' ');
      inBlockComment = true;
      i++;
      continue;
    }
    if (ch == '"') {
      out.push_back(' ');
      inString = true;
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

std::string trimCodeLine(const std::string &line, bool &inBlockComment) {
  return trimRightCopy(trimLeftCopy(sanitizeCodeLine(line, inBlockComment)));
}

bool shouldAdvanceGroupingState(const std::string &trimmed, size_t lineIndex,
                                const std::vector<char> *lineActive) {
  if (lineActive && lineIndex < lineActive->size() && !(*lineActive)[lineIndex])
    return false;
  return !trimmed.empty() && trimmed[0] != '#';
}

bool isContinuationLine(const std::string &trimmed) {
  if (trimmed.empty())
    return false;
  const std::string ops = "+-*/%&|^!=<>?:,.";
  const char tail = trimmed.back();
  return ops.find(tail) != std::string::npos || tail == '\\';
}

bool startsWithKeywordToken(const std::vector<LexToken> &tokens,
                            const std::string &keyword) {
  for (const auto &token : tokens) {
    if (token.kind == LexToken::Kind::Identifier)
      return token.text == keyword;
  }
  return false;
}

bool isStandaloneAttributeSpecifier(const std::vector<LexToken> &tokens) {
  if (tokens.size() < 2 || tokens.front().kind != LexToken::Kind::Punct ||
      tokens.front().text != "[" || tokens.back().kind != LexToken::Kind::Punct ||
      tokens.back().text != "]") {
    return false;
  }
  int bracketDepth = 0;
  bool sawOpenBracket = false;
  for (size_t i = 0; i < tokens.size(); i++) {
    if (tokens[i].kind != LexToken::Kind::Punct)
      continue;
    if (tokens[i].text == "[") {
      bracketDepth++;
      sawOpenBracket = true;
      continue;
    }
    if (tokens[i].text == "]") {
      if (bracketDepth == 0)
        return false;
      bracketDepth--;
      if (bracketDepth == 0 && i + 1 != tokens.size())
        return false;
    }
  }
  return sawOpenBracket && bracketDepth == 0;
}

bool endsWithPostfixUpdateExpression(const std::vector<LexToken> &tokens) {
  if (tokens.size() < 3)
    return false;
  const auto &last = tokens[tokens.size() - 1];
  const auto &penultimate = tokens[tokens.size() - 2];
  if (last.kind != LexToken::Kind::Punct ||
      penultimate.kind != LexToken::Kind::Punct) {
    return false;
  }
  const bool isIncrement = last.text == "+" && penultimate.text == "+";
  const bool isDecrement = last.text == "-" && penultimate.text == "-";
  if (!isIncrement && !isDecrement)
    return false;
  const auto &base = tokens[tokens.size() - 3];
  return base.kind == LexToken::Kind::Identifier ||
         (base.kind == LexToken::Kind::Punct &&
          (base.text == "]" || base.text == ")"));
}

bool isFunctionLikeHeader(const std::vector<LexToken> &tokens,
                          const std::string &nextTrimmed) {
  size_t openParen = std::string::npos;
  for (size_t i = 0; i < tokens.size(); i++) {
    if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "(") {
      openParen = i;
      break;
    }
  }
  if (openParen == std::string::npos)
    return false;
  int identifierCountBeforeParen = 0;
  for (size_t i = 0; i < openParen; i++) {
    if (tokens[i].kind == LexToken::Kind::Identifier &&
        !isQualifierToken(tokens[i].text)) {
      identifierCountBeforeParen++;
    }
  }
  if (identifierCountBeforeParen < 2)
    return false;
  if (!nextTrimmed.empty() &&
      (nextTrimmed[0] == '{' || nextTrimmed[0] == ':')) {
    return true;
  }
  if (!tokens.empty()) {
    const auto &last = tokens.back();
    if (last.kind == LexToken::Kind::Punct &&
        (last.text == "{" || last.text == ":")) {
      return true;
    }
  }
  return false;
}

} // namespace

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

bool extractMetadataDeclarationHeaderShared(const std::string &line,
                                            std::string &typeOut,
                                            std::string &nameOut) {
  typeOut.clear();
  nameOut.clear();

  std::string code = line;
  const size_t lineComment = code.find("//");
  if (lineComment != std::string::npos)
    code = code.substr(0, lineComment);

  const auto tokens = lexLineTokens(code);
  if (tokens.empty() || tokens[0].text == "#")
    return false;

  bool sawColon = false;
  for (const auto &token : tokens) {
    if (token.kind != LexToken::Kind::Punct)
      continue;
    const std::string &text = token.text;
    if (text == ":") {
      sawColon = true;
      continue;
    }
    if (text == "(" || text == ")" || text == ";" || text == "," ||
        text == "=" || text == "<" || text == ">" || text == "[" ||
        text == "]" || text == "{" || text == "}") {
      return false;
    }
  }

  int identifierCountBeforeColon = 0;
  for (const auto &token : tokens) {
    if (token.kind == LexToken::Kind::Punct && token.text == ":")
      break;
    if (token.kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(token.text))
      continue;
    if (typeOut.empty()) {
      typeOut = token.text;
    } else if (nameOut.empty()) {
      nameOut = token.text;
    } else {
      return false;
    }
    identifierCountBeforeColon++;
  }
  if (identifierCountBeforeColon < 2)
    return false;
  if (sawColon && (typeOut.empty() || nameOut.empty()))
    return false;
  return !typeOut.empty() && !nameOut.empty();
}

bool findMetadataDeclarationHeaderPosShared(const std::string &line,
                                            std::string &typeOut,
                                            std::string &nameOut,
                                            size_t &nameStartOut,
                                            size_t &nameEndOut) {
  nameStartOut = 0;
  nameEndOut = 0;
  if (!extractMetadataDeclarationHeaderShared(line, typeOut, nameOut) ||
      nameOut.empty()) {
    return false;
  }

  std::string code = line;
  const size_t lineComment = code.find("//");
  if (lineComment != std::string::npos)
    code = code.substr(0, lineComment);

  const auto tokens = lexLineTokens(code);
  const LexToken *nameToken = nullptr;
  for (const auto &token : tokens) {
    if (token.kind == LexToken::Kind::Punct && token.text == ":")
      break;
    if (token.kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(token.text))
      continue;
    nameToken = &token;
  }
  if (!nameToken || nameToken->text != nameOut)
    return false;

  nameStartOut = nameToken->start;
  nameEndOut = nameToken->end;
  return true;
}

bool extractFxBlockDeclarationHeaderShared(const std::string &line,
                                           std::string &typeOut,
                                           std::string &nameOut) {
  typeOut.clear();
  nameOut.clear();

  std::string code = line;
  const size_t lineComment = code.find("//");
  if (lineComment != std::string::npos)
    code = code.substr(0, lineComment);

  const auto tokens = lexLineTokens(code);
  if (tokens.size() < 2 || tokens[0].text == "#")
    return false;

  auto isBlockType = [](const std::string &t) {
    return t == "texture" || t == "Texture" || t == "Texture2D" ||
           t == "Texture3D" || t == "TextureCube" || t == "SamplerState" ||
           t == "SamplerComparisonState" || t == "BlendState" ||
           t == "DepthStencilState" || t == "RasterizerState";
  };

  bool hasAssignBefore = false;
  for (const auto &token : tokens) {
    if (token.kind == LexToken::Kind::Punct && token.text == "=")
      return false;
    if (token.kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(token.text))
      continue;
    if (typeOut.empty()) {
      typeOut = token.text;
      continue;
    }
    nameOut = token.text;
    break;
  }

  if (!isBlockType(typeOut) || nameOut.empty())
    return false;
  return true;
}

bool extractMacroDefinitionInLineShared(const std::string &line,
                                        ParsedMacroDefinitionInfo &resultOut) {
  resultOut = ParsedMacroDefinitionInfo{};

  std::string code = line;
  const size_t lineComment = code.find("//");
  if (lineComment != std::string::npos)
    code = code.substr(0, lineComment);

  const auto tokens = lexLineTokens(code);
  if (tokens.size() < 3)
    return false;
  if (tokens[0].kind != LexToken::Kind::Punct || tokens[0].text != "#")
    return false;
  if (tokens[1].kind != LexToken::Kind::Identifier ||
      tokens[1].text != "define") {
    return false;
  }
  if (tokens[2].kind != LexToken::Kind::Identifier)
    return false;

  resultOut.name = tokens[2].text;
  resultOut.nameStart = tokens[2].start;
  resultOut.nameEnd = tokens[2].end;

  size_t replacementStart = tokens[2].end;
  size_t tokenIndex = 3;
  if (tokenIndex < tokens.size() &&
      tokens[tokenIndex].kind == LexToken::Kind::Punct &&
      tokens[tokenIndex].text == "(" &&
      tokens[tokenIndex].start == tokens[2].end) {
    resultOut.isFunctionLike = true;
    int depth = 0;
    bool started = false;
    std::string currentParam;
    for (; tokenIndex < tokens.size(); tokenIndex++) {
      const auto &tok = tokens[tokenIndex];
      if (tok.kind == LexToken::Kind::Punct && tok.text == "(") {
        depth++;
        if (depth == 1) {
          started = true;
          continue;
        }
      } else if (tok.kind == LexToken::Kind::Punct && tok.text == ")") {
        if (depth == 1) {
          const std::string param =
              trimRightCopy(trimLeftCopy(currentParam));
          if (!param.empty())
            resultOut.parameters.push_back(param);
          currentParam.clear();
          replacementStart = tok.end;
          tokenIndex++;
          break;
        }
        if (depth > 0)
          depth--;
      }
      if (!started)
        continue;
      if (tok.kind == LexToken::Kind::Punct && tok.text == "," && depth == 1) {
        const std::string param =
            trimRightCopy(trimLeftCopy(currentParam));
        if (!param.empty())
          resultOut.parameters.push_back(param);
        currentParam.clear();
        continue;
      }
      if (!currentParam.empty())
        currentParam.push_back(' ');
      currentParam += tok.text;
    }
    if (replacementStart <= tokens[2].end)
      replacementStart = tokens[2].end;
  }

  if (replacementStart < code.size()) {
    resultOut.replacementText =
        trimRightCopy(trimLeftCopy(code.substr(replacementStart)));
  }
  return !resultOut.name.empty();
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

std::vector<std::string> splitLinesShared(const std::string &text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line))
    lines.push_back(line);
  if (!text.empty() && text.back() == '\n')
    lines.emplace_back();
  return lines;
}

std::vector<std::string> buildTrimmedCodeLinesShared(const std::string &text) {
  return buildTrimmedCodeLineScanShared(text).trimmedLines;
}

TrimmedCodeLineScanSharedResult buildTrimmedCodeLineScanShared(
    const std::string &text, const std::vector<char> *lineActive) {
  const std::vector<std::string> lines = splitLinesShared(text);
  TrimmedCodeLineScanSharedResult result;
  result.trimmedLines.reserve(lines.size());
  result.parenDepthAfterLine.reserve(lines.size());
  result.bracketDepthAfterLine.reserve(lines.size());

  bool inBlockComment = false;
  int parenDepth = 0;
  int bracketDepth = 0;
  for (size_t lineIndex = 0; lineIndex < lines.size(); lineIndex++) {
    const std::string sanitized = sanitizeCodeLine(lines[lineIndex], inBlockComment);
    const std::string trimmed =
        trimRightCopy(trimLeftCopy(sanitized));
    result.trimmedLines.push_back(trimmed);

    if (shouldAdvanceGroupingState(trimmed, lineIndex, lineActive)) {
      for (char ch : sanitized) {
        if (ch == '(') {
          parenDepth++;
        } else if (ch == ')') {
          parenDepth = std::max(0, parenDepth - 1);
        } else if (ch == '[') {
          bracketDepth++;
        } else if (ch == ']') {
          bracketDepth = std::max(0, bracketDepth - 1);
        }
      }
    }

    result.parenDepthAfterLine.push_back(parenDepth);
    result.bracketDepthAfterLine.push_back(bracketDepth);
  }

  return result;
}

bool shouldReportMissingSemicolonShared(const std::string &trimmed,
                                        const std::string &nextTrimmed,
                                        bool insideOpenGroupingAfterLine) {
  if (trimmed.empty() || trimmed[0] == '#')
    return false;
  const char tail = trimmed.back();
  if (tail == ';' || tail == '{' || tail == '}' || tail == ':' || tail == ',')
    return false;
  if (insideOpenGroupingAfterLine)
    return false;

  const auto tokens = lexLineTokens(trimmed);
  if (tokens.empty())
    return false;
  if (isStandaloneAttributeSpecifier(tokens))
    return false;

  if (startsWithKeywordToken(tokens, "if") ||
      startsWithKeywordToken(tokens, "for") ||
      startsWithKeywordToken(tokens, "while") ||
      startsWithKeywordToken(tokens, "switch") ||
      startsWithKeywordToken(tokens, "else") ||
      startsWithKeywordToken(tokens, "do") ||
      startsWithKeywordToken(tokens, "case") ||
      startsWithKeywordToken(tokens, "default")) {
    return false;
  }
  std::string ignoredName;
  if (extractStructNameInLine(trimmed, ignoredName) ||
      extractCBufferNameInLineShared(trimmed, ignoredName)) {
    return false;
  }
  if (isFunctionLikeHeader(tokens, nextTrimmed))
    return false;
  if (endsWithPostfixUpdateExpression(tokens))
    return true;
  if (isContinuationLine(trimmed))
    return false;

  for (size_t i = 0; i + 1 < tokens.size(); i++) {
    if (tokens[i].kind == LexToken::Kind::Identifier &&
        tokens[i + 1].kind == LexToken::Kind::Punct &&
        tokens[i + 1].text == "=") {
      return true;
    }
  }

  if (!extractDeclarationsInLineShared(trimmed).empty())
    return true;

  if (startsWithKeywordToken(tokens, "return") ||
      startsWithKeywordToken(tokens, "break") ||
      startsWithKeywordToken(tokens, "continue") ||
      startsWithKeywordToken(tokens, "discard")) {
    return true;
  }

  const auto &last = tokens.back();
  if (last.kind == LexToken::Kind::Identifier ||
      (last.kind == LexToken::Kind::Punct &&
       (last.text == ")" || last.text == "]"))) {
    return true;
  }
  return false;
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
