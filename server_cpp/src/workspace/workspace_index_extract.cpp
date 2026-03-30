#include "workspace_index_internal.hpp"


#include "active_unit.hpp"
#include "executable_path.hpp"
#include "include_resolver.hpp"
#include "json.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "nsf_lexer.hpp"
#include "server_parse.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;


bool buildCodeMaskForLine(const std::string &lineText,
                                 bool &inBlockCommentInOut,
                                 std::vector<char> &maskOut) {
  maskOut.assign(lineText.size(), 1);
  bool inString = false;
  bool inLineComment = false;
  for (size_t i = 0; i < lineText.size(); i++) {
    char ch = lineText[i];
    char next = (i + 1 < lineText.size()) ? lineText[i + 1] : '\0';
    if (inLineComment) {
      maskOut[i] = 0;
      continue;
    }
    if (inBlockCommentInOut) {
      maskOut[i] = 0;
      if (ch == '*' && next == '/') {
        if (i + 1 < maskOut.size())
          maskOut[i + 1] = 0;
        inBlockCommentInOut = false;
        i++;
      }
      continue;
    }
    if (inString) {
      maskOut[i] = 0;
      if (ch == '"' && (i == 0 || lineText[i - 1] != '\\'))
        inString = false;
      continue;
    }
    if (ch == '"') {
      maskOut[i] = 0;
      inString = true;
      continue;
    }
    if (ch == '/' && next == '/') {
      maskOut[i] = 0;
      if (i + 1 < maskOut.size())
        maskOut[i + 1] = 0;
      inLineComment = true;
      i++;
      continue;
    }
    if (ch == '/' && next == '*') {
      maskOut[i] = 0;
      if (i + 1 < maskOut.size())
        maskOut[i + 1] = 0;
      inBlockCommentInOut = true;
      i++;
      continue;
    }
  }
  return true;
}

struct LiteralConditionalFrame {
  bool parentActive = true;
  bool currentActive = true;
  bool branchTaken = false;
  bool transparent = false;
};

static bool literalConditionalStackIsActive(
    const std::vector<LiteralConditionalFrame> &stack) {
  if (stack.empty())
    return true;
  return stack.back().currentActive;
}

static bool parseLiteralConditionalValue(const std::vector<LexToken> &tokens,
                                         size_t valueIndex, bool &valueOut) {
  if (valueIndex >= tokens.size())
    return false;
  if (tokens[valueIndex].text == "0") {
    valueOut = false;
    return true;
  }
  if (tokens[valueIndex].text == "1") {
    valueOut = true;
    return true;
  }
  return false;
}

static bool handleLiteralConditionalDirective(
    const std::vector<LexToken> &tokens,
    std::vector<LiteralConditionalFrame> &stack) {
  if (tokens.empty() || tokens.front().kind != LexToken::Kind::Punct ||
      tokens.front().text != "#" || tokens.size() < 2 ||
      tokens[1].kind != LexToken::Kind::Identifier) {
    return false;
  }

  const std::string directive = tokens[1].text;
  const bool parentActive =
      stack.empty() ? true : stack.back().currentActive;

  if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
    LiteralConditionalFrame frame;
    frame.parentActive = parentActive;
    bool literalValue = false;
    if (directive == "if" &&
        parseLiteralConditionalValue(tokens, 2, literalValue)) {
      frame.transparent = false;
      frame.currentActive = parentActive && literalValue;
      frame.branchTaken = literalValue;
    } else {
      frame.transparent = true;
      frame.currentActive = parentActive;
      frame.branchTaken = false;
    }
    stack.push_back(frame);
    return true;
  }

  if (directive == "elif") {
    if (stack.empty())
      return true;
    LiteralConditionalFrame &frame = stack.back();
    if (frame.transparent) {
      frame.currentActive = frame.parentActive;
      return true;
    }
    bool literalValue = false;
    if (!parseLiteralConditionalValue(tokens, 2, literalValue)) {
      frame.transparent = true;
      frame.currentActive = frame.parentActive;
      return true;
    }
    frame.currentActive = frame.parentActive && !frame.branchTaken &&
                          literalValue;
    if (literalValue)
      frame.branchTaken = true;
    return true;
  }

  if (directive == "else") {
    if (stack.empty())
      return true;
    LiteralConditionalFrame &frame = stack.back();
    if (frame.transparent) {
      frame.currentActive = frame.parentActive;
      return true;
    }
    frame.currentActive = frame.parentActive && !frame.branchTaken;
    frame.branchTaken = true;
    return true;
  }

  if (directive == "endif") {
    if (!stack.empty())
      stack.pop_back();
    return true;
  }

  return false;
}

