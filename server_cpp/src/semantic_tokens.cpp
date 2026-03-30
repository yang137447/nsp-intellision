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

struct SemanticTokenTypeIndices {
  int keywordType = 0;
  int numberType = 0;
  int macroType = 0;
  int functionType = 0;
  int variableType = 0;
  int typeType = 0;
  int propertyType = 0;
  int operatorType = 0;
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
  legend.tokenTypes = {"keyword",  "number",  "macro",    "function",
                       "variable", "type",    "property", "operator"};
  legend.tokenModifiers = {};
  return legend;
}

static SemanticTokenTypeIndices
resolveTokenTypeIndices(const SemanticTokenLegend &legend) {
  SemanticTokenTypeIndices indices;
  indices.keywordType = findTokenTypeIndex(legend, "keyword");
  indices.numberType = findTokenTypeIndex(legend, "number");
  indices.macroType = findTokenTypeIndex(legend, "macro");
  indices.functionType = findTokenTypeIndex(legend, "function");
  indices.variableType = findTokenTypeIndex(legend, "variable");
  indices.typeType = findTokenTypeIndex(legend, "type");
  indices.propertyType = findTokenTypeIndex(legend, "property");
  indices.operatorType = findTokenTypeIndex(legend, "operator");
  return indices;
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
                         const SemanticTokenTypeIndices &tokenTypes,
                         bool &inBlockComment, std::vector<RawSemanticToken> &out) {
  static const std::unordered_set<std::string> typeKeywords = {
      "void",    "bool",    "int",     "uint",   "float",   "float2",
      "float3",  "float4",  "float2x2","float3x3","float4x4",
      "half",    "half2",   "half3",   "half4",  "Texture2D",
      "Texture3D","SamplerState", "SamplerComparisonState"};
  static const std::string kOperatorChars = "+-*/%=&|^!~?:<>";

  bool expectTypeIdentifier = false;
  int attributeDepth = 0;
  const std::string trimmedLine = trimLeft(line);
  const bool isDirectiveLine = !trimmedLine.empty() && trimmedLine[0] == '#';
  const size_t directiveHashPos =
      isDirectiveLine ? line.find('#') : std::string::npos;

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
      if (end == std::string::npos) {
        return;
      }
      inBlockComment = false;
      i = end + 2;
      continue;
    }

    char ch = line[i];
    char next = (i + 1 < line.size()) ? line[i + 1] : '\0';
    if (std::isspace(static_cast<unsigned char>(ch))) {
      i++;
      continue;
    }

    if (i == 0 && isDirectiveLine) {
      if (directiveHashPos != std::string::npos) {
        pushToken(directiveHashPos, directiveHashPos + 1,
                  tokenTypes.macroType, 0);
        size_t wordStart = scanToNonSpace(line, directiveHashPos + 1);
        size_t wordEnd = wordStart;
        while (wordEnd < line.size() && isIdentifierChar(line[wordEnd]))
          wordEnd++;
        if (wordEnd > wordStart) {
          pushToken(wordStart, wordEnd, tokenTypes.macroType, 0);
        }
      }
      return;
    }

    if (ch == '/' && next == '/') {
      return;
    }
    if (ch == '/' && next == '*') {
      size_t end = line.find("*/", i + 2);
      if (end == std::string::npos) {
        inBlockComment = true;
        return;
      }
      i = end + 2;
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
      i = end;
      continue;
    }
    if (isNumberStart(line, i)) {
      size_t end = scanNumberToken(line, i);
      pushToken(i, end, tokenTypes.numberType, 0);
      i = end;
      continue;
    }
    if (isIdentifierChar(ch)) {
      size_t end = scanIdentifierToken(line, i);
      std::string word = line.substr(i, end - i);
      int tokType = tokenTypes.variableType;
      if (attributeDepth > 0) {
        tokType = tokenTypes.keywordType;
      } else if (isHlslKeyword(word) || isHlslSystemSemantic(word)) {
        tokType = tokenTypes.keywordType;
        if (word == "struct" || word == "cbuffer")
          expectTypeIdentifier = true;
      } else if (typeKeywords.find(word) != typeKeywords.end() ||
                 expectTypeIdentifier) {
        tokType = tokenTypes.typeType;
        expectTypeIdentifier = false;
      } else {
        size_t after = scanToNonSpace(line, end);
        if (after < line.size() && line[after] == '(') {
          tokType = tokenTypes.functionType;
        } else if (i > 0 && line[i - 1] == '.') {
          tokType = tokenTypes.propertyType;
        }
      }
      pushToken(i, end, tokType, 0);
      i = end;
      continue;
    }

    if (kOperatorChars.find(ch) != std::string::npos) {
      pushToken(i, i + 1, tokenTypes.operatorType, 0);
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
  const SemanticTokenTypeIndices tokenTypes = resolveTokenTypeIndices(legend);
  for (int lineIndex = 0; lineIndex < static_cast<int>(lines.size()); lineIndex++) {
    if (lineIndex < startLine)
      continue;
    if (lineIndex > endLine)
      break;
    tokenizeLine(lines[lineIndex], lineIndex, tokenTypes, inBlockComment,
                 tokens);
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
