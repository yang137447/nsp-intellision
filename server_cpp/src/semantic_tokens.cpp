#include "semantic_tokens.hpp"

#include "language_registry.hpp"
#include "lsp_helpers.hpp"
#include "nsf_lexer.hpp"
#include "text_utils.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

struct RawSemanticToken {
  int line = 0;
  int start = 0;
  int length = 0;
  int tokenType = 0;
  int tokenModifiers = 0;
};

static std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  return lines;
}

static int findTokenTypeIndex(const SemanticTokenLegend &legend,
                              const std::string &tokenType) {
  for (size_t i = 0; i < legend.tokenTypes.size(); i++) {
    if (legend.tokenTypes[i] == tokenType)
      return static_cast<int>(i);
  }
  return 0;
}

SemanticTokenLegend createDefaultSemanticTokenLegend() {
  SemanticTokenLegend legend;
  legend.tokenTypes = {"comment",  "string", "keyword", "number",
                       "macro",    "function", "variable", "type",
                       "property", "operator"};
  legend.tokenModifiers = {};
  return legend;
}

static bool isNumberStart(const std::string &line, size_t i) {
  char ch = line[i];
  if (std::isdigit(static_cast<unsigned char>(ch)))
    return true;
  if (ch == '.' && i + 1 < line.size() &&
      std::isdigit(static_cast<unsigned char>(line[i + 1])))
    return true;
  return false;
}

static size_t scanNumberToken(const std::string &line, size_t start) {
  size_t i = start;
  bool sawDot = false;
  if (i < line.size() && line[i] == '.') {
    sawDot = true;
    i++;
  }
  while (i < line.size() &&
         std::isdigit(static_cast<unsigned char>(line[i]))) {
    i++;
  }
  if (!sawDot && i < line.size() && line[i] == '.') {
    sawDot = true;
    i++;
    while (i < line.size() &&
           std::isdigit(static_cast<unsigned char>(line[i]))) {
      i++;
    }
  }
  if (i < line.size() && (line[i] == 'f' || line[i] == 'h')) {
    i++;
  }
  return i;
}

static size_t scanStringToken(const std::string &line, size_t start) {
  size_t i = start + 1;
  while (i < line.size()) {
    char ch = line[i];
    if (ch == '"' && line[i - 1] != '\\') {
      return i + 1;
    }
    i++;
  }
  return line.size();
}

static size_t scanIdentifierToken(const std::string &line, size_t start) {
  size_t i = start;
  while (i < line.size() && isIdentifierChar(line[i]))
    i++;
  return i;
}

static size_t scanToNonSpace(const std::string &line, size_t start) {
  size_t i = start;
  while (i < line.size() &&
         std::isspace(static_cast<unsigned char>(line[i]))) {
    i++;
  }
  return i;
}

static std::string trimLeft(const std::string &line) { return trimLeftCopy(line); }