static void
appendStructMembersFromLines(const std::string &uri, const std::string &text,
                             const std::vector<std::string> &workspaceFolders,
                             const std::vector<std::string> &includePaths,
                             const std::vector<std::string> &shaderExtensions,
                             int depth,
                             std::unordered_set<std::string> &visited,
                             std::vector<IndexedStructMember> &membersOut,
                             std::unordered_set<std::string> &seenNames) {
  if (depth <= 0)
    return;
  if (!visited.insert(toLowerCopy(uri)).second)
    return;

  std::istringstream stream(text);
  std::string lineText;
  bool inBlockComment = false;
  std::vector<char> mask;
  std::vector<LiteralConditionalFrame> conditionalStack;
  while (std::getline(stream, lineText)) {
    bool maskBlock = inBlockComment;
    buildCodeMaskForLine(lineText, maskBlock, mask);
    inBlockComment = maskBlock;
    const auto rawTokens = lexLineTokens(lineText);
    std::vector<LexToken> tokens;
    tokens.reserve(rawTokens.size());
    for (const auto &t : rawTokens) {
      if (t.start < mask.size() && mask[t.start])
        tokens.push_back(t);
    }
    if (handleLiteralConditionalDirective(tokens, conditionalStack))
      continue;
    if (!literalConditionalStackIsActive(conditionalStack))
      continue;

    std::string includePath;
    bool includeBlock = false;
    if (extractIncludePathOutsideComments(lineText, includeBlock,
                                          includePath)) {
      auto candidates = resolveIncludeCandidates(
          uri, includePath, workspaceFolders, includePaths, shaderExtensions);
      for (const auto &candidate : candidates) {
        std::string nextText;
        if (!readFileToString(candidate, nextText))
          continue;
        appendStructMembersFromLines(
            pathToUri(candidate), nextText, workspaceFolders, includePaths,
            shaderExtensions, depth - 1, visited, membersOut, seenNames);
        break;
      }
      continue;
    }
    if (tokens.empty())
      continue;
    if (tokens.front().kind == LexToken::Kind::Punct &&
        tokens.front().text == "#")
      continue;

    size_t typeIndex = std::string::npos;
    for (size_t i = 0; i < tokens.size(); i++) {
      if (tokens[i].kind != LexToken::Kind::Identifier)
        continue;
      if (isQualifierToken(tokens[i].text))
        continue;
      typeIndex = i;
      break;
    }
    if (typeIndex == std::string::npos)
      continue;
    const std::string memberType = tokens[typeIndex].text;
    if (memberType.empty())
      continue;

    for (size_t i = typeIndex + 1; i < tokens.size(); i++) {
      if (tokens[i].kind == LexToken::Kind::Punct) {
        if (tokens[i].text == ":" || tokens[i].text == ";" ||
            tokens[i].text == "=")
          break;
        continue;
      }
      if (tokens[i].kind != LexToken::Kind::Identifier)
        continue;
      const std::string memberName = tokens[i].text;
      if (memberName.empty())
        continue;
      if (!seenNames.insert(memberName).second)
        continue;
      IndexedStructMember m;
      m.name = memberName;
      m.type = memberType;
      membersOut.push_back(std::move(m));
    }
  }
}

