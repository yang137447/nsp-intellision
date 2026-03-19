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

namespace {
struct TokenRange {
  size_t start = 0;
  size_t end = 0;
};

struct ParsedDeclaration {
  std::string type;
  std::string name;
  size_t start = 0;
  size_t end = 0;
};

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

static std::vector<ParsedDeclaration>
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

  std::vector<ParsedDeclaration> parsed;
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

    parsed.push_back(ParsedDeclaration{
        baseType, firstDecl->text, firstDecl->start, firstDecl->end});
    for (size_t i = 1; i < segments.size(); i++) {
      const LexToken *decl = findDeclaratorTokenInRange(tokens, segments[i], false);
      if (!decl)
        continue;
      parsed.push_back(
          ParsedDeclaration{baseType, decl->text, decl->start, decl->end});
    }
  }
  return parsed;
}
} // namespace

std::vector<std::string> extractDeclaredNamesFromLine(const std::string &line) {
  const auto parsed = parseDeclarationsInLine(line);
  std::vector<std::string> names;
  names.reserve(parsed.size());
  for (const auto &item : parsed) {
    names.push_back(item.name);
  }
  return names;
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