static void tokenizeLine(const std::string &line, int lineIndex,
                         const SemanticTokenLegend &legend,
                         bool &inBlockComment, std::vector<RawSemanticToken> &out) {
  const int commentType = findTokenTypeIndex(legend, "comment");
  const int stringType = findTokenTypeIndex(legend, "string");
  const int keywordType = findTokenTypeIndex(legend, "keyword");
  const int numberType = findTokenTypeIndex(legend, "number");
  const int macroType = findTokenTypeIndex(legend, "macro");
  const int functionType = findTokenTypeIndex(legend, "function");
  const int variableType = findTokenTypeIndex(legend, "variable");
  const int typeType = findTokenTypeIndex(legend, "type");
  const int propertyType = findTokenTypeIndex(legend, "property");
  const int operatorType = findTokenTypeIndex(legend, "operator");

  static const std::unordered_set<std::string> typeKeywords = {
      "void",    "bool",    "int",     "uint",   "float",   "float2",
      "float3",  "float4",  "float2x2","float3x3","float4x4",
      "half",    "half2",   "half3",   "half4",  "Texture2D",
      "Texture3D","SamplerState", "SamplerComparisonState"};

  bool expectTypeIdentifier = false;
  int attributeDepth = 0;

  auto pushToken = [&](size_t startByte, size_t endByte, int tokType,
                       int tokModifiers) {
    if (endByte < startByte)
      endByte = startByte;
    int start = byteOffsetInLineToUtf16(line, static_cast<int>(startByte));
    int end = byteOffsetInLineToUtf16(line, static_cast<int>(endByte));
    if (end < start)
      end = start;
    out.push_back(
        RawSemanticToken{lineIndex, start, end - start, tokType, tokModifiers});
  };

  auto prevNonSpaceChar = [&](size_t index) -> char {
    if (index == 0)
      return '\0';
    size_t i = index;
    while (i > 0) {
      i--;
      char ch = line[i];
      if (!std::isspace(static_cast<unsigned char>(ch)))
        return ch;
    }
    return '\0';
  };

  auto isAttributeBracketStart = [&](size_t index) {
    char prev = prevNonSpaceChar(index);
    return prev == '\0' || prev == '{' || prev == ';' || prev == ']';
  };

  size_t i = 0;
  while (i < line.size()) {
    if (inBlockComment) {
      size_t end = line.find("*/", i);
      size_t tokenEnd = end == std::string::npos ? line.size() : end + 2;
      pushToken(i, tokenEnd, commentType, 0);
      if (end == std::string::npos) {
        return;
      }
      inBlockComment = false;
      i = tokenEnd;
      continue;
    }

    char ch = line[i];
    char next = (i + 1 < line.size()) ? line[i + 1] : '\0';
    if (std::isspace(static_cast<unsigned char>(ch))) {
      i++;
      continue;
    }

    std::string trimmed = trimLeft(line);
    if (i == 0 && !trimmed.empty() && trimmed[0] == '#') {
      size_t hashPos = line.find('#');
      if (hashPos != std::string::npos) {
        pushToken(hashPos, hashPos + 1, macroType, 0);
        size_t wordStart = scanToNonSpace(line, hashPos + 1);
        size_t wordEnd = wordStart;
        while (wordEnd < line.size() && isIdentifierChar(line[wordEnd]))
          wordEnd++;
        if (wordEnd > wordStart) {
          pushToken(wordStart, wordEnd, macroType, 0);
        }
        size_t quote = line.find('"', wordEnd);
        if (quote != std::string::npos) {
          size_t quoteEnd = line.find('"', quote + 1);
          if (quoteEnd != std::string::npos && quoteEnd > quote + 1) {
            pushToken(quote + 1, quoteEnd, stringType, 0);
          }
        } else {
          size_t lt = line.find('<', wordEnd);
          if (lt != std::string::npos) {
            size_t gt = line.find('>', lt + 1);
            if (gt != std::string::npos && gt > lt + 1) {
              pushToken(lt + 1, gt, stringType, 0);
            }
          }
        }
      }
      return;
    }

    if (ch == '/' && next == '/') {
      pushToken(i, line.size(), commentType, 0);
      return;
    }
    if (ch == '/' && next == '*') {
      size_t end = line.find("*/", i + 2);
      size_t tokenEnd = end == std::string::npos ? line.size() : end + 2;
      pushToken(i, tokenEnd, commentType, 0);
      if (end == std::string::npos) {
        inBlockComment = true;
        return;
      }
      i = tokenEnd;
      continue;
    }
    if (ch == '[' && isAttributeBracketStart(i)) {
      attributeDepth++;
      i++;
      continue;
    }
    if (ch == ']' && attributeDepth > 0) {
      attributeDepth--;
      i++;
      continue;
    }
    if (ch == '"') {
      size_t end = scanStringToken(line, i);
      pushToken(i, end, stringType, 0);
      i = end;
      continue;
    }
    if (isNumberStart(line, i)) {
      size_t end = scanNumberToken(line, i);
      pushToken(i, end, numberType, 0);
      i = end;
      continue;
    }
    if (isIdentifierChar(ch)) {
      size_t end = scanIdentifierToken(line, i);
      std::string word = line.substr(i, end - i);
      int tokType = variableType;
      if (attributeDepth > 0) {
        tokType = keywordType;
      } else if (isHlslKeyword(word) || isHlslSystemSemantic(word)) {
        tokType = keywordType;
        if (word == "struct" || word == "cbuffer")
          expectTypeIdentifier = true;
      } else if (typeKeywords.find(word) != typeKeywords.end() ||
                 expectTypeIdentifier) {
        tokType = typeType;
        expectTypeIdentifier = false;
      } else {
        size_t after = scanToNonSpace(line, end);
        if (after < line.size() && line[after] == '(') {
          tokType = functionType;
        } else if (i > 0 && line[i - 1] == '.') {
          tokType = propertyType;
        }
      }
      pushToken(i, end, tokType, 0);
      i = end;
      continue;
    }

    if (std::string("+-*/%=&|^!~?:<>").find(ch) != std::string::npos) {
      pushToken(i, i + 1, operatorType, 0);
      i++;
      continue;
    }

    i++;
  }
}