void
extractStructMembers(const std::string &uri, const std::string &text,
                     const std::vector<std::string> &workspaceFolders,
                     const std::vector<std::string> &includePaths,
                     const std::vector<std::string> &shaderExtensions,
                     std::vector<IndexedStruct> &structsOut) {
  std::istringstream stream(text);
  std::string lineText;
  bool inBlockComment = false;
  bool inStruct = false;
  std::string currentStruct;
  int braceDepth = 0;
  std::vector<IndexedStructMember> membersOrdered;
  std::unordered_set<std::string> seenNames;
  std::vector<char> mask;
  std::vector<LiteralConditionalFrame> conditionalStack;
  while (std::getline(stream, lineText)) {
    bool maskBlock = inBlockComment;
    buildCodeMaskForLine(lineText, maskBlock, mask);
    inBlockComment = maskBlock;
    const auto rawTokens = lexLineTokens(lineText);
    std::vector<LexToken> tokens;
    tokens.reserve(rawTokens.size());
    for (const auto &t : rawTokens) {
      if (t.start < mask.size() && mask[t.start])
        tokens.push_back(t);
    }
    if (handleLiteralConditionalDirective(tokens, conditionalStack))
      continue;
    if (!literalConditionalStackIsActive(conditionalStack))
      continue;
    if (tokens.empty())
      continue;

    if (!inStruct) {
      bool sawStruct = false;
      std::string name;
      for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Identifier &&
            tokens[i].text == "struct" &&
            tokens[i + 1].kind == LexToken::Kind::Identifier) {
          sawStruct = true;
          name = tokens[i + 1].text;
          break;
        }
      }
      if (!sawStruct || name.empty())
        continue;
      inStruct = true;
      currentStruct = name;
      braceDepth = 0;
      membersOrdered.clear();
      seenNames.clear();
      for (const auto &t : tokens) {
        if (t.kind != LexToken::Kind::Punct)
          continue;
        if (t.text == "{")
          braceDepth++;
        else if (t.text == "}")
          braceDepth = braceDepth > 0 ? braceDepth - 1 : 0;
      }
      continue;
    }

    for (const auto &t : tokens) {
      if (t.kind != LexToken::Kind::Punct)
        continue;
      if (t.text == "{")
        braceDepth++;
      else if (t.text == "}")
        braceDepth = braceDepth > 0 ? braceDepth - 1 : 0;
    }

    if (inStruct && braceDepth == 0) {
      IndexedStruct st;
      st.name = currentStruct;
      st.members = std::move(membersOrdered);
      if (!st.name.empty() && !st.members.empty())
        structsOut.push_back(std::move(st));
      inStruct = false;
      currentStruct.clear();
      membersOrdered.clear();
      seenNames.clear();
      continue;
    }

    if (!inStruct || braceDepth != 1)
      continue;

    std::string includePath;
    bool includeBlock = false;
    if (extractIncludePathOutsideComments(lineText, includeBlock,
                                          includePath)) {
      auto candidates = resolveIncludeCandidates(
          uri, includePath, workspaceFolders, includePaths, shaderExtensions);
      for (const auto &candidate : candidates) {
        std::string incText;
        if (!readFileToString(candidate, incText))
          continue;
        std::unordered_set<std::string> visited;
        appendStructMembersFromLines(
            pathToUri(candidate), incText, workspaceFolders, includePaths,
            shaderExtensions, 8, visited, membersOrdered, seenNames);
        break;
      }
      continue;
    }

    size_t typeIndex = std::string::npos;
    for (size_t i = 0; i < tokens.size(); i++) {
      if (tokens[i].kind != LexToken::Kind::Identifier)
        continue;
      if (isQualifierToken(tokens[i].text))
        continue;
      typeIndex = i;
      break;
    }
    if (typeIndex == std::string::npos)
      continue;
    const std::string memberType = tokens[typeIndex].text;
    if (memberType.empty())
      continue;

    for (size_t i = typeIndex + 1; i < tokens.size(); i++) {
      if (tokens[i].kind == LexToken::Kind::Punct) {
        if (tokens[i].text == ":" || tokens[i].text == ";" ||
            tokens[i].text == "=")
          break;
        continue;
      }
      if (tokens[i].kind != LexToken::Kind::Identifier)
        continue;
      const std::string memberName = tokens[i].text;
      if (memberName.empty())
        continue;
      if (!seenNames.insert(memberName).second)
        continue;
      IndexedStructMember m;
      m.name = memberName;
      m.type = memberType;
      membersOrdered.push_back(std::move(m));
    }
  }
}

