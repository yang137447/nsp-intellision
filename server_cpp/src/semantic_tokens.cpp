#include "semantic_tokens.hpp"

#include "language_registry.hpp"
#include "lsp_helpers.hpp"
#include "nsf_lexer.hpp"
#include "semantic_snapshot.hpp"
#include "text_utils.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
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
  int parameterType = 0;
};

struct SemanticTokenModifierMasks {
  int declaration = 0;
  int modification = 0;
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

static int findTokenModifierIndex(const SemanticTokenLegend &legend,
                                  const std::string &tokenModifier) {
  for (size_t i = 0; i < legend.tokenModifiers.size(); i++) {
    if (legend.tokenModifiers[i] == tokenModifier)
      return static_cast<int>(i);
  }
  return -1;
}

SemanticTokenLegend createDefaultSemanticTokenLegend() {
  SemanticTokenLegend legend;
  legend.tokenTypes = {"keyword",  "number",  "macro",    "function",
                       "variable", "type",    "property", "operator",
                       "parameter"};
  legend.tokenModifiers = {"declaration", "modification"};
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
  indices.parameterType = findTokenTypeIndex(legend, "parameter");
  return indices;
}

static SemanticTokenModifierMasks
resolveTokenModifierMasks(const SemanticTokenLegend &legend) {
  SemanticTokenModifierMasks masks;
  const int declarationIndex = findTokenModifierIndex(legend, "declaration");
  const int modificationIndex = findTokenModifierIndex(legend, "modification");
  if (declarationIndex >= 0 && declarationIndex < 30)
    masks.declaration = 1 << declarationIndex;
  if (modificationIndex >= 0 && modificationIndex < 30)
    masks.modification = 1 << modificationIndex;
  return masks;
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

static bool factLocationMatchesToken(int factLine, int factCharacter,
                                     int tokenLine, int tokenCharacter) {
  return factLine >= 0 && factCharacter >= 0 && factLine == tokenLine &&
         factCharacter == tokenCharacter;
}

struct SemanticTokenClassificationContext {
  const SemanticSnapshot *snapshot = nullptr;

  const SemanticSnapshot::FunctionInfo *findContainingFunction(int line) const {
    if (!snapshot)
      return nullptr;
    const SemanticSnapshot::FunctionInfo *best = nullptr;
    int bestSpan = 0;
    for (const auto &function : snapshot->functions) {
      const int visibleEndLine =
          function.hasBody && function.bodyEndLine >= 0
              ? function.bodyEndLine
              : (function.signatureEndLine >= 0 ? function.signatureEndLine
                                                : function.line);
      if (line < function.line || line > visibleEndLine)
        continue;
      const int span = visibleEndLine - function.line;
      if (!best || span < bestSpan) {
        best = &function;
        bestSpan = span;
      }
    }
    return best;
  }

  bool isParameter(const SemanticSnapshot::FunctionInfo *function,
                   const std::string &name) const {
    if (!function || name.empty())
      return false;
    for (const auto &parameter : function->parameterInfos) {
      if (parameter.first == name)
        return true;
    }
    return false;
  }

  bool isParameterDeclaration(const SemanticSnapshot::FunctionInfo *function,
                              const std::string &name, int line,
                              int character) const {
    if (!function || name.empty())
      return false;
    for (const auto &parameter : function->parameterDetails) {
      if (parameter.name == name &&
          factLocationMatchesToken(parameter.line, parameter.character, line,
                                   character)) {
        return true;
      }
    }
    return false;
  }

  const SemanticSnapshot::FunctionInfo::LocalInfo *
  findVisibleLocal(const SemanticSnapshot::FunctionInfo *function,
                   const std::string &name, size_t offset) const {
    if (!function || name.empty())
      return nullptr;
    const SemanticSnapshot::FunctionInfo::LocalInfo *best = nullptr;
    for (const auto &local : function->locals) {
      if (local.name != name || local.offset > offset)
        continue;
      if (local.scopeStartOffset > offset ||
          (local.scopeEndOffset > 0 && offset >= local.scopeEndOffset)) {
        continue;
      }
      if (!best || local.depth > best->depth ||
          (local.depth == best->depth && local.offset >= best->offset)) {
        best = &local;
      }
    }
    return best;
  }

  bool isLocalDeclaration(
      const SemanticSnapshot::FunctionInfo::LocalInfo *local,
      int line, int character) const {
    return local && factLocationMatchesToken(local->line, local->character, line,
                                             character);
  }

  const SemanticSnapshot::GlobalInfo *
  findGlobal(const std::string &name) const {
    if (!snapshot || name.empty())
      return nullptr;
    auto it = snapshot->globalByName.find(name);
    if (it == snapshot->globalByName.end() || it->second >= snapshot->globals.size())
      return nullptr;
    return &snapshot->globals[it->second];
  }

  bool isGlobalDeclaration(const SemanticSnapshot::GlobalInfo *global, int line,
                           int character) const {
    return global && factLocationMatchesToken(global->line, global->character,
                                              line, character);
  }

  const SemanticSnapshot::FieldInfo *
  findStructFieldDeclaration(const std::string &name, int line,
                             int character) const {
    if (!snapshot || name.empty())
      return nullptr;
    for (const auto &structure : snapshot->structs) {
      for (const auto &field : structure.fields) {
        if (field.name == name &&
            factLocationMatchesToken(field.line, field.character, line,
                                     character)) {
          return &field;
        }
      }
    }
    return nullptr;
  }

  const SemanticSnapshot::FieldInfo *
  findCBufferFieldDeclaration(const std::string &name, int line,
                              int character) const {
    if (!snapshot || name.empty())
      return nullptr;
    for (const auto &cbuffer : snapshot->cbuffers) {
      for (const auto &field : cbuffer.fields) {
        if (field.name == name &&
            factLocationMatchesToken(field.line, field.character, line,
                                     character)) {
          return &field;
        }
      }
    }
    return nullptr;
  }

  bool isCBufferFieldName(const std::string &name) const {
    if (!snapshot || name.empty())
      return false;
    for (const auto &cbuffer : snapshot->cbuffers) {
      for (const auto &field : cbuffer.fields) {
        if (field.name == name)
          return true;
      }
    }
    return false;
  }
};

static size_t previousNonSpaceBefore(const std::string &line, size_t index) {
  size_t i = std::min(index, line.size());
  while (i > 0) {
    i--;
    if (!std::isspace(static_cast<unsigned char>(line[i])))
      return i;
  }
  return std::string::npos;
}

static bool startsWithAt(const std::string &line, size_t index,
                         const std::string &text) {
  return index <= line.size() && line.compare(index, text.size(), text) == 0;
}

static bool isAssignmentOperatorAt(const std::string &line, size_t index) {
  if (index >= line.size())
    return false;
  const char ch = line[index];
  if (ch == '=') {
    const char next = index + 1 < line.size() ? line[index + 1] : '\0';
    const char prev = index > 0 ? line[index - 1] : '\0';
    return next != '=' && prev != '=' && prev != '!' && prev != '<' &&
           prev != '>';
  }
  if (std::string("+-*/%").find(ch) != std::string::npos) {
    return index + 1 < line.size() && line[index + 1] == '=';
  }
  return false;
}

static bool isModifiedTokenSpan(const std::string &line, size_t startByte,
                                size_t endByte) {
  size_t after = scanToNonSpace(line, endByte);
  if (startsWithAt(line, after, "++") || startsWithAt(line, after, "--"))
    return true;
  if (isAssignmentOperatorAt(line, after))
    return true;

  const size_t prev = previousNonSpaceBefore(line, startByte);
  if (prev != std::string::npos && prev > 0) {
    const size_t beforePrev = previousNonSpaceBefore(line, prev);
    if (beforePrev != std::string::npos) {
      const std::string op =
          std::string(1, line[beforePrev]) + std::string(1, line[prev]);
      if (op == "++" || op == "--")
        return true;
    }
  }
  return false;
}

static void applySemanticClassification(
    const SemanticTokenClassificationContext &context, const std::string &line,
    const std::string &word, int lineIndex, int startCharacter,
    size_t startByte, size_t endByte, size_t absoluteOffset,
    const SemanticTokenTypeIndices &tokenTypes,
    const SemanticTokenModifierMasks &tokenModifiers, int &tokType,
    int &tokModifiers) {
  if (!context.snapshot || word.empty())
    return;

  bool isDeclaration = false;
  const bool propertyByAccess = tokType == tokenTypes.propertyType;

  if (context.findStructFieldDeclaration(word, lineIndex, startCharacter) ||
      context.findCBufferFieldDeclaration(word, lineIndex, startCharacter)) {
    tokType = tokenTypes.propertyType;
    isDeclaration = true;
  } else {
    const auto *function = context.findContainingFunction(lineIndex);
    const auto *local =
        context.findVisibleLocal(function, word, absoluteOffset);
    if (local) {
      tokType = tokenTypes.variableType;
      isDeclaration = context.isLocalDeclaration(local, lineIndex,
                                                 startCharacter);
    } else if (context.isParameter(function, word)) {
      tokType = tokenTypes.parameterType;
      isDeclaration =
          context.isParameterDeclaration(function, word, lineIndex,
                                         startCharacter);
    } else if (propertyByAccess) {
      tokType = tokenTypes.propertyType;
    } else if (context.isCBufferFieldName(word)) {
      tokType = tokenTypes.propertyType;
    } else if (const auto *global = context.findGlobal(word)) {
      tokType = tokenTypes.variableType;
      isDeclaration =
          context.isGlobalDeclaration(global, lineIndex, startCharacter);
    }
  }

  if (isDeclaration) {
    tokModifiers |= tokenModifiers.declaration;
    return;
  }

  if ((tokType == tokenTypes.variableType || tokType == tokenTypes.parameterType ||
       tokType == tokenTypes.propertyType) &&
      isModifiedTokenSpan(line, startByte, endByte)) {
    tokModifiers |= tokenModifiers.modification;
  }
}

static void tokenizeLine(const std::string &line, int lineIndex,
                         const SemanticTokenTypeIndices &tokenTypes,
                         const SemanticTokenModifierMasks &tokenModifiers,
                         const SemanticTokenClassificationContext &context,
                         size_t lineStartOffset, bool &inBlockComment,
                         std::vector<RawSemanticToken> &out) {
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
      int tokModifiers = 0;
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
      if (tokType == tokenTypes.variableType ||
          tokType == tokenTypes.propertyType) {
        const int startCharacter =
            byteOffsetInLineToUtf16(line, static_cast<int>(i));
        applySemanticClassification(
            context, line, word, lineIndex, startCharacter, i, end,
            lineStartOffset + i, tokenTypes, tokenModifiers, tokType,
            tokModifiers);
      }
      pushToken(i, end, tokType, tokModifiers);
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
                          const SemanticTokenLegend &legend,
                          const SemanticSnapshot *snapshot) {
  std::vector<RawSemanticToken> tokens;
  bool inBlockComment = false;
  const SemanticTokenTypeIndices tokenTypes = resolveTokenTypeIndices(legend);
  const SemanticTokenModifierMasks tokenModifiers =
      resolveTokenModifierMasks(legend);
  const SemanticTokenClassificationContext context{snapshot};
  size_t lineStartOffset = 0;
  for (int lineIndex = 0; lineIndex < static_cast<int>(lines.size()); lineIndex++) {
    const size_t currentLineStartOffset = lineStartOffset;
    lineStartOffset += lines[lineIndex].size() + 1;
    if (lineIndex < startLine)
      continue;
    if (lineIndex > endLine)
      break;
    tokenizeLine(lines[lineIndex], lineIndex, tokenTypes, tokenModifiers,
                 context, currentLineStartOffset, inBlockComment, tokens);
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
                             const SemanticTokenLegend &legend,
                             const SemanticSnapshot *snapshot) {
  auto lines = splitLines(text);
  auto tokens = collectTokensForLineRange(lines, 0,
                                         static_cast<int>(lines.size()) - 1,
                                         legend, snapshot);
  return encodeTokens(tokens);
}

Json buildSemanticTokensRange(const std::string &text, int startLine,
                              int startCharacter, int endLine, int endCharacter,
                              const SemanticTokenLegend &legend,
                              const SemanticSnapshot *snapshot) {
  auto lines = splitLines(text);
  startLine = std::max(0, startLine);
  endLine = std::min(static_cast<int>(lines.size()) - 1, endLine);
  if (endLine < startLine) {
    Json result = makeObject();
    result.o["data"] = makeArray();
    return result;
  }
  auto tokens =
      collectTokensForLineRange(lines, startLine, endLine, legend, snapshot);
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