static Json encodeTokens(const std::vector<RawSemanticToken> &tokens) {
  Json data = makeArray();
  int lastLine = 0;
  int lastStart = 0;
  bool hasLast = false;
  for (const auto &t : tokens) {
    int deltaLine = hasLast ? (t.line - lastLine) : t.line;
    int deltaStart = 0;
    if (!hasLast) {
      deltaStart = t.start;
    } else {
      deltaStart = (deltaLine == 0) ? (t.start - lastStart) : t.start;
    }
    data.a.push_back(makeNumber(deltaLine));
    data.a.push_back(makeNumber(deltaStart));
    data.a.push_back(makeNumber(t.length));
    data.a.push_back(makeNumber(t.tokenType));
    data.a.push_back(makeNumber(t.tokenModifiers));
    lastLine = t.line;
    lastStart = t.start;
    hasLast = true;
  }
  Json result = makeObject();
  result.o["data"] = data;
  return result;
}

static std::vector<RawSemanticToken>
collectTokensForLineRange(const std::vector<std::string> &lines,
                          int startLine, int endLine,
                          const SemanticTokenLegend &legend) {
  std::vector<RawSemanticToken> tokens;
  bool inBlockComment = false;
  for (int lineIndex = 0; lineIndex < static_cast<int>(lines.size()); lineIndex++) {
    if (lineIndex < startLine)
      continue;
    if (lineIndex > endLine)
      break;
    tokenizeLine(lines[lineIndex], lineIndex, legend, inBlockComment, tokens);
  }
  std::sort(tokens.begin(), tokens.end(),
            [](const RawSemanticToken &a, const RawSemanticToken &b) {
              if (a.line != b.line)
                return a.line < b.line;
              return a.start < b.start;
            });
  return tokens;
}

Json buildSemanticTokensFull(const std::string &text,
                             const SemanticTokenLegend &legend) {
  auto lines = splitLines(text);
  auto tokens = collectTokensForLineRange(lines, 0,
                                         static_cast<int>(lines.size()) - 1,
                                         legend);
  return encodeTokens(tokens);
}

Json buildSemanticTokensRange(const std::string &text, int startLine,
                              int startCharacter, int endLine, int endCharacter,
                              const SemanticTokenLegend &legend) {
  auto lines = splitLines(text);
  startLine = std::max(0, startLine);
  endLine = std::min(static_cast<int>(lines.size()) - 1, endLine);
  if (endLine < startLine) {
    Json result = makeObject();
    result.o["data"] = makeArray();
    return result;
  }
  auto tokens = collectTokensForLineRange(lines, startLine, endLine, legend);
  std::vector<RawSemanticToken> filtered;
  for (const auto &t : tokens) {
    if (t.line < startLine || t.line > endLine)
      continue;
    if (t.line == startLine && t.start + t.length <= startCharacter)
      continue;
    if (t.line == endLine && t.start >= endCharacter)
      continue;
    filtered.push_back(t);
  }
  return encodeTokens(filtered);
}