void extractDefinitions(const std::string &uri, const std::string &text,
                               std::vector<IndexedDefinition> &defsOut) {
  std::istringstream stream(text);
  std::string lineText;
  bool inBlockComment = false;
  std::vector<char> mask;

  bool pendingCbuffer = false;
  bool inCbuffer = false;
  int cbufferBraceDepth = 0;
  int braceDepth = 0;

  bool pendingUiVar = false;
  int pendingUiLine = -1;
  int pendingUiStartByte = -1;
  int pendingUiEndByte = -1;
  std::string pendingUiName;
  std::string pendingUiType;

  int lineIndex = 0;
  std::vector<LiteralConditionalFrame> conditionalStack;

  auto record = [&](const std::string &name, const std::string &type, int kind,
                    int startByte, int endByte, int line) {
    if (name.empty())
      return;
    IndexedDefinition def;
    def.name = name;
    def.type = type;
    def.uri = uri;
    def.line = line;
    def.start = byteOffsetInLineToUtf16(getLineAt(text, line), startByte);
    def.end = byteOffsetInLineToUtf16(getLineAt(text, line), endByte);
    if (def.end < def.start)
      def.end = def.start;
    def.kind = kind;
    defsOut.push_back(std::move(def));
  };

  while (std::getline(stream, lineText)) {
    bool maskBlock = inBlockComment;
    buildCodeMaskForLine(lineText, maskBlock, mask);
    inBlockComment = maskBlock;
    const auto rawTokens = lexLineTokens(lineText);
    std::vector<LexToken> tokens;
    tokens.reserve(rawTokens.size());
    for (const auto &t : rawTokens) {
      if (t.start < mask.size() && mask[t.start])
        tokens.push_back(t);
    }

    std::string trimmed = trimLeftCopy(lineText);
    std::string trimmedRight = trimmed;
    while (!trimmedRight.empty() &&
           std::isspace(static_cast<unsigned char>(trimmedRight.back()))) {
      trimmedRight.pop_back();
    }

    if (handleLiteralConditionalDirective(tokens, conditionalStack)) {
      lineIndex++;
      continue;
    }
    if (!literalConditionalStackIsActive(conditionalStack)) {
      lineIndex++;
      continue;
    }

    if (pendingUiVar) {
      if (trimmedRight == "<") {
        record(pendingUiName, pendingUiType, 8, pendingUiStartByte,
               pendingUiEndByte, pendingUiLine);
        pendingUiVar = false;
      } else if (!trimmedRight.empty() && trimmedRight.rfind("//", 0) != 0) {
        pendingUiVar = false;
      }
    }

    std::string cbufferName;
    if (!inCbuffer && braceDepth == 0 &&
        extractCBufferNameInLineShared(lineText, cbufferName)) {
      pendingCbuffer = true;
    }

    if (braceDepth == 0 && !tokens.empty()) {
      if (trimmed.rfind("#define", 0) == 0) {
        for (size_t i = 0; i + 1 < tokens.size(); i++) {
          if (tokens[i].kind == LexToken::Kind::Identifier &&
              tokens[i].text == "define" &&
              tokens[i + 1].kind == LexToken::Kind::Identifier) {
            record(tokens[i + 1].text, "", 14,
                   static_cast<int>(tokens[i + 1].start),
                   static_cast<int>(tokens[i + 1].end), lineIndex);
            break;
          }
        }
      }

      std::string aggregateName;
      const bool isStructDecl = extractStructNameInLine(lineText, aggregateName);
      const bool isCBufferDecl =
          !isStructDecl && extractCBufferNameInLineShared(lineText, aggregateName);
      if (isStructDecl || isCBufferDecl || trimmed.rfind("technique", 0) == 0 ||
          trimmed.rfind("pass", 0) == 0) {
        if (isStructDecl || isCBufferDecl) {
          for (size_t i = 0; i < tokens.size(); i++) {
            if (tokens[i].kind == LexToken::Kind::Identifier &&
                tokens[i].text == aggregateName) {
              record(tokens[i].text, "", 23, static_cast<int>(tokens[i].start),
                     static_cast<int>(tokens[i].end), lineIndex);
              break;
            }
          }
        } else {
          for (size_t i = 0; i + 1 < tokens.size(); i++) {
            if (tokens[i].kind == LexToken::Kind::Identifier &&
                (tokens[i].text == "technique" || tokens[i].text == "pass") &&
                tokens[i + 1].kind == LexToken::Kind::Identifier) {
              record(tokens[i + 1].text, "", 23,
                     static_cast<int>(tokens[i + 1].start),
                     static_cast<int>(tokens[i + 1].end), lineIndex);
              break;
            }
          }
        }
      }

      std::string fxBlockType;
      std::string fxBlockName;
      if (extractFxBlockDeclarationHeaderShared(lineText, fxBlockType,
                                                fxBlockName)) {
        for (size_t i = 0; i < tokens.size(); i++) {
          if (tokens[i].kind == LexToken::Kind::Identifier &&
              tokens[i].text == fxBlockName) {
            record(tokens[i].text, fxBlockType, 8,
                   static_cast<int>(tokens[i].start),
                   static_cast<int>(tokens[i].end), lineIndex);
            break;
          }
        }
      }

      if (tokens.size() >= 3 && tokens[0].text != "#") {
        size_t typeIndex = std::string::npos;
        for (size_t i = 0; i < tokens.size(); i++) {
          if (tokens[i].kind != LexToken::Kind::Identifier)
            continue;
          if (isQualifierToken(tokens[i].text))
            continue;
          const std::string &t = tokens[i].text;
          if (t == "return" || t == "if" || t == "for" || t == "while" ||
              t == "switch" || t == "struct" || t == "cbuffer")
            break;
          typeIndex = i;
          break;
        }
        if (typeIndex != std::string::npos) {
          size_t nameIndex = std::string::npos;
          for (size_t i = typeIndex + 1; i < tokens.size(); i++) {
            if (tokens[i].kind != LexToken::Kind::Identifier)
              continue;
            if (isQualifierToken(tokens[i].text))
              continue;
            nameIndex = i;
            break;
          }
          if (nameIndex != std::string::npos) {
            const std::string name = tokens[nameIndex].text;
            const std::string type = tokens[typeIndex].text;
            std::string uiType;
            std::string uiName;
            size_t uiStart = 0;
            size_t uiEnd = 0;
            if (findMetadataDeclarationHeaderPosShared(lineText, uiType,
                                                       uiName, uiStart,
                                                       uiEnd) &&
                uiName == name) {
              pendingUiVar = true;
              pendingUiLine = lineIndex;
              pendingUiStartByte = static_cast<int>(uiStart);
              pendingUiEndByte = static_cast<int>(uiEnd);
              pendingUiName = uiName;
              pendingUiType = uiType;
            } else if (nameIndex + 1 < tokens.size() &&
                       tokens[nameIndex + 1].kind == LexToken::Kind::Punct &&
                       tokens[nameIndex + 1].text == "(") {
              int parenDepth = 0;
              size_t closeIndex = std::string::npos;
              for (size_t i = nameIndex + 1; i < tokens.size(); i++) {
                if (tokens[i].kind != LexToken::Kind::Punct)
                  continue;
                if (tokens[i].text == "(")
                  parenDepth++;
                else if (tokens[i].text == ")") {
                  parenDepth--;
                  if (parenDepth == 0) {
                    closeIndex = i;
                    break;
                  }
                }
              }
              if (closeIndex != std::string::npos) {
                bool recorded = false;
                for (size_t k = closeIndex + 1; k < tokens.size(); k++) {
                  if (tokens[k].kind != LexToken::Kind::Punct)
                    continue;
                  if (tokens[k].text == ":" || tokens[k].text == "{" ||
                      tokens[k].text == ";") {
                    record(name, type, 12,
                           static_cast<int>(tokens[nameIndex].start),
                           static_cast<int>(tokens[nameIndex].end), lineIndex);
                    recorded = true;
                    break;
                  }
                }
                if (!recorded) {
                  record(name, type, 12,
                         static_cast<int>(tokens[nameIndex].start),
                         static_cast<int>(tokens[nameIndex].end), lineIndex);
                }
              }
            } else {
              bool hasSemi = false;
              bool hasAssignBefore = false;
              for (size_t j = 0; j < tokens.size(); j++) {
                if (tokens[j].kind == LexToken::Kind::Punct &&
                    tokens[j].text == ";") {
                  hasSemi = true;
                  break;
                }
                if (tokens[j].kind == LexToken::Kind::Punct &&
                    tokens[j].text == "=") {
                  hasAssignBefore = true;
                  break;
                }
              }
              if (hasSemi && !hasAssignBefore) {
                record(name, type, 8, static_cast<int>(tokens[nameIndex].start),
                       static_cast<int>(tokens[nameIndex].end), lineIndex);
              }
            }
          }
        }
      }
    }

    if (inCbuffer && cbufferBraceDepth == 1 && !tokens.empty()) {
      size_t typeIndex = std::string::npos;
      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        if (isQualifierToken(tokens[i].text))
          continue;
        typeIndex = i;
        break;
      }
      if (typeIndex != std::string::npos && typeIndex + 1 < tokens.size() &&
          tokens[typeIndex + 1].kind == LexToken::Kind::Identifier) {
        bool hasSemi = false;
        for (const auto &t : tokens) {
          if (t.kind == LexToken::Kind::Punct && t.text == ";") {
            hasSemi = true;
            break;
          }
        }
        if (hasSemi) {
          record(tokens[typeIndex + 1].text, tokens[typeIndex].text, 13,
                 static_cast<int>(tokens[typeIndex + 1].start),
                 static_cast<int>(tokens[typeIndex + 1].end), lineIndex);
        }
      }
    }

    bool inLineComment = false;
    for (size_t c = 0; c < lineText.size(); c++) {
      if (!inLineComment && c + 1 < lineText.size() && lineText[c] == '/' &&
          lineText[c + 1] == '/') {
        inLineComment = true;
      }
      if (inLineComment)
        break;
      if (lineText[c] == '{') {
        braceDepth++;
        if (pendingCbuffer) {
          inCbuffer = true;
          cbufferBraceDepth = 1;
          pendingCbuffer = false;
        } else if (inCbuffer) {
          cbufferBraceDepth++;
        }
      } else if (lineText[c] == '}') {
        braceDepth = braceDepth > 0 ? braceDepth - 1 : 0;
        if (inCbuffer) {
          cbufferBraceDepth = cbufferBraceDepth > 0 ? cbufferBraceDepth - 1 : 0;
          if (cbufferBraceDepth == 0)
            inCbuffer = false;
        }
      }
    }

    lineIndex++;
  }
}
