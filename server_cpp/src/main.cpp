#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "active_unit.hpp"
#include "crash_handler.hpp"
#include "definition_fallback.hpp"
#include "definition_location.hpp"
#include "diagnostics.hpp"
#include "expanded_source.hpp"
#include "fast_ast.hpp"
#include "full_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_docs.hpp"
#include "include_resolver.hpp"
#include "indeterminate_reasons.hpp"
#include "json.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "nsf_lexer.hpp"
#include "preprocessor_view.hpp"
#include "semantic_snapshot.hpp"
#include "semantic_cache.hpp"
#include "server_documents.hpp"
#include "server_occurrences.hpp"
#include "server_parse.hpp"
#include "server_request_handlers.hpp"
#include "server_settings.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"

static bool isWordBoundary(const std::string &text, size_t start, size_t end) {
  bool leftOk = start == 0 || !isIdentifierChar(text[start - 1]);
  bool rightOk = end >= text.size() || !isIdentifierChar(text[end]);
  return leftOk && rightOk;
}

static bool isControlOrOperatorToken(const std::string &token) {
  static const std::unordered_set<std::string> blocked = {
      "return", "if",       "for", "while", "switch", "case", "else", "do",
      "break",  "continue", "=",   "+",     "-",      "*",    "/",    "%",
      "&",      "|",        "^",   "&&",    "||",     "!",    "~",    "?",
      "(",      "[",        "{",   ",",     ".",      "->"};
  return blocked.find(token) != blocked.end();
}

bool findMacroDefinitionInLine(const std::string &line, const std::string &word,
                               size_t &posOut);

std::vector<std::string> getIncludeGraphUrisCached(
    const std::string &rootUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions);

size_t positionToOffset(const std::string &text, int line, int character) {
  return positionToOffsetUtf16(text, line, character);
}

std::vector<std::string> getIncludeGraphUrisCached(
    const std::string &rootUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions);

bool collectStructFieldsInIncludeGraph(
    const std::string &uri, const std::string &structName,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    std::unordered_set<std::string> &visited,
    std::vector<std::string> &fieldsOut) {
  const auto orderedUris = getIncludeGraphUrisCached(
      uri, documents, workspaceFolders, includePaths, shaderExtensions);
  prefetchDocumentTexts(orderedUris, documents);
  for (const auto &candidateUri : orderedUris) {
    if (!visited.insert(candidateUri).second)
      continue;
    std::string text;
    if (!loadDocumentText(candidateUri, documents, text))
      continue;
    uint64_t candidateEpoch = 0;
    auto candidateIt = documents.find(candidateUri);
    if (candidateIt != documents.end())
      candidateEpoch = candidateIt->second.epoch;
    std::vector<SemanticSnapshotStructFieldInfo> fieldInfos;
    if (querySemanticSnapshotStructFieldInfos(
            candidateUri, text, candidateEpoch, workspaceFolders, includePaths,
            shaderExtensions, defines, structName, fieldInfos) &&
        !fieldInfos.empty()) {
      fieldsOut.clear();
      fieldsOut.reserve(fieldInfos.size());
      for (const auto &field : fieldInfos)
        fieldsOut.push_back(field.name);
      return true;
    }
  }
  return false;
}

bool collectStructFieldsInIncludeGraph(
    const std::string &uri, const std::string &structName,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited,
    std::vector<std::string> &fieldsOut) {
  static const std::unordered_map<std::string, int> emptyDefines;
  return collectStructFieldsInIncludeGraph(uri, structName, documents,
                                           workspaceFolders, includePaths,
                                           shaderExtensions, emptyDefines,
                                           visited, fieldsOut);
}

bool findMacroDefinitionInLine(const std::string &line, const std::string &word,
                               size_t &posOut) {
  std::string code = line;
  size_t lineComment = code.find("//");
  if (lineComment != std::string::npos)
    code = code.substr(0, lineComment);
  auto tokens = lexLineTokens(code);
  if (tokens.size() >= 3 && tokens[0].text == "#" &&
      tokens[1].kind == LexToken::Kind::Identifier &&
      tokens[1].text == "define" &&
      tokens[2].kind == LexToken::Kind::Identifier && tokens[2].text == word) {
    posOut = tokens[2].start;
    return true;
  }
  return false;
}

bool findDeclaredIdentifierInDeclarationLine(const std::string &line,
                                             const std::string &word,
                                             size_t &posOut) {
  std::string code = line;
  size_t lineComment = code.find("//");
  if (lineComment != std::string::npos)
    code = code.substr(0, lineComment);

  auto tokens = lexLineTokens(code);
  if (tokens.empty())
    return false;

  if (tokens[0].text == "#") {
    return findMacroDefinitionInLine(code, word, posOut);
  }

  auto isBlockedStart = [&](const std::string &t) {
    return t == "return" || t == "if" || t == "for" || t == "while" ||
           t == "switch" || t == "case" || t == "else" || t == "do";
  };
  if (tokens[0].kind == LexToken::Kind::Identifier &&
      isBlockedStart(tokens[0].text))
    return false;

  struct Segment {
    size_t start = 0;
    size_t end = 0;
  };
  std::vector<Segment> segments;
  int angleDepth = 0;
  int parenDepth = 0;
  int bracketDepth = 0;
  size_t segStart = 0;
  for (size_t idx = 0; idx < tokens.size(); idx++) {
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

    if (angleDepth == 0 && parenDepth == 0 && bracketDepth == 0) {
      if (t == ";") {
        segments.push_back(Segment{segStart, idx});
        segStart = idx + 1;
        continue;
      }
      if (t == ",") {
        segments.push_back(Segment{segStart, idx});
        segStart = idx + 1;
      }
    }
  }
  if (segments.empty())
    return false;

  auto findDeclaratorName = [&](const Segment &seg,
                                bool requireTypePrefix) -> const LexToken * {
    int a = 0;
    int p = 0;
    int b = 0;
    const LexToken *candidate = nullptr;
    bool sawParenAtTop = false;
    bool sawEqualAtTop = false;
    for (size_t idx = seg.start; idx < seg.end; idx++) {
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
      for (size_t idx = seg.start; idx < seg.end; idx++) {
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
        if (tok.kind == LexToken::Kind::Identifier &&
            !isQualifierToken(tok.text))
          hasIdentifierBefore = true;
      }
      if (!hasIdentifierBefore)
        return nullptr;
    }
    if (sawParenAtTop)
      return nullptr;

    size_t candidateIndex = static_cast<size_t>(candidate - &tokens[0]);
    if (candidateIndex > seg.start) {
      const std::string &prev = tokens[candidateIndex - 1].text;
      if (prev == "." || prev == "->")
        return nullptr;
    }
    return candidate;
  };

  for (size_t segIndex = 0; segIndex < segments.size(); segIndex++) {
    const Segment &seg = segments[segIndex];
    const LexToken *nameTok = findDeclaratorName(seg, segIndex == 0);
    if (!nameTok)
      continue;
    if (nameTok->text == word) {
      posOut = nameTok->start;
      return true;
    }
  }
  return false;
}

static bool findParameterDeclarationInText(const std::string &text,
                                           const std::string &word,
                                           int &defLine, int &defStart,
                                           int &defEnd) {
  std::vector<size_t> lineStarts;
  lineStarts.push_back(0);
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '\n')
      lineStarts.push_back(i + 1);
  }
  auto indexToLineCol = [&](size_t index, int &line, int &col) {
    auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), index);
    size_t lineIndex = it == lineStarts.begin()
                           ? 0
                           : static_cast<size_t>((it - lineStarts.begin()) - 1);
    line = static_cast<int>(lineIndex);
    size_t lineStart = lineStarts[lineIndex];
    size_t lineEnd = text.size();
    if (lineIndex + 1 < lineStarts.size()) {
      lineEnd = lineStarts[lineIndex + 1];
      if (lineEnd > 0)
        lineEnd -= 1;
    }
    if (lineEnd < lineStart)
      lineEnd = lineStart;
    std::string lineText = text.substr(lineStart, lineEnd - lineStart);
    col =
        byteOffsetInLineToUtf16(lineText, static_cast<int>(index - lineStart));
  };

  bool inLineComment = false;
  bool inBlockComment = false;
  bool inString = false;

  auto isFuncHeaderKeyword = [&](const std::string &t) {
    return t == "if" || t == "for" || t == "while" || t == "switch" ||
           t == "return";
  };

  for (size_t i = 0; i < text.size(); i++) {
    char ch = text[i];
    char next = i + 1 < text.size() ? text[i + 1] : '\0';

    if (inLineComment) {
      if (ch == '\n')
        inLineComment = false;
      continue;
    }
    if (inBlockComment) {
      if (ch == '*' && next == '/') {
        inBlockComment = false;
        i++;
      }
      continue;
    }
    if (inString) {
      if (ch == '"' && (i == 0 || text[i - 1] != '\\'))
        inString = false;
      continue;
    }
    if (ch == '/' && next == '/') {
      inLineComment = true;
      i++;
      continue;
    }
    if (ch == '/' && next == '*') {
      inBlockComment = true;
      i++;
      continue;
    }
    if (ch == '"') {
      inString = true;
      continue;
    }

    if (ch != '(')
      continue;

    size_t back = i;
    while (back > 0 && std::isspace(static_cast<unsigned char>(text[back - 1])))
      back--;
    if (back == 0)
      continue;
    size_t nameEnd = back;
    size_t nameStart = nameEnd;
    while (nameStart > 0 && isIdentifierChar(text[nameStart - 1]))
      nameStart--;
    if (nameStart == nameEnd)
      continue;
    std::string funcName = text.substr(nameStart, nameEnd - nameStart);
    if (isFuncHeaderKeyword(funcName))
      continue;

    size_t back2 = nameStart;
    while (back2 > 0 &&
           std::isspace(static_cast<unsigned char>(text[back2 - 1])))
      back2--;
    size_t typeEnd = back2;
    size_t typeStart = typeEnd;
    while (typeStart > 0 && isIdentifierChar(text[typeStart - 1]))
      typeStart--;
    if (typeStart == typeEnd)
      continue;

    size_t j = i + 1;
    int parenDepth = 0;
    int angleDepth = 0;
    int bracketDepth = 0;
    std::vector<LexToken> current;
    bool localLineComment = false;
    bool localBlockComment = false;
    bool localString = false;
    auto flushParam = [&]() -> bool {
      if (current.empty())
        return false;
      const LexToken *nameTok = nullptr;
      int a = 0, b = 0, p = 0;
      int identifierCount = 0;
      for (size_t ti = 0; ti < current.size(); ti++) {
        const auto &tok = current[ti];
        const std::string &t = tok.text;
        if (t == "<")
          a++;
        else if (t == ">" && a > 0)
          a--;
        else if (t == "[")
          b++;
        else if (t == "]" && b > 0)
          b--;
        else if (t == "(")
          p++;
        else if (t == ")" && p > 0)
          p--;
        bool top = a == 0 && b == 0 && p == 0;
        if (!top)
          continue;
        if (t == ":" || t == "=")
          break;
        if (tok.kind != LexToken::Kind::Identifier)
          continue;
        if (isQualifierToken(t))
          continue;
        identifierCount++;
        nameTok = &tok;
      }
      if (!nameTok || identifierCount < 2)
        return false;
      if (nameTok->text != word)
        return false;
      int outLine = 0;
      int outCol = 0;
      indexToLineCol(nameTok->start, outLine, outCol);
      defLine = outLine;
      defStart = outCol;
      defEnd = outCol + static_cast<int>(word.size());
      return true;
    };

    for (; j < text.size(); j++) {
      char cj = text[j];
      char nj = j + 1 < text.size() ? text[j + 1] : '\0';

      if (localLineComment) {
        if (cj == '\n')
          localLineComment = false;
        continue;
      }
      if (localBlockComment) {
        if (cj == '*' && nj == '/') {
          localBlockComment = false;
          j++;
        }
        continue;
      }
      if (localString) {
        if (cj == '"' && text[j - 1] != '\\')
          localString = false;
        continue;
      }
      if (cj == '/' && nj == '/') {
        localLineComment = true;
        j++;
        continue;
      }
      if (cj == '/' && nj == '*') {
        localBlockComment = true;
        j++;
        continue;
      }
      if (cj == '"') {
        localString = true;
        continue;
      }

      if (cj == '(') {
        parenDepth++;
      } else if (cj == ')' && parenDepth > 0) {
        parenDepth--;
      } else if (cj == '<') {
        angleDepth++;
      } else if (cj == '>' && angleDepth > 0) {
        angleDepth--;
      } else if (cj == '[') {
        bracketDepth++;
      } else if (cj == ']' && bracketDepth > 0) {
        bracketDepth--;
      }

      if (cj == ')' && parenDepth == 0 && angleDepth == 0 &&
          bracketDepth == 0) {
        if (flushParam())
          return true;
        break;
      }
      if (cj == ',' && parenDepth == 0 && angleDepth == 0 &&
          bracketDepth == 0) {
        if (flushParam())
          return true;
        current.clear();
        continue;
      }

      if (std::isspace(static_cast<unsigned char>(cj)))
        continue;

      if (isIdentifierChar(cj)) {
        size_t start = j;
        j++;
        while (j < text.size() && isIdentifierChar(text[j]))
          j++;
        current.push_back(LexToken{LexToken::Kind::Identifier,
                                   text.substr(start, j - start), start, j});
        j--;
        continue;
      }
      if (j + 1 < text.size()) {
        std::string two = text.substr(j, 2);
        if (two == "::" || two == "->" || two == "&&" || two == "||" ||
            two == "<=" || two == ">=" || two == "==" || two == "!=") {
          current.push_back(LexToken{LexToken::Kind::Punct, two, j, j + 2});
          j++;
          continue;
        }
      }
      current.push_back(
          LexToken{LexToken::Kind::Punct, std::string(1, cj), j, j + 1});
    }
  }
  return false;
}

static bool findFxBlockDeclarationInLine(const std::string &line,
                                         const std::string &word,
                                         size_t &posOut) {
  std::string code = line;
  size_t lineComment = code.find("//");
  if (lineComment != std::string::npos)
    code = code.substr(0, lineComment);
  auto tokens = lexLineTokens(code);
  if (tokens.size() < 2)
    return false;
  if (tokens[0].text == "#")
    return false;

  bool hasAssignBefore = false;
  for (const auto &tok : tokens) {
    if (tok.kind == LexToken::Kind::Punct && tok.text == "=") {
      hasAssignBefore = true;
      break;
    }
    if (tok.kind == LexToken::Kind::Identifier && tok.text == word)
      break;
  }
  if (hasAssignBefore)
    return false;

  auto isBlockType = [](const std::string &t) {
    return t == "texture" || t == "Texture" || t == "Texture2D" ||
           t == "Texture3D" || t == "TextureCube" || t == "SamplerState" ||
           t == "SamplerComparisonState" || t == "BlendState" ||
           t == "DepthStencilState" || t == "RasterizerState";
  };

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
    return false;
  if (!isBlockType(tokens[typeIndex].text))
    return false;
  if (typeIndex + 1 >= tokens.size())
    return false;
  if (tokens[typeIndex + 1].kind != LexToken::Kind::Identifier)
    return false;
  if (tokens[typeIndex + 1].text != word)
    return false;

  posOut = tokens[typeIndex + 1].start;
  return true;
}

static bool findNamedDefinitionInText(const std::string &text,
                                      const std::string &word,
                                      const PreprocessorView *preprocessorView,
                                      int &defLine, int &defStart,
                                      int &defEnd) {
  std::istringstream stream(text);
  std::string scanLine;
  int scanIndex = 0;
  std::string pendingUiVarName;
  int pendingUiVarLine = -1;
  size_t pendingUiVarStart = 0;
  std::string pendingUiVarLineText;
  while (std::getline(stream, scanLine)) {
    if (preprocessorView &&
        scanIndex < static_cast<int>(preprocessorView->lineActive.size()) &&
        !preprocessorView->lineActive[scanIndex]) {
      scanIndex++;
      continue;
    }

    std::string trimmed = scanLine;
    trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(),
                                                [](unsigned char ch) {
                                                  return !std::isspace(ch);
                                                }));

    if (!pendingUiVarName.empty()) {
      std::string t = trimmed;
      while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) {
        t.pop_back();
      }
      if (t == "<") {
        if (pendingUiVarName == word) {
          defLine = pendingUiVarLine;
          defStart = byteOffsetInLineToUtf16(
              pendingUiVarLineText, static_cast<int>(pendingUiVarStart));
          defEnd = defStart + static_cast<int>(word.size());
          return true;
        }
        pendingUiVarName.clear();
        pendingUiVarLine = -1;
        pendingUiVarStart = 0;
        pendingUiVarLineText.clear();
      } else if (!t.empty() && t.rfind("//", 0) != 0) {
        pendingUiVarName.clear();
        pendingUiVarLine = -1;
        pendingUiVarStart = 0;
        pendingUiVarLineText.clear();
      }
    }

    {
      std::string code = scanLine;
      size_t lineComment = code.find("//");
      if (lineComment != std::string::npos)
        code = code.substr(0, lineComment);
      const auto tokens = lexLineTokens(code);
      if (tokens.size() >= 2 && tokens[0].text != "#") {
        size_t typeIndex = std::string::npos;
        for (size_t i = 0; i < tokens.size(); i++) {
          if (tokens[i].kind != LexToken::Kind::Identifier)
            continue;
          if (isQualifierToken(tokens[i].text))
            continue;
          if (tokens[i].text == "return" || tokens[i].text == "if" ||
              tokens[i].text == "for" || tokens[i].text == "while" ||
              tokens[i].text == "switch" || tokens[i].text == "struct" ||
              tokens[i].text == "cbuffer")
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
            bool disqualify = false;
            for (const auto &t : tokens) {
              if (t.kind != LexToken::Kind::Punct)
                continue;
              if (t.text == ";" || t.text == "=" || t.text == "(" ||
                  t.text == "{" || t.text == "}" || t.text == "<" ||
                  t.text == ">") {
                disqualify = true;
                break;
              }
            }
            if (!disqualify) {
              pendingUiVarName = tokens[nameIndex].text;
              pendingUiVarLine = scanIndex;
              pendingUiVarStart = tokens[nameIndex].start;
              pendingUiVarLineText = scanLine;
            }
          }
        }
      }
    }

    auto hasWordAt = [&](const std::string &prefix) {
      if (trimmed.rfind(prefix, 0) != 0)
        return false;
      size_t nameStart = scanLine.find(prefix) + prefix.size();
      while (nameStart < scanLine.size() &&
             std::isspace(static_cast<unsigned char>(scanLine[nameStart]))) {
        nameStart++;
      }
      size_t nameEnd = scanLine.find_first_of(" \t{(:", nameStart);
      if (nameEnd == std::string::npos)
        nameEnd = scanLine.size();
      if (nameEnd <= nameStart)
        return false;
      return scanLine.substr(nameStart, nameEnd - nameStart) == word;
    };
    if (hasWordAt("struct ") || hasWordAt("cbuffer ") ||
        hasWordAt("technique ") || hasWordAt("pass ")) {
      size_t nameStart = scanLine.find(word);
      defLine = scanIndex;
      defStart = byteOffsetInLineToUtf16(scanLine, static_cast<int>(nameStart));
      defEnd = defStart + static_cast<int>(word.size());
      return true;
    }
    size_t funcPos = scanLine.find(word);
    while (funcPos != std::string::npos) {
      if (!isWordBoundary(scanLine, funcPos, funcPos + word.size())) {
        funcPos = scanLine.find(word, funcPos + word.size());
        continue;
      }
      size_t openParen = scanLine.find('(', funcPos + word.size());
      if (openParen != std::string::npos) {
        std::string signaturePrefix = scanLine.substr(0, openParen);
        if (signaturePrefix.find('=') != std::string::npos) {
          funcPos = scanLine.find(word, funcPos + word.size());
          continue;
        }
        std::string trimmedPrefix = signaturePrefix;
        trimmedPrefix.erase(
            trimmedPrefix.begin(),
            std::find_if(trimmedPrefix.begin(), trimmedPrefix.end(),
                         [](unsigned char ch) { return !std::isspace(ch); }));
        if (!trimmedPrefix.empty() && trimmedPrefix.rfind("//", 0) != 0) {
          std::istringstream prefixStream(signaturePrefix);
          std::vector<std::string> tokens;
          std::string token;
          while (prefixStream >> token) {
            tokens.push_back(token);
          }
          if (tokens.size() >= 2 && tokens.back() == word) {
            const std::string &firstToken = tokens.front();
            if (firstToken != "return" && firstToken != "if" &&
                firstToken != "for" && firstToken != "while" &&
                firstToken != "switch") {
              size_t closeParen = scanLine.find(')', openParen + 1);
              if (closeParen != std::string::npos) {
                std::string suffix = scanLine.substr(closeParen + 1);
                size_t suffixStart = suffix.find_first_not_of(" \t");
                suffix = suffixStart == std::string::npos
                             ? ""
                             : suffix.substr(suffixStart);
                if (suffix.empty() || suffix[0] == ':' || suffix[0] == '{' ||
                    suffix[0] == ';') {
                  defLine = scanIndex;
                  defStart = byteOffsetInLineToUtf16(scanLine,
                                                     static_cast<int>(funcPos));
                  defEnd = defStart + static_cast<int>(word.size());
                  return true;
                }
              }
            }
          }
        }
      }
      funcPos = scanLine.find(word, funcPos + word.size());
    }

    size_t declPos = 0;
    if (findMacroDefinitionInLine(scanLine, word, declPos) ||
        findDeclaredIdentifierInDeclarationLine(scanLine, word, declPos) ||
        findFxBlockDeclarationInLine(scanLine, word, declPos)) {
      defLine = scanIndex;
      defStart = byteOffsetInLineToUtf16(scanLine, static_cast<int>(declPos));
      defEnd = defStart + static_cast<int>(word.size());
      return true;
    }
    scanIndex++;
  }
  if (findParameterDeclarationInText(text, word, defLine, defStart, defEnd))
    return true;
  return false;
}

static bool findNamedDefinitionInText(const std::string &text,
                                      const std::string &word, int &defLine,
                                      int &defStart, int &defEnd) {
  return findNamedDefinitionInText(text, word, nullptr, defLine, defStart,
                                   defEnd);
}

static bool hasSameBranchFamilyShape(const PreprocBranchSig &a,
                                     const PreprocBranchSig &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (a[i].first != b[i].first)
      return false;
  }
  return true;
}

static bool branchSigStartsWith(const PreprocBranchSig &value,
                                const PreprocBranchSig &prefix) {
  if (prefix.size() > value.size())
    return false;
  for (size_t i = 0; i < prefix.size(); i++) {
    if (value[i] != prefix[i])
      return false;
  }
  return true;
}

static bool branchSigsCompatible(const PreprocBranchSig &a,
                                 const PreprocBranchSig &b) {
  size_t i = 0;
  size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i].first < b[j].first) {
      i++;
      continue;
    }
    if (b[j].first < a[i].first) {
      j++;
      continue;
    }
    if (a[i].second != b[j].second)
      return false;
    i++;
    j++;
  }
  return true;
}

static bool findConditionalFamilyDeclarationOnLine(const std::string &lineText,
                                                   const std::string &word) {
  auto findFunctionDefinitionOnLine = [&](size_t &posOut) {
    size_t funcPos = lineText.find(word);
    while (funcPos != std::string::npos) {
      if (!isWordBoundary(lineText, funcPos, funcPos + word.size())) {
        funcPos = lineText.find(word, funcPos + word.size());
        continue;
      }
      size_t openParen = lineText.find('(', funcPos + word.size());
      if (openParen == std::string::npos) {
        funcPos = lineText.find(word, funcPos + word.size());
        continue;
      }
      std::string signaturePrefix = lineText.substr(0, openParen);
      if (signaturePrefix.find('=') != std::string::npos) {
        funcPos = lineText.find(word, funcPos + word.size());
        continue;
      }
      std::string trimmedPrefix = signaturePrefix;
      trimmedPrefix.erase(
          trimmedPrefix.begin(),
          std::find_if(trimmedPrefix.begin(), trimmedPrefix.end(),
                       [](unsigned char ch) { return !std::isspace(ch); }));
      if (!trimmedPrefix.empty() && trimmedPrefix.rfind("//", 0) != 0) {
        std::istringstream prefixStream(signaturePrefix);
        std::vector<std::string> tokens;
        std::string token;
        while (prefixStream >> token)
          tokens.push_back(token);
        if (tokens.size() >= 2 && tokens.back() == word) {
          const std::string &firstToken = tokens.front();
          if (firstToken != "return" && firstToken != "if" &&
              firstToken != "for" && firstToken != "while" &&
              firstToken != "switch") {
            size_t closeParen = lineText.find(')', openParen + 1);
            if (closeParen != std::string::npos) {
              std::string suffix = lineText.substr(closeParen + 1);
              size_t suffixStart = suffix.find_first_not_of(" \t");
              suffix = suffixStart == std::string::npos
                           ? ""
                           : suffix.substr(suffixStart);
              if (suffix.empty() || suffix[0] == ':' || suffix[0] == '{' ||
                  suffix[0] == ';') {
                posOut = funcPos;
                return true;
              }
            }
          }
        }
      }
      funcPos = lineText.find(word, funcPos + word.size());
    }
    return false;
  };

  size_t pos = 0;
  if (findDeclaredIdentifierInDeclarationLine(lineText, word, pos))
    return true;
  if (findFunctionDefinitionOnLine(pos))
    return true;
  if (findMacroDefinitionInLine(lineText, word, pos))
    return true;
  if (findFxBlockDeclarationInLine(lineText, word, pos))
    return true;
  return false;
}

static bool collectConditionalFamilyOccurrencesInSingleDocument(
    const std::string &uri, const std::string &text, const std::string &word,
    const std::unordered_map<std::string, int> &defines,
    const DefinitionLocation &definition,
    std::vector<LocatedOccurrence> &locationsOut) {
  locationsOut.clear();
  if (definition.uri != uri || definition.line < 0)
    return false;

  const PreprocessorView preprocessorView = buildPreprocessorView(text, defines);
  if (definition.line >= static_cast<int>(preprocessorView.branchSigs.size()))
    return false;

  const PreprocBranchSig &targetSig = preprocessorView.branchSigs[definition.line];
  if (targetSig.empty())
    return false;

  std::vector<PreprocBranchSig> familyVariants;
  std::istringstream stream(text);
  std::string lineText;
  int lineIndex = 0;
  while (std::getline(stream, lineText)) {
    if (lineIndex >= static_cast<int>(preprocessorView.branchSigs.size()))
      break;
    if (findConditionalFamilyDeclarationOnLine(lineText, word) &&
        hasSameBranchFamilyShape(preprocessorView.branchSigs[lineIndex],
                                 targetSig)) {
      familyVariants.push_back(preprocessorView.branchSigs[lineIndex]);
    }
    lineIndex++;
  }

  if (familyVariants.size() < 2)
    return false;

  auto occurrences = findOccurrences(text, word);
  for (const auto &occ : occurrences) {
    if (occ.line < 0 ||
        occ.line >= static_cast<int>(preprocessorView.branchSigs.size()))
      continue;
    const PreprocBranchSig &occSig = preprocessorView.branchSigs[occ.line];
    for (const auto &familySig : familyVariants) {
      if (branchSigsCompatible(occSig, familySig)) {
        locationsOut.push_back(LocatedOccurrence{uri, occ.line, occ.start,
                                                 occ.end});
        break;
      }
    }
  }

  return !locationsOut.empty();
}

static std::vector<LocatedOccurrence> collectActiveOccurrencesInDocument(
    const std::string &uri, const std::string &text, const std::string &word,
    const std::unordered_map<std::string, int> &defines) {
  std::vector<LocatedOccurrence> locations;
  const PreprocessorView preprocessorView = buildPreprocessorView(text, defines);
  auto occurrences = findOccurrences(text, word);
  for (const auto &occ : occurrences) {
    if (occ.line >= 0 &&
        occ.line < static_cast<int>(preprocessorView.lineActive.size()) &&
        !preprocessorView.lineActive[occ.line]) {
      continue;
    }
    locations.push_back(LocatedOccurrence{uri, occ.line, occ.start, occ.end});
  }
  return locations;
}

static void appendUniqueOccurrence(
    std::vector<LocatedOccurrence> &locations,
    std::unordered_set<std::string> &seenKeys,
    const LocatedOccurrence &occurrence) {
  const std::string key =
      occurrence.uri + "|" + std::to_string(occurrence.line) + "|" +
      std::to_string(occurrence.start) + "|" + std::to_string(occurrence.end);
  if (!seenKeys.insert(key).second)
    return;
  locations.push_back(occurrence);
}

struct IncludeGraphCacheMetricsSnapshot {
  uint64_t lookups = 0;
  uint64_t cacheHits = 0;
  uint64_t rebuilds = 0;
  uint64_t invalidations = 0;
};

std::atomic<bool> gSemanticCacheEnabled{true};
std::mutex gIncludeGraphCacheMutex;
std::unordered_map<std::string, std::vector<std::string>>
    gIncludeGraphOrderedByRoot;
std::unordered_map<std::string, std::unordered_set<std::string>>
    gIncludeGraphRootsByUri;
IncludeGraphCacheMetricsSnapshot gIncludeGraphCacheMetrics;

SemanticCacheKey
makeSemanticCacheKeyForUri(const std::string &uri,
                           const std::vector<std::string> &workspaceFolders,
                           const std::vector<std::string> &includePaths,
                           const std::vector<std::string> &shaderExtensions) {
  SemanticCacheKey key;
  key.workspaceFolders = workspaceFolders;
  key.includePaths = includePaths;
  key.shaderExtensions = shaderExtensions;
  key.unitPath = uriToPath(uri);
  return key;
}

void eraseIncludeGraphRootLocked(const std::string &rootUri) {
  auto rootIt = gIncludeGraphOrderedByRoot.find(rootUri);
  if (rootIt == gIncludeGraphOrderedByRoot.end())
    return;
  for (const auto &depUri : rootIt->second) {
    auto depIt = gIncludeGraphRootsByUri.find(depUri);
    if (depIt == gIncludeGraphRootsByUri.end())
      continue;
    depIt->second.erase(rootUri);
    if (depIt->second.empty())
      gIncludeGraphRootsByUri.erase(depIt);
  }
  gIncludeGraphOrderedByRoot.erase(rootIt);
}

void collectIncludeGraphUrisUncached(
    const std::string &nodeUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited,
    std::vector<std::string> &orderedUris) {
  if (!visited.insert(nodeUri).second)
    return;
  orderedUris.push_back(nodeUri);
  std::string text;
  if (!loadDocumentText(nodeUri, documents, text))
    return;
  std::istringstream stream(text);
  std::string lineText;
  while (std::getline(stream, lineText)) {
    std::string includePath;
    if (!extractIncludePath(lineText, includePath))
      continue;
    auto candidates = resolveIncludeCandidates(
        nodeUri, includePath, workspaceFolders, includePaths, shaderExtensions);
    std::vector<std::string> candidateUris;
    candidateUris.reserve(candidates.size());
    for (const auto &candidate : candidates) {
      struct _stat statBuffer;
      if (_stat(candidate.c_str(), &statBuffer) != 0)
        continue;
      candidateUris.push_back(pathToUri(candidate));
    }
    prefetchDocumentTexts(candidateUris, documents);
    for (const auto &candidateUri : candidateUris) {
      collectIncludeGraphUrisUncached(candidateUri, documents, workspaceFolders,
                                      includePaths, shaderExtensions, visited,
                                      orderedUris);
      break;
    }
  }
}

static std::vector<std::string> getIncludeGraphUrisCachedImpl(
    const std::string &rootUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions, bool allowSemanticCache) {
  uint64_t rootEpoch = 0;
  auto rootIt = documents.find(rootUri);
  if (rootIt != documents.end())
    rootEpoch = rootIt->second.epoch;
  if (allowSemanticCache &&
      gSemanticCacheEnabled.load(std::memory_order_relaxed)) {
    SemanticCacheKey key = makeSemanticCacheKeyForUri(
        rootUri, workspaceFolders, includePaths, shaderExtensions);
    auto snapshot = semanticCacheGetSnapshot(key, rootUri, rootEpoch);
    if (snapshot && snapshot->includeGraphComplete)
      return snapshot->includeGraphUrisOrdered;
  }
  {
    std::lock_guard<std::mutex> lock(gIncludeGraphCacheMutex);
    gIncludeGraphCacheMetrics.lookups++;
    auto cached = gIncludeGraphOrderedByRoot.find(rootUri);
    if (cached != gIncludeGraphOrderedByRoot.end()) {
      gIncludeGraphCacheMetrics.cacheHits++;
      return cached->second;
    }
  }

  std::unordered_set<std::string> visited;
  std::vector<std::string> orderedUris;
  collectIncludeGraphUrisUncached(rootUri, documents, workspaceFolders,
                                  includePaths, shaderExtensions, visited,
                                  orderedUris);

  {
    std::lock_guard<std::mutex> lock(gIncludeGraphCacheMutex);
    gIncludeGraphCacheMetrics.rebuilds++;
    eraseIncludeGraphRootLocked(rootUri);
    gIncludeGraphOrderedByRoot[rootUri] = orderedUris;
    for (const auto &depUri : orderedUris)
      gIncludeGraphRootsByUri[depUri].insert(rootUri);
  }
  if (allowSemanticCache &&
      gSemanticCacheEnabled.load(std::memory_order_relaxed)) {
    SemanticCacheKey key = makeSemanticCacheKeyForUri(
        rootUri, workspaceFolders, includePaths, shaderExtensions);
    SemanticSnapshot created;
    created.uri = rootUri;
    created.documentEpoch = rootEpoch;
    created.includeGraphUrisOrdered = orderedUris;
    created.includeGraphComplete = true;
    semanticCacheUpsertSnapshot(key, created);
  }
  return orderedUris;
}

std::vector<std::string> getIncludeGraphUrisCached(
    const std::string &rootUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions) {
  return getIncludeGraphUrisCachedImpl(rootUri, documents, workspaceFolders,
                                       includePaths, shaderExtensions, true);
}

void invalidateIncludeGraphCacheByUri(const std::string &changedUri) {
  if (changedUri.empty())
    return;
  semanticCacheInvalidateUri(changedUri);
  std::lock_guard<std::mutex> lock(gIncludeGraphCacheMutex);
  gIncludeGraphCacheMetrics.invalidations++;
  std::vector<std::string> impactedRoots;
  auto depIt = gIncludeGraphRootsByUri.find(changedUri);
  if (depIt != gIncludeGraphRootsByUri.end())
    impactedRoots.assign(depIt->second.begin(), depIt->second.end());
  if (gIncludeGraphOrderedByRoot.find(changedUri) !=
      gIncludeGraphOrderedByRoot.end()) {
    impactedRoots.push_back(changedUri);
  }
  std::sort(impactedRoots.begin(), impactedRoots.end());
  impactedRoots.erase(std::unique(impactedRoots.begin(), impactedRoots.end()),
                      impactedRoots.end());
  for (const auto &rootUri : impactedRoots)
    eraseIncludeGraphRootLocked(rootUri);
}

void invalidateIncludeGraphCacheByUris(
    const std::vector<std::string> &changedUris) {
  for (const auto &changedUri : changedUris)
    invalidateIncludeGraphCacheByUri(changedUri);
}

IncludeGraphCacheMetricsSnapshot takeIncludeGraphCacheMetricsSnapshot() {
  std::lock_guard<std::mutex> lock(gIncludeGraphCacheMutex);
  IncludeGraphCacheMetricsSnapshot snapshot = gIncludeGraphCacheMetrics;
  gIncludeGraphCacheMetrics = IncludeGraphCacheMetricsSnapshot{};
  return snapshot;
}

void invalidateAllIncludeGraphCaches() {
  std::lock_guard<std::mutex> lock(gIncludeGraphCacheMutex);
  gIncludeGraphCacheMetrics.invalidations++;
  gIncludeGraphOrderedByRoot.clear();
  gIncludeGraphRootsByUri.clear();
}

bool findDefinitionInIncludeGraph(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    std::unordered_set<std::string> &visited, DefinitionLocation &location) {
  if (!visited.insert(uri).second)
    return false;
  uint64_t rootEpoch = 0;
  auto rootIt = documents.find(uri);
  if (rootIt != documents.end())
    rootEpoch = rootIt->second.epoch;
  if (gSemanticCacheEnabled.load(std::memory_order_relaxed)) {
    SemanticCacheKey key = makeSemanticCacheKeyForUri(
        uri, workspaceFolders, includePaths, shaderExtensions);
    auto snapshot = semanticCacheGetSnapshot(key, uri, rootEpoch);
    if (snapshot) {
      prefetchDocumentTexts(snapshot->includeGraphUrisOrdered, documents);
      for (const auto &candidateUri : snapshot->includeGraphUrisOrdered) {
        visited.insert(candidateUri);
        std::string text;
        if (!loadDocumentText(candidateUri, documents, text))
          continue;
        int defLine = -1;
        int defStart = -1;
        int defEnd = -1;
        const PreprocessorView preprocessorView =
            buildPreprocessorView(text, defines);
        if (findNamedDefinitionInText(text, word, &preprocessorView, defLine,
                                      defStart, defEnd)) {
          location =
              DefinitionLocation{candidateUri, defLine, defStart, defEnd};
          return true;
        }
      }
    }
  }
  const auto orderedUris = getIncludeGraphUrisCached(
      uri, documents, workspaceFolders, includePaths, shaderExtensions);
  prefetchDocumentTexts(orderedUris, documents);
  for (const auto &candidateUri : orderedUris) {
    visited.insert(candidateUri);
    std::string text;
    if (!loadDocumentText(candidateUri, documents, text))
      continue;
    int defLine = -1;
    int defStart = -1;
    int defEnd = -1;
    const PreprocessorView preprocessorView =
        buildPreprocessorView(text, defines);
    if (findNamedDefinitionInText(text, word, &preprocessorView, defLine,
                                  defStart, defEnd)) {
      location = DefinitionLocation{candidateUri, defLine, defStart, defEnd};
      return true;
    }
  }
  return false;
}

bool findDefinitionInIncludeGraph(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited, DefinitionLocation &location) {
  static const std::unordered_map<std::string, int> emptyDefines;
  return findDefinitionInIncludeGraph(uri, word, documents, workspaceFolders,
                                      includePaths, shaderExtensions,
                                      emptyDefines, visited, location);
}

bool findDefinitionInIncludeGraphLegacy(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited, DefinitionLocation &location) {
  if (!visited.insert(uri).second)
    return false;
  const auto orderedUris = getIncludeGraphUrisCachedImpl(
      uri, documents, workspaceFolders, includePaths, shaderExtensions, false);
  prefetchDocumentTexts(orderedUris, documents);
  for (const auto &candidateUri : orderedUris) {
    visited.insert(candidateUri);
    std::string text;
    if (!loadDocumentText(candidateUri, documents, text))
      continue;
    int defLine = -1;
    int defStart = -1;
    int defEnd = -1;
    if (findNamedDefinitionInText(text, word, defLine, defStart, defEnd)) {
      location = DefinitionLocation{candidateUri, defLine, defStart, defEnd};
      return true;
    }
  }
  return false;
}

static std::vector<LocatedOccurrence> collectOccurrencesInIncludeGraph(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines) {
  auto orderedUris = getIncludeGraphUrisCached(uri, documents, workspaceFolders,
                                               includePaths, shaderExtensions);
  prefetchDocumentTexts(orderedUris, documents);
  std::vector<LocatedOccurrence> locations;
  for (const auto &candidateUri : orderedUris) {
    std::string text;
    if (!loadDocumentText(candidateUri, documents, text))
      continue;
    auto docLocations =
        collectActiveOccurrencesInDocument(candidateUri, text, word, defines);
    locations.insert(locations.end(), docLocations.begin(), docLocations.end());
  }
  return locations;
}
static std::vector<LocatedOccurrence> collectOccurrencesInSingleDocument(
    const std::string &uri, const std::string &text, const std::string &word,
    const std::unordered_map<std::string, int> &defines) {
  return collectActiveOccurrencesInDocument(uri, text, word, defines);
}

std::vector<LocatedOccurrence> collectOccurrencesForSymbol(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines) {
  std::string currentText;
  if (!loadDocumentText(uri, documents, currentText))
    return {};

  DefinitionLocation definition;
  std::unordered_set<std::string> visited;
  if (findDefinitionInIncludeGraph(uri, word, documents, workspaceFolders,
                                   includePaths, shaderExtensions, defines,
                                   visited, definition)) {
    if (definition.uri == uri) {
      std::vector<LocatedOccurrence> conditionalFamilyLocations;
      if (collectConditionalFamilyOccurrencesInSingleDocument(
              uri, currentText, word, defines, definition,
              conditionalFamilyLocations)) {
        return conditionalFamilyLocations;
      }
      return collectOccurrencesInSingleDocument(uri, currentText, word, defines);
    }
    auto locations = collectOccurrencesInIncludeGraph(
        uri, word, documents, workspaceFolders, includePaths, shaderExtensions,
        defines);
    std::string definitionText;
    if (loadDocumentText(definition.uri, documents, definitionText)) {
      std::vector<LocatedOccurrence> familyLocations;
      if (collectConditionalFamilyOccurrencesInSingleDocument(
              definition.uri, definitionText, word, defines, definition,
              familyLocations)) {
        std::vector<LocatedOccurrence> merged;
        std::unordered_set<std::string> seenKeys;
        merged.reserve(locations.size() + familyLocations.size());
        for (const auto &occ : locations)
          appendUniqueOccurrence(merged, seenKeys, occ);
        for (const auto &occ : familyLocations)
          appendUniqueOccurrence(merged, seenKeys, occ);
        return merged;
      }
    }
    return locations;
  }

  return collectOccurrencesInSingleDocument(uri, currentText, word, defines);
}

std::vector<LocatedOccurrence> collectOccurrencesForSymbol(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions) {
  static const std::unordered_map<std::string, int> emptyDefines;
  return collectOccurrencesForSymbol(uri, word, documents, workspaceFolders,
                                     includePaths, shaderExtensions,
                                     emptyDefines);
}

int main(int argc, char **argv) {
  installCrashHandler("nsf_lsp_crash.log");
#ifdef _WIN32
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--debug-wait") {
      waitForDebugger();
    }
  }
#endif
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  getHlslBuiltinNames();
  ServerRequestContext core;
  std::mutex coreMutex;
  core.shaderExtensions = {".nsf", ".hlsl", ".hlsli", ".fx", ".usf",
                           ".ush"};
  core.semanticLegend = createDefaultSemanticTokenLegend();
  std::unordered_map<std::string, int> preprocessorDefines;
  auto applySettings = [&](const Json &settings) {
    std::lock_guard<std::mutex> lock(coreMutex);
    applySettingsFromJson(
        settings, core.includePaths, core.shaderExtensions, preprocessorDefines,
        core.inlayHintsEnabled, core.inlayHintsParameterNamesEnabled,
        core.semanticTokensEnabled, core.diagnosticsExpensiveRulesEnabled,
        core.diagnosticsTimeBudgetMs, core.diagnosticsMaxItems,
        core.diagnosticsFastEnabled, core.diagnosticsFastDelayMs,
        core.diagnosticsFastTimeBudgetMs, core.diagnosticsFastMaxItems,
        core.diagnosticsFullEnabled, core.diagnosticsFullDelayMs,
        core.diagnosticsFullExpensiveRulesEnabled,
        core.diagnosticsFullTimeBudgetMs, core.diagnosticsFullMaxItems,
        core.diagnosticsWorkerCount, core.diagnosticsAutoWorkerCount,
        core.semanticCacheEnabled,
        core.diagnosticsIndeterminateEnabled,
        core.diagnosticsIndeterminateSeverity,
        core.diagnosticsIndeterminateMaxItems,
        core.diagnosticsIndeterminateSuppressWhenErrors,
        core.indexingWorkerCount, core.indexingQueueCapacity);
    core.preprocessorDefines = preprocessorDefines;
    const int maxPrefetchWorkers =
        std::min(std::max(1, core.indexingWorkerCount), 8);
    setDocumentPrefetchMaxConcurrency(maxPrefetchWorkers);
    gSemanticCacheEnabled.store(core.semanticCacheEnabled,
                                std::memory_order_relaxed);
  };
  setDocumentPrefetchMaxConcurrency(
      std::min(std::max(1, core.indexingWorkerCount), 8));

  static const std::vector<std::string> noLanguageItems;

  enum class DiagnosticsJobKind : int { Fast = 0, Full = 1 };
  enum class DiagnosticsQueuePriority : int { Fast = 0, Full = 1 };

  struct PendingDiagnosticsJob {
    DiagnosticsJobKind kind = DiagnosticsJobKind::Fast;
    bool hasPairedFull = false;
    int documentVersion = 0;
    uint64_t documentEpoch = 0;
    std::string uri;
    std::string text;
    std::vector<std::string> workspaceFolders;
    std::vector<std::string> includePaths;
    std::vector<std::string> shaderExtensions;
    std::unordered_map<std::string, int> defines;
    DiagnosticsBuildOptions diagnosticsOptions;
    std::chrono::steady_clock::time_point enqueuedAt;
    std::chrono::steady_clock::time_point due;
    std::chrono::steady_clock::time_point readyAt;
  };

  struct MethodMetric {
    uint64_t count = 0;
    uint64_t cancelled = 0;
    uint64_t failed = 0;
    double totalMs = 0.0;
    double maxMs = 0.0;
  };
  struct DiagnosticsMetric {
    uint64_t count = 0;
    uint64_t truncated = 0;
    uint64_t timedOut = 0;
    uint64_t heavyRulesSkipped = 0;
    uint64_t indeterminateTotal = 0;
    uint64_t indeterminateReasonRhsTypeEmpty = 0;
    uint64_t indeterminateReasonBudgetTimeout = 0;
    uint64_t indeterminateReasonHeavyRulesSkipped = 0;
    uint64_t semanticCacheSnapshotHit = 0;
    uint64_t semanticCacheSnapshotMiss = 0;
    uint64_t staleDroppedBeforeBuild = 0;
    uint64_t staleDroppedBeforePublish = 0;
    uint64_t canceledInPending = 0;
    uint64_t queueMaxPendingTotal = 0;
    uint64_t queueMaxReadyTotal = 0;
    uint64_t queueWaitSamples = 0;
    double queueWaitTotalMs = 0.0;
    double queueWaitMaxMs = 0.0;
    double totalMs = 0.0;
    double maxMs = 0.0;
  };
  std::mutex metricsMutex;
  std::unordered_map<std::string, MethodMetric> methodMetrics;
  DiagnosticsMetric diagnosticsMetrics;

  std::mutex diagnosticsMutex;
  std::condition_variable diagnosticsCv;
  bool diagnosticsStopping = false;
  std::unordered_map<std::string, PendingDiagnosticsJob> diagnosticsPendingFast;
  std::unordered_map<std::string, PendingDiagnosticsJob> diagnosticsPendingFull;
  std::deque<PendingDiagnosticsJob> diagnosticsReady[2];
  uint64_t documentEpochCounter = 0;

  auto recordMethodMetric = [&](const std::string &method, double durationMs,
                                bool canceled, bool failed) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    MethodMetric &metric = methodMetrics[method];
    metric.count++;
    if (canceled)
      metric.cancelled++;
    if (failed)
      metric.failed++;
    metric.totalMs += durationMs;
    metric.maxMs = std::max(metric.maxMs, durationMs);
  };
  auto recordDiagnosticsMetric = [&](const DiagnosticsBuildResult &result) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.count++;
    diagnosticsMetrics.totalMs += result.elapsedMs;
    diagnosticsMetrics.maxMs =
        std::max(diagnosticsMetrics.maxMs, result.elapsedMs);
    if (result.truncated)
      diagnosticsMetrics.truncated++;
    if (result.timedOut)
      diagnosticsMetrics.timedOut++;
    if (result.heavyRulesSkipped)
      diagnosticsMetrics.heavyRulesSkipped++;
    diagnosticsMetrics.indeterminateTotal += result.indeterminateTotal;
    diagnosticsMetrics.indeterminateReasonRhsTypeEmpty +=
        result.indeterminateReasonRhsTypeEmpty;
    diagnosticsMetrics.indeterminateReasonBudgetTimeout +=
        result.indeterminateReasonBudgetTimeout;
    diagnosticsMetrics.indeterminateReasonHeavyRulesSkipped +=
        result.indeterminateReasonHeavyRulesSkipped;
  };
  auto recordDiagnosticsStaleDropBeforeBuild = [&]() {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.staleDroppedBeforeBuild++;
  };
  auto recordDiagnosticsStaleDropBeforePublish = [&]() {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.staleDroppedBeforePublish++;
  };
  auto recordDiagnosticsQueueWait = [&](double waitMs) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.queueWaitSamples++;
    diagnosticsMetrics.queueWaitTotalMs += waitMs;
    diagnosticsMetrics.queueWaitMaxMs =
        std::max(diagnosticsMetrics.queueWaitMaxMs, waitMs);
  };
  auto recordDiagnosticsQueueMax = [&](size_t pendingTotal, size_t readyTotal) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.queueMaxPendingTotal =
        std::max(diagnosticsMetrics.queueMaxPendingTotal,
                 static_cast<uint64_t>(pendingTotal));
    diagnosticsMetrics.queueMaxReadyTotal =
        std::max(diagnosticsMetrics.queueMaxReadyTotal,
                 static_cast<uint64_t>(readyTotal));
  };

  auto isDocumentEpochCurrent = [&](const std::string &uri, uint64_t epoch) {
    std::lock_guard<std::mutex> lock(coreMutex);
    auto it = core.documents.find(uri);
    return it != core.documents.end() && it->second.epoch == epoch;
  };

  auto publishDiagnosticsNow = [&](const PendingDiagnosticsJob &job) {
    if (!isDocumentEpochCurrent(job.uri, job.documentEpoch)) {
      recordDiagnosticsStaleDropBeforeBuild();
      return;
    }
    if (job.kind == DiagnosticsJobKind::Full) {
      updateFullAstForDocument(job.uri, job.text, job.documentEpoch);
    }
    Json params = makeObject();
    params.o["uri"] = makeString(job.uri);
    const DiagnosticsBuildResult diagnosticsResult =
        buildDiagnosticsWithOptions(job.uri, job.text, job.workspaceFolders,
                                    job.includePaths, job.shaderExtensions,
                                    job.defines, job.diagnosticsOptions);
    params.o["diagnostics"] = diagnosticsResult.diagnostics;
    recordDiagnosticsMetric(diagnosticsResult);
    if (job.kind == DiagnosticsJobKind::Fast && job.hasPairedFull &&
        diagnosticsResult.timedOut) {
      return;
    }
    if (!isDocumentEpochCurrent(job.uri, job.documentEpoch)) {
      recordDiagnosticsStaleDropBeforePublish();
      return;
    }
    writeNotification("textDocument/publishDiagnostics", params);
  };

  auto scheduleDiagnosticsForText =
      [&](const Document &document, bool scheduleFast = true,
          bool scheduleFull = true, int fastDelayOverrideMs = -1,
          int fullDelayOverrideMs = -1) {
        PendingDiagnosticsJob fastJob;
        bool queueFast = false;
        PendingDiagnosticsJob fullJob;
        bool queueFull = false;
        {
          std::lock_guard<std::mutex> lock(coreMutex);
          const auto now = std::chrono::steady_clock::now();
          if (scheduleFast && core.diagnosticsFastEnabled) {
            fastJob.kind = DiagnosticsJobKind::Fast;
            fastJob.hasPairedFull = scheduleFull && core.diagnosticsFullEnabled;
            fastJob.uri = document.uri;
            fastJob.text = document.text;
            fastJob.documentVersion = document.version;
            fastJob.documentEpoch = document.epoch;
            fastJob.workspaceFolders = core.workspaceFolders;
            fastJob.includePaths = core.includePaths;
            fastJob.shaderExtensions = core.shaderExtensions;
            fastJob.defines = preprocessorDefines;
            fastJob.diagnosticsOptions.enableExpensiveRules = true;
            fastJob.diagnosticsOptions.timeBudgetMs =
                core.diagnosticsFastTimeBudgetMs;
            fastJob.diagnosticsOptions.maxItems = core.diagnosticsFastMaxItems;
            fastJob.diagnosticsOptions.semanticCacheEnabled =
                core.semanticCacheEnabled;
            fastJob.diagnosticsOptions.documentEpoch = document.epoch;
            fastJob.diagnosticsOptions.indeterminateEnabled =
                core.diagnosticsIndeterminateEnabled;
            fastJob.diagnosticsOptions.indeterminateSeverity =
                core.diagnosticsIndeterminateSeverity;
            fastJob.diagnosticsOptions.indeterminateMaxItems =
                core.diagnosticsIndeterminateMaxItems;
            fastJob.diagnosticsOptions.indeterminateSuppressWhenErrors =
                core.diagnosticsIndeterminateSuppressWhenErrors;
            const int delayMs = fastDelayOverrideMs >= 0
                                    ? fastDelayOverrideMs
                                    : core.diagnosticsFastDelayMs;
            fastJob.enqueuedAt = now;
            fastJob.due = now + std::chrono::milliseconds(std::max(0, delayMs));
            queueFast = true;
          }
          if (scheduleFull && core.diagnosticsFullEnabled) {
            fullJob.kind = DiagnosticsJobKind::Full;
            fullJob.uri = document.uri;
            fullJob.text = document.text;
            fullJob.documentVersion = document.version;
            fullJob.documentEpoch = document.epoch;
            fullJob.workspaceFolders = core.workspaceFolders;
            fullJob.includePaths = core.includePaths;
            fullJob.shaderExtensions = core.shaderExtensions;
            fullJob.defines = preprocessorDefines;
            fullJob.diagnosticsOptions.enableExpensiveRules =
                core.diagnosticsFullExpensiveRulesEnabled;
            fullJob.diagnosticsOptions.timeBudgetMs =
                core.diagnosticsFullTimeBudgetMs;
            fullJob.diagnosticsOptions.maxItems = core.diagnosticsFullMaxItems;
            fullJob.diagnosticsOptions.semanticCacheEnabled =
                core.semanticCacheEnabled;
            fullJob.diagnosticsOptions.documentEpoch = document.epoch;
            fullJob.diagnosticsOptions.indeterminateEnabled =
                core.diagnosticsIndeterminateEnabled;
            fullJob.diagnosticsOptions.indeterminateSeverity =
                core.diagnosticsIndeterminateSeverity;
            fullJob.diagnosticsOptions.indeterminateMaxItems =
                core.diagnosticsIndeterminateMaxItems;
            fullJob.diagnosticsOptions.indeterminateSuppressWhenErrors =
                core.diagnosticsIndeterminateSuppressWhenErrors;
            const int delayMs = fullDelayOverrideMs >= 0
                                    ? fullDelayOverrideMs
                                    : core.diagnosticsFullDelayMs;
            fullJob.enqueuedAt = now;
            fullJob.due = now + std::chrono::milliseconds(std::max(0, delayMs));
            queueFull = true;
          }
        }
        if (!queueFast && !queueFull)
          return;
        {
          std::lock_guard<std::mutex> lock(diagnosticsMutex);
          if (queueFast)
            diagnosticsPendingFast[document.uri] = std::move(fastJob);
          if (queueFull)
            diagnosticsPendingFull[document.uri] = std::move(fullJob);
          const size_t pendingTotal =
              diagnosticsPendingFast.size() + diagnosticsPendingFull.size();
          const size_t readyTotal =
              diagnosticsReady[0].size() + diagnosticsReady[1].size();
          recordDiagnosticsQueueMax(pendingTotal, readyTotal);
        }
        diagnosticsCv.notify_all();
      };

  auto cancelDiagnosticsAndPublishEmpty = [&](const std::string &uri) {
    size_t canceledJobs = 0;
    {
      std::lock_guard<std::mutex> lock(diagnosticsMutex);
      canceledJobs += diagnosticsPendingFast.erase(uri);
      canceledJobs += diagnosticsPendingFull.erase(uri);
      const size_t pendingTotal =
          diagnosticsPendingFast.size() + diagnosticsPendingFull.size();
      const size_t readyTotal =
          diagnosticsReady[0].size() + diagnosticsReady[1].size();
      recordDiagnosticsQueueMax(pendingTotal, readyTotal);
    }
    if (canceledJobs > 0) {
      std::lock_guard<std::mutex> lock(metricsMutex);
      diagnosticsMetrics.canceledInPending += canceledJobs;
    }
    Json params = makeObject();
    params.o["uri"] = makeString(uri);
    params.o["diagnostics"] = makeArray();
    writeNotification("textDocument/publishDiagnostics", params);
  };

  std::thread diagnosticsSchedulerThread([&]() {
    std::unique_lock<std::mutex> lock(diagnosticsMutex);
    while (!diagnosticsStopping) {
      if (diagnosticsPendingFast.empty() && diagnosticsPendingFull.empty()) {
        diagnosticsCv.wait(lock, [&]() {
          return diagnosticsStopping || !diagnosticsPendingFast.empty() ||
                 !diagnosticsPendingFull.empty();
        });
        continue;
      }

      auto nextDue = std::chrono::steady_clock::time_point::max();
      for (const auto &entry : diagnosticsPendingFast) {
        nextDue = std::min(nextDue, entry.second.due);
      }
      for (const auto &entry : diagnosticsPendingFull) {
        nextDue = std::min(nextDue, entry.second.due);
      }

      diagnosticsCv.wait_until(lock, nextDue,
                               [&]() { return diagnosticsStopping; });
      if (diagnosticsStopping)
        break;

      const auto now = std::chrono::steady_clock::now();
      auto collectReady =
          [&](std::unordered_map<std::string, PendingDiagnosticsJob> &pending,
              DiagnosticsQueuePriority priority) {
            for (auto it = pending.begin(); it != pending.end();) {
              if (it->second.due <= now) {
                it->second.readyAt = now;
                diagnosticsReady[static_cast<int>(priority)].push_back(
                    std::move(it->second));
                it = pending.erase(it);
              } else {
                ++it;
              }
            }
          };
      collectReady(diagnosticsPendingFast, DiagnosticsQueuePriority::Fast);
      collectReady(diagnosticsPendingFull, DiagnosticsQueuePriority::Full);
      const size_t pendingTotal =
          diagnosticsPendingFast.size() + diagnosticsPendingFull.size();
      const size_t readyTotal =
          diagnosticsReady[0].size() + diagnosticsReady[1].size();
      recordDiagnosticsQueueMax(pendingTotal, readyTotal);
      diagnosticsCv.notify_all();
    }
  });

  auto takeNextReadyDiagnosticsJob = [&]() -> PendingDiagnosticsJob {
    std::unique_lock<std::mutex> lock(diagnosticsMutex);
    diagnosticsCv.wait(lock, [&]() {
      if (diagnosticsStopping)
        return true;
      return !diagnosticsReady[static_cast<int>(DiagnosticsQueuePriority::Fast)]
                  .empty() ||
             !diagnosticsReady[static_cast<int>(DiagnosticsQueuePriority::Full)]
                  .empty();
    });
    if (diagnosticsStopping)
      return PendingDiagnosticsJob{};
    for (int i = 0; i < 2; i++) {
      if (!diagnosticsReady[i].empty()) {
        PendingDiagnosticsJob job = std::move(diagnosticsReady[i].front());
        diagnosticsReady[i].pop_front();
        const auto now = std::chrono::steady_clock::now();
        if (job.readyAt.time_since_epoch().count() > 0) {
          const double waitMs = static_cast<double>(
              std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                    job.readyAt)
                  .count());
          recordDiagnosticsQueueWait(waitMs);
        }
        const size_t pendingTotal =
            diagnosticsPendingFast.size() + diagnosticsPendingFull.size();
        const size_t readyTotal =
            diagnosticsReady[0].size() + diagnosticsReady[1].size();
        recordDiagnosticsQueueMax(pendingTotal, readyTotal);
        return job;
      }
    }
    return PendingDiagnosticsJob{};
  };

  auto takeNextReadyDiagnosticsJobForPriority =
      [&](DiagnosticsQueuePriority priority) -> PendingDiagnosticsJob {
    const int idx = static_cast<int>(priority);
    std::unique_lock<std::mutex> lock(diagnosticsMutex);
    diagnosticsCv.wait(lock, [&]() {
      if (diagnosticsStopping)
        return true;
      return !diagnosticsReady[idx].empty();
    });
    if (diagnosticsStopping)
      return PendingDiagnosticsJob{};
    PendingDiagnosticsJob job = std::move(diagnosticsReady[idx].front());
    diagnosticsReady[idx].pop_front();
    const auto now = std::chrono::steady_clock::now();
    if (job.readyAt.time_since_epoch().count() > 0) {
      const double waitMs = static_cast<double>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                                job.readyAt)
              .count());
      recordDiagnosticsQueueWait(waitMs);
    }
    const size_t pendingTotal =
        diagnosticsPendingFast.size() + diagnosticsPendingFull.size();
    const size_t readyTotal =
        diagnosticsReady[0].size() + diagnosticsReady[1].size();
    recordDiagnosticsQueueMax(pendingTotal, readyTotal);
    return job;
  };

  auto runMixedDiagnosticsWorker = [&]() {
    while (true) {
      PendingDiagnosticsJob job = takeNextReadyDiagnosticsJob();
      if (job.uri.empty())
        break;
      publishDiagnosticsNow(job);
    }
  };
  auto runFastDiagnosticsWorker = [&]() {
    while (true) {
      PendingDiagnosticsJob job = takeNextReadyDiagnosticsJobForPriority(
          DiagnosticsQueuePriority::Fast);
      if (job.uri.empty())
        break;
      publishDiagnosticsNow(job);
    }
  };
  auto runFullDiagnosticsWorker = [&]() {
    while (true) {
      PendingDiagnosticsJob job = takeNextReadyDiagnosticsJobForPriority(
          DiagnosticsQueuePriority::Full);
      if (job.uri.empty())
        break;
      publishDiagnosticsNow(job);
    }
  };

  size_t diagnosticsWorkerCountSnapshot = 2;
  {
    std::lock_guard<std::mutex> lock(coreMutex);
    if (core.diagnosticsAutoWorkerCount) {
      const size_t hardwareWorkers =
          std::max<size_t>(1, std::thread::hardware_concurrency());
      size_t suggested = std::max<size_t>(1, hardwareWorkers / 2);
      if (hardwareWorkers >= 4)
        suggested = std::max<size_t>(2, suggested);
      diagnosticsWorkerCountSnapshot = std::min<size_t>(suggested, 8);
    } else {
      diagnosticsWorkerCountSnapshot =
          core.diagnosticsWorkerCount < 1
              ? static_cast<size_t>(1)
              : static_cast<size_t>(core.diagnosticsWorkerCount);
    }
  }
  std::vector<std::thread> diagnosticsWorkers;
  diagnosticsWorkers.reserve(diagnosticsWorkerCountSnapshot);
  if (diagnosticsWorkerCountSnapshot <= 1) {
    diagnosticsWorkers.emplace_back(runMixedDiagnosticsWorker);
  } else {
    diagnosticsWorkers.emplace_back(runFastDiagnosticsWorker);
    for (size_t worker = 1; worker < diagnosticsWorkerCountSnapshot; worker++) {
      diagnosticsWorkers.emplace_back(runFullDiagnosticsWorker);
    }
  }

  auto stopDiagnosticsThread = [&]() {
    {
      std::lock_guard<std::mutex> lock(diagnosticsMutex);
      diagnosticsStopping = true;
      diagnosticsPendingFast.clear();
      diagnosticsPendingFull.clear();
      diagnosticsReady[0].clear();
      diagnosticsReady[1].clear();
    }
    diagnosticsCv.notify_all();
    if (diagnosticsSchedulerThread.joinable())
      diagnosticsSchedulerThread.join();
    for (auto &worker : diagnosticsWorkers) {
      if (worker.joinable())
        worker.join();
    }
  };

  bool metricsThreadStopping = false;
  std::mutex metricsThreadMutex;
  std::condition_variable metricsThreadCv;
  std::thread metricsThread([&]() {
    std::unique_lock<std::mutex> lock(metricsThreadMutex);
    while (!metricsThreadStopping) {
      metricsThreadCv.wait_for(lock, std::chrono::seconds(5),
                               [&]() { return metricsThreadStopping; });
      if (metricsThreadStopping)
        break;
      Json params = makeObject();
      Json methods = makeObject();
      DiagnosticsMetric diagnosticsSnapshot;
      size_t pendingFastSize = 0;
      size_t pendingFullSize = 0;
      size_t readyFastSize = 0;
      size_t readyFullSize = 0;
      FastAstMetricsSnapshot fastAstSnapshot;
      IncludeGraphCacheMetricsSnapshot includeGraphSnapshot;
      FullAstMetricsSnapshot fullAstSnapshot;
      SignatureHelpMetricsSnapshot signatureHelpSnapshot;
      {
        std::lock_guard<std::mutex> metricLock(metricsMutex);
        for (const auto &entry : methodMetrics) {
          Json item = makeObject();
          item.o["count"] = makeNumber(static_cast<double>(entry.second.count));
          item.o["cancelled"] =
              makeNumber(static_cast<double>(entry.second.cancelled));
          item.o["failed"] =
              makeNumber(static_cast<double>(entry.second.failed));
          item.o["avgMs"] = makeNumber(
              entry.second.count > 0 ? (entry.second.totalMs /
                                        static_cast<double>(entry.second.count))
                                     : 0.0);
          item.o["maxMs"] = makeNumber(entry.second.maxMs);
          methods.o[entry.first] = std::move(item);
        }
        diagnosticsSnapshot = diagnosticsMetrics;
        methodMetrics.clear();
        diagnosticsMetrics = DiagnosticsMetric{};
      }
      {
        std::lock_guard<std::mutex> diagnosticsLock(diagnosticsMutex);
        pendingFastSize = diagnosticsPendingFast.size();
        pendingFullSize = diagnosticsPendingFull.size();
        readyFastSize = diagnosticsReady[0].size();
        readyFullSize = diagnosticsReady[1].size();
      }
      fastAstSnapshot = takeFastAstMetricsSnapshot();
      includeGraphSnapshot = takeIncludeGraphCacheMetricsSnapshot();
      fullAstSnapshot = takeFullAstMetricsSnapshot();
      signatureHelpSnapshot = takeSignatureHelpMetricsSnapshot();
      SemanticCacheMetricsSnapshot semanticCacheSnapshot =
          takeSemanticCacheMetricsSnapshot();
      diagnosticsSnapshot.semanticCacheSnapshotHit +=
          semanticCacheSnapshot.snapshotHit;
      diagnosticsSnapshot.semanticCacheSnapshotMiss +=
          semanticCacheSnapshot.snapshotMiss;
      Json diagnostics = makeObject();
      diagnostics.o["count"] =
          makeNumber(static_cast<double>(diagnosticsSnapshot.count));
      diagnostics.o["truncated"] =
          makeNumber(static_cast<double>(diagnosticsSnapshot.truncated));
      diagnostics.o["timedOut"] =
          makeNumber(static_cast<double>(diagnosticsSnapshot.timedOut));
      diagnostics.o["heavyRulesSkipped"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.heavyRulesSkipped));
      diagnostics.o["indeterminateTotal"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.indeterminateTotal));
      Json diagnosticsIndeterminateReasons = makeObject();
      diagnosticsIndeterminateReasons
          .o[IndeterminateReason::DiagnosticsRhsTypeEmpty] =
          makeNumber(static_cast<double>(
              diagnosticsSnapshot.indeterminateReasonRhsTypeEmpty));
      diagnosticsIndeterminateReasons
          .o[IndeterminateReason::DiagnosticsBudgetTimeout] =
          makeNumber(static_cast<double>(
              diagnosticsSnapshot.indeterminateReasonBudgetTimeout));
      diagnosticsIndeterminateReasons
          .o[IndeterminateReason::DiagnosticsHeavyRulesSkipped] =
          makeNumber(static_cast<double>(
              diagnosticsSnapshot.indeterminateReasonHeavyRulesSkipped));
      diagnostics.o["indeterminateReasons"] = diagnosticsIndeterminateReasons;
      diagnostics.o["semanticCacheSnapshotHit"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.semanticCacheSnapshotHit));
      diagnostics.o["semanticCacheSnapshotMiss"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.semanticCacheSnapshotMiss));
      diagnostics.o["staleDroppedBeforeBuild"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.staleDroppedBeforeBuild));
      diagnostics.o["staleDroppedBeforePublish"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.staleDroppedBeforePublish));
      diagnostics.o["canceledInPending"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.canceledInPending));
      diagnostics.o["queueWaitAvgMs"] = makeNumber(
          diagnosticsSnapshot.queueWaitSamples > 0
              ? (diagnosticsSnapshot.queueWaitTotalMs /
                 static_cast<double>(diagnosticsSnapshot.queueWaitSamples))
              : 0.0);
      diagnostics.o["queueWaitMaxMs"] =
          makeNumber(diagnosticsSnapshot.queueWaitMaxMs);
      diagnostics.o["queueMaxPendingTotal"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.queueMaxPendingTotal));
      diagnostics.o["queueMaxReadyTotal"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.queueMaxReadyTotal));
      diagnostics.o["avgMs"] =
          makeNumber(diagnosticsSnapshot.count > 0
                         ? (diagnosticsSnapshot.totalMs /
                            static_cast<double>(diagnosticsSnapshot.count))
                         : 0.0);
      diagnostics.o["maxMs"] = makeNumber(diagnosticsSnapshot.maxMs);
      Json diagnosticsQueue = makeObject();
      diagnosticsQueue.o["pendingFast"] =
          makeNumber(static_cast<double>(pendingFastSize));
      diagnosticsQueue.o["pendingFull"] =
          makeNumber(static_cast<double>(pendingFullSize));
      diagnosticsQueue.o["pendingTotal"] =
          makeNumber(static_cast<double>(pendingFastSize + pendingFullSize));
      diagnosticsQueue.o["readyFast"] =
          makeNumber(static_cast<double>(readyFastSize));
      diagnosticsQueue.o["readyFull"] =
          makeNumber(static_cast<double>(readyFullSize));
      diagnosticsQueue.o["readyTotal"] =
          makeNumber(static_cast<double>(readyFastSize + readyFullSize));
      Json fastAst = makeObject();
      fastAst.o["lookups"] =
          makeNumber(static_cast<double>(fastAstSnapshot.lookups));
      fastAst.o["cacheHits"] =
          makeNumber(static_cast<double>(fastAstSnapshot.cacheHits));
      fastAst.o["cacheReused"] =
          makeNumber(static_cast<double>(fastAstSnapshot.cacheReused));
      fastAst.o["rebuilds"] =
          makeNumber(static_cast<double>(fastAstSnapshot.rebuilds));
      fastAst.o["functionsIndexed"] =
          makeNumber(static_cast<double>(fastAstSnapshot.functionsIndexed));
      Json includeGraphCache = makeObject();
      includeGraphCache.o["lookups"] =
          makeNumber(static_cast<double>(includeGraphSnapshot.lookups));
      includeGraphCache.o["cacheHits"] =
          makeNumber(static_cast<double>(includeGraphSnapshot.cacheHits));
      includeGraphCache.o["rebuilds"] =
          makeNumber(static_cast<double>(includeGraphSnapshot.rebuilds));
      includeGraphCache.o["invalidations"] =
          makeNumber(static_cast<double>(includeGraphSnapshot.invalidations));
      Json fullAst = makeObject();
      fullAst.o["lookups"] =
          makeNumber(static_cast<double>(fullAstSnapshot.lookups));
      fullAst.o["cacheHits"] =
          makeNumber(static_cast<double>(fullAstSnapshot.cacheHits));
      fullAst.o["rebuilds"] =
          makeNumber(static_cast<double>(fullAstSnapshot.rebuilds));
      fullAst.o["invalidations"] =
          makeNumber(static_cast<double>(fullAstSnapshot.invalidations));
      fullAst.o["functionsIndexed"] =
          makeNumber(static_cast<double>(fullAstSnapshot.functionsIndexed));
      fullAst.o["includesIndexed"] =
          makeNumber(static_cast<double>(fullAstSnapshot.includesIndexed));
      fullAst.o["documentsCached"] =
          makeNumber(static_cast<double>(fullAstSnapshot.documentsCached));
      Json signatureHelp = makeObject();
      signatureHelp.o["indeterminateTotal"] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.indeterminateTotal));
      Json signatureHelpIndeterminateReasons = makeObject();
      signatureHelpIndeterminateReasons
          .o[IndeterminateReason::SignatureHelpCallTargetUnknown] =
          makeNumber(static_cast<double>(
              signatureHelpSnapshot.indeterminateReasonCallTargetUnknown));
      signatureHelpIndeterminateReasons
          .o[IndeterminateReason::SignatureHelpDefinitionTextUnavailable] =
          makeNumber(static_cast<double>(
              signatureHelpSnapshot
                  .indeterminateReasonDefinitionTextUnavailable));
      signatureHelpIndeterminateReasons
          .o[IndeterminateReason::SignatureHelpSignatureExtractFailed] =
          makeNumber(static_cast<double>(
              signatureHelpSnapshot.indeterminateReasonSignatureExtractFailed));
      signatureHelpIndeterminateReasons
          .o[IndeterminateReason::SignatureHelpOther] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.indeterminateReasonOther));
      signatureHelp.o["indeterminateReasons"] =
          signatureHelpIndeterminateReasons;
      Json overloadResolverMetrics = makeObject();
      overloadResolverMetrics.o["attempts"] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.overloadResolverAttempts));
      overloadResolverMetrics.o["resolved"] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.overloadResolverResolved));
      overloadResolverMetrics.o["ambiguous"] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.overloadResolverAmbiguous));
      overloadResolverMetrics.o["noViable"] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.overloadResolverNoViable));
      overloadResolverMetrics.o["shadowMismatch"] =
          makeNumber(static_cast<double>(
              signatureHelpSnapshot.overloadResolverShadowMismatch));
      signatureHelp.o["overloadResolver"] = overloadResolverMetrics;
      params.o["methods"] = methods;
      params.o["diagnostics"] = diagnostics;
      params.o["diagnosticsQueue"] = diagnosticsQueue;
      params.o["fastAst"] = fastAst;
      params.o["includeGraphCache"] = includeGraphCache;
      params.o["fullAst"] = fullAst;
      params.o["signatureHelp"] = signatureHelp;
      writeNotification("nsf/metrics", params);
    }
  });

  auto stopMetricsThread = [&]() {
    {
      std::lock_guard<std::mutex> lock(metricsThreadMutex);
      metricsThreadStopping = true;
    }
    metricsThreadCv.notify_one();
    if (metricsThread.joinable())
      metricsThread.join();
  };

  enum class RequestPriority : int { P0 = 0, P1 = 1, P2 = 2, P3 = 3 };

  struct QueuedRequest {
    std::string method;
    Json id;
    Json params;
    bool hasId = false;
    RequestPriority priority = RequestPriority::P2;
    std::string inlayUri;
    uint64_t inlayEpoch = 0;
  };

  std::mutex queueMutex;
  std::condition_variable queueCv;
  bool requestWorkersStopping = false;
  std::deque<QueuedRequest> requestQueues[4];
  std::unordered_set<std::string> canceledStringIds;
  std::unordered_set<int64_t> canceledNumericIds;
  std::unordered_map<std::string, uint64_t> inlayLatestEpochByUri;

  auto markRequestCanceled = [&](const Json &requestId) {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (requestId.type == Json::Type::String && !requestId.s.empty()) {
      canceledStringIds.insert(requestId.s);
      return;
    }
    if (requestId.type == Json::Type::Number) {
      canceledNumericIds.insert(
          static_cast<int64_t>(std::llround(getNumberValue(requestId))));
    }
  };

  auto clearRequestCancelState = [&](const Json &requestId) {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (requestId.type == Json::Type::String && !requestId.s.empty()) {
      canceledStringIds.erase(requestId.s);
      return;
    }
    if (requestId.type == Json::Type::Number) {
      canceledNumericIds.erase(
          static_cast<int64_t>(std::llround(getNumberValue(requestId))));
    }
  };

  auto isRequestCanceled = [&](const QueuedRequest &request) -> bool {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (request.hasId) {
      if (request.id.type == Json::Type::String && !request.id.s.empty() &&
          canceledStringIds.find(request.id.s) != canceledStringIds.end()) {
        return true;
      }
      if (request.id.type == Json::Type::Number &&
          canceledNumericIds.find(static_cast<int64_t>(std::llround(
              getNumberValue(request.id)))) != canceledNumericIds.end()) {
        return true;
      }
    }
    if (!request.inlayUri.empty() && request.inlayEpoch > 0) {
      auto it = inlayLatestEpochByUri.find(request.inlayUri);
      if (it != inlayLatestEpochByUri.end() && it->second != request.inlayEpoch)
        return true;
    }
    return false;
  };

  auto priorityForMethod = [&](const std::string &method) {
    if (method == "textDocument/hover" || method == "textDocument/definition" ||
        method == "textDocument/signatureHelp") {
      return RequestPriority::P1;
    }
    if (method == "textDocument/completion") {
      return RequestPriority::P2;
    }
    if (method == "textDocument/inlayHint" ||
        method == "textDocument/semanticTokens/full" ||
        method == "textDocument/semanticTokens/range") {
      return RequestPriority::P3;
    }
    return RequestPriority::P2;
  };

  auto enqueueRequest = [&](QueuedRequest request) {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (request.method == "textDocument/inlayHint" &&
        !request.inlayUri.empty()) {
      request.inlayEpoch = ++inlayLatestEpochByUri[request.inlayUri];
      auto &q = requestQueues[static_cast<int>(RequestPriority::P3)];
      for (auto it = q.begin(); it != q.end();) {
        if (it->method == "textDocument/inlayHint" &&
            it->inlayUri == request.inlayUri) {
          if (it->hasId)
            writeError(it->id, -32800, "Request cancelled");
          if (it->hasId) {
            if (it->id.type == Json::Type::String && !it->id.s.empty()) {
              canceledStringIds.erase(it->id.s);
            } else if (it->id.type == Json::Type::Number) {
              canceledNumericIds.erase(
                  static_cast<int64_t>(std::llround(getNumberValue(it->id))));
            }
          }
          it = q.erase(it);
        } else {
          ++it;
        }
      }
    }
    requestQueues[static_cast<int>(request.priority)].push_back(
        std::move(request));
    queueCv.notify_one();
  };

  auto takeNextRequest = [&]() -> QueuedRequest {
    std::unique_lock<std::mutex> lock(queueMutex);
    queueCv.wait(lock, [&]() {
      if (requestWorkersStopping)
        return true;
      for (const auto &q : requestQueues) {
        if (!q.empty())
          return true;
      }
      return false;
    });
    if (requestWorkersStopping) {
      bool empty = true;
      for (const auto &q : requestQueues) {
        if (!q.empty()) {
          empty = false;
          break;
        }
      }
      if (empty)
        return QueuedRequest{};
    }
    for (int i = 0; i < 4; i++) {
      if (!requestQueues[i].empty()) {
        QueuedRequest next = std::move(requestQueues[i].front());
        requestQueues[i].pop_front();
        return next;
      }
    }
    return QueuedRequest{};
  };

  auto runRequestWorker = [&]() {
    while (true) {
      QueuedRequest request = takeNextRequest();
      if (request.method.empty()) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (requestWorkersStopping)
          break;
        continue;
      }

      const auto requestStart = std::chrono::steady_clock::now();
      if (isRequestCanceled(request)) {
        if (request.hasId)
          writeError(request.id, -32800, "Request cancelled");
        if (request.hasId)
          clearRequestCancelState(request.id);
        recordMethodMetric(
            request.method,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - requestStart)
                .count(),
            true, false);
        continue;
      }

      ServerRequestContext requestCtx;
      {
        std::lock_guard<std::mutex> lock(coreMutex);
        requestCtx = core;
      }
      requestCtx.isCancellationRequested = [&, request]() {
        return isRequestCanceled(request);
      };
      const Json *paramsPtr =
          request.params.type == Json::Type::Null ? nullptr : &request.params;
      bool handled =
          handleCoreRequestMethods(request.method, request.id, paramsPtr,
                                   requestCtx, noLanguageItems,
                                   noLanguageItems);
      bool failed = false;
      if (!handled && request.hasId) {
        writeError(request.id, -32601, "Method not implemented");
        failed = true;
      }
      const bool canceled = isRequestCanceled(request);
      recordMethodMetric(request.method,
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - requestStart)
                             .count(),
                         canceled, failed);
      if (request.hasId)
        clearRequestCancelState(request.id);
    }
  };

  std::thread requestWorkerHigh(runRequestWorker);
  std::thread requestWorkerLow(runRequestWorker);

  auto stopRequestWorkers = [&]() {
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      requestWorkersStopping = true;
    }
    queueCv.notify_all();
    if (requestWorkerHigh.joinable())
      requestWorkerHigh.join();
    if (requestWorkerLow.joinable())
      requestWorkerLow.join();
  };

  while (true) {
    std::string payload;
    if (!readMessage(payload)) {
      break;
    }
    Json message;
    if (!parseJson(payload, message))
      continue;
    const Json *methodValue = getObjectValue(message, "method");
    std::string method = methodValue ? getStringValue(*methodValue) : "";
    const Json *idValue = getObjectValue(message, "id");
    Json id = idValue ? *idValue : makeNull();
    const Json *params = getObjectValue(message, "params");
    if (method.empty())
      continue;

    if (method == "initialize") {
      if (params) {
        const Json *initOptions =
            getObjectValue(*params, "initializationOptions");
        if (initOptions)
          applySettings(*initOptions);
        const Json *folders = getObjectValue(*params, "workspaceFolders");
        if (folders && folders->type == Json::Type::Array) {
          std::lock_guard<std::mutex> lock(coreMutex);
          core.workspaceFolders.clear();
          for (const auto &item : folders->a) {
            const Json *uriValue = getObjectValue(item, "uri");
            if (uriValue && uriValue->type == Json::Type::String)
              core.workspaceFolders.push_back(uriToPath(uriValue->s));
          }
        }
      }
      std::vector<std::string> workspaceFoldersSnapshot;
      std::vector<std::string> includePathsSnapshot;
      std::vector<std::string> shaderExtensionsSnapshot;
      SemanticTokenLegend semanticLegendSnapshot;
      {
        std::lock_guard<std::mutex> lock(coreMutex);
        workspaceFoldersSnapshot = core.workspaceFolders;
        includePathsSnapshot = core.includePaths;
        shaderExtensionsSnapshot = core.shaderExtensions;
        semanticLegendSnapshot = core.semanticLegend;
        workspaceIndexSetConcurrencyLimits(
            core.indexingWorkerCount <= 0
                ? static_cast<size_t>(1)
                : static_cast<size_t>(core.indexingWorkerCount),
            core.indexingQueueCapacity <= 0
                ? static_cast<size_t>(4096)
                : static_cast<size_t>(core.indexingQueueCapacity));
      }
      workspaceIndexConfigure(workspaceFoldersSnapshot, includePathsSnapshot,
                              shaderExtensionsSnapshot);
      Json completionProvider = makeObject();
      Json triggerChars = makeArray();
      for (const auto &ch :
           std::vector<std::string>{"#", "\"", "<", ".", "["}) {
        triggerChars.a.push_back(makeString(ch));
      }
      completionProvider.o["triggerCharacters"] = triggerChars;
      Json renameProvider = makeObject();
      renameProvider.o["prepareProvider"] = makeBool(true);
      Json semanticTokensProvider = makeObject();
      Json semanticLegendJson = makeObject();
      Json semanticTokenTypes = makeArray();
      for (const auto &t : semanticLegendSnapshot.tokenTypes) {
        semanticTokenTypes.a.push_back(makeString(t));
      }
      Json semanticTokenModifiers = makeArray();
      for (const auto &m : semanticLegendSnapshot.tokenModifiers) {
        semanticTokenModifiers.a.push_back(makeString(m));
      }
      semanticLegendJson.o["tokenTypes"] = semanticTokenTypes;
      semanticLegendJson.o["tokenModifiers"] = semanticTokenModifiers;
      semanticTokensProvider.o["legend"] = semanticLegendJson;
      semanticTokensProvider.o["full"] = makeBool(true);
      semanticTokensProvider.o["range"] = makeBool(true);
      Json signatureHelpProvider = makeObject();
      Json signatureTriggers = makeArray();
      for (const auto &ch : std::vector<std::string>{"(", ","}) {
        signatureTriggers.a.push_back(makeString(ch));
      }
      signatureHelpProvider.o["triggerCharacters"] = signatureTriggers;
      Json inlayHintProvider = makeBool(true);
      Json caps = makeObject();
      caps.o["textDocumentSync"] = makeNumber(2);
      caps.o["hoverProvider"] = makeBool(true);
      caps.o["definitionProvider"] = makeBool(true);
      caps.o["referencesProvider"] = makeBool(true);
      caps.o["documentSymbolProvider"] = makeBool(true);
      caps.o["renameProvider"] = renameProvider;
      caps.o["completionProvider"] = completionProvider;
      caps.o["semanticTokensProvider"] = semanticTokensProvider;
      caps.o["signatureHelpProvider"] = signatureHelpProvider;
      caps.o["inlayHintProvider"] = inlayHintProvider;
      Json result = makeObject();
      result.o["capabilities"] = caps;
      writeResponse(id, result);
      continue;
    }

    if (method == "workspace/didChangeConfiguration" && params) {
      const Json *settings = getObjectValue(*params, "settings");
      if (settings)
        applySettings(*settings);
      std::vector<std::string> workspaceFoldersSnapshot;
      std::vector<std::string> includePathsSnapshot;
      std::vector<std::string> shaderExtensionsSnapshot;
      std::vector<Document> documentSnapshot;
      {
        std::lock_guard<std::mutex> lock(coreMutex);
        core.scanDefinitionCache.clear();
        core.scanDefinitionMisses.clear();
        core.scanStructFieldsCache.clear();
        core.scanStructFieldsMisses.clear();
        core.scanCacheKey.clear();
        workspaceFoldersSnapshot = core.workspaceFolders;
        includePathsSnapshot = core.includePaths;
        shaderExtensionsSnapshot = core.shaderExtensions;
        workspaceIndexSetConcurrencyLimits(
            core.indexingWorkerCount <= 0
                ? static_cast<size_t>(1)
                : static_cast<size_t>(core.indexingWorkerCount),
            core.indexingQueueCapacity <= 0
                ? static_cast<size_t>(4096)
                : static_cast<size_t>(core.indexingQueueCapacity));
        for (const auto &entry : core.documents)
          documentSnapshot.push_back(entry.second);
      }
      invalidateAllIncludeGraphCaches();
      workspaceIndexConfigure(workspaceFoldersSnapshot, includePathsSnapshot,
                              shaderExtensionsSnapshot);
      namespace fs = std::filesystem;
      auto normalize = [](const std::string &value) -> std::string {
        std::string out;
        out.reserve(value.size());
        for (unsigned char ch : value) {
          char c = static_cast<char>(std::tolower(ch));
          if (c == '\\')
            c = '/';
          out.push_back(c);
        }
        while (out.size() > 1 && out.back() == '/')
          out.pop_back();
        return out;
      };
      auto isUnderOrEqual = [&](const std::string &dir,
                                const std::string &path) -> bool {
        if (dir.empty())
          return false;
        if (path.size() < dir.size())
          return false;
        if (path.rfind(dir, 0) != 0)
          return false;
        if (path.size() == dir.size())
          return true;
        return path[dir.size()] == '/';
      };

      const std::string unitPath = getActiveUnitPath();
      const std::string unitDir =
          unitPath.empty() ? std::string()
                           : fs::path(unitPath).parent_path().string();
      const std::string unitDirN = normalize(unitDir);
      for (const auto &entry : documentSnapshot) {
        std::string docPath = uriToPath(entry.uri);
        if (docPath.empty())
          docPath = entry.uri;
        const std::string docPathN = normalize(docPath);
        const bool unitRelated =
            !unitDirN.empty() && isUnderOrEqual(unitDirN, docPathN);
        const int delay = unitPath.empty() ? 250 : (unitRelated ? 120 : 650);
        scheduleDiagnosticsForText(entry, true, true, std::min(delay, 120),
                                   delay);
      }
      continue;
    }

    if (method == "nsf/getIndexingState") {
      writeResponse(id, workspaceIndexGetIndexingState());
      continue;
    }

    if (method == "nsf/kickIndexing") {
      std::string reason = "manual";
      if (params && params->type == Json::Type::Object) {
        const Json *reasonValue = getObjectValue(*params, "reason");
        if (reasonValue && reasonValue->type == Json::Type::String &&
            !reasonValue->s.empty()) {
          reason = reasonValue->s;
        }
      }
      workspaceIndexKickIndexing(reason);
      if (id.type != Json::Type::Null)
        writeResponse(id, makeNull());
      continue;
    }

    if (method == "nsf/rebuildIndex") {
      std::string reason = "manual";
      bool clearDiskCache = false;
      if (params && params->type == Json::Type::Object) {
        const Json *reasonValue = getObjectValue(*params, "reason");
        if (reasonValue && reasonValue->type == Json::Type::String &&
            !reasonValue->s.empty()) {
          reason = reasonValue->s;
        }
        const Json *clearValue = getObjectValue(*params, "clearDiskCache");
        if (clearValue)
          clearDiskCache = getBoolValue(*clearValue, false);
      }
      {
        std::lock_guard<std::mutex> lock(coreMutex);
        core.scanCacheKey.clear();
        core.scanDefinitionCache.clear();
        core.scanDefinitionMisses.clear();
        core.scanStructFieldsCache.clear();
        core.scanStructFieldsMisses.clear();
      }
      invalidateAllIncludeGraphCaches();
      invalidateAllFastAstCaches();
      invalidateAllFullAstCaches();
      semanticCacheInvalidateAll();
      workspaceIndexRebuild(reason, clearDiskCache);
      if (id.type != Json::Type::Null)
        writeResponse(id, makeNull());
      continue;
    }

    if (method == "workspace/didChangeWatchedFiles" && params) {
      {
        std::lock_guard<std::mutex> lock(coreMutex);
        core.scanDefinitionCache.clear();
        core.scanDefinitionMisses.clear();
        core.scanStructFieldsCache.clear();
        core.scanStructFieldsMisses.clear();
      }
      {
        namespace fs = std::filesystem;
        std::vector<std::string> changedUris;
        std::unordered_set<std::string> changedDocUris;
        std::unordered_set<std::string> changedPaths;
        std::unordered_set<std::string> includeImpactedPaths;
        std::vector<Document> documentSnapshot;
        auto normalize = [](const std::string &value) -> std::string {
          std::string out;
          out.reserve(value.size());
          for (unsigned char ch : value) {
            char c = static_cast<char>(std::tolower(ch));
            if (c == '\\')
              c = '/';
            out.push_back(c);
          }
          while (out.size() > 1 && out.back() == '/')
            out.pop_back();
          return out;
        };
        auto isUnderOrEqual = [&](const std::string &dir,
                                  const std::string &path) -> bool {
          if (dir.empty())
            return false;
          if (path.size() < dir.size())
            return false;
          if (path.rfind(dir, 0) != 0)
            return false;
          if (path.size() == dir.size())
            return true;
          return path[dir.size()] == '/';
        };
        const Json *changes = getObjectValue(*params, "changes");
        if (changes && changes->type == Json::Type::Array) {
          for (const auto &entry : changes->a) {
            if (entry.type != Json::Type::Object)
              continue;
            const Json *uriValue = getObjectValue(entry, "uri");
            if (uriValue && uriValue->type == Json::Type::String) {
              changedUris.push_back(uriValue->s);
              changedDocUris.insert(uriValue->s);
              std::string p = uriToPath(uriValue->s);
              if (p.empty())
                p = uriValue->s;
              changedPaths.insert(normalize(p));
            }
          }
        }
        if (!changedUris.empty())
          invalidateDocumentTextCacheByUris(changedUris);
        if (!changedUris.empty())
          invalidateFastAstByUris(changedUris);
        if (!changedUris.empty())
          invalidateFullAstByUris(changedUris);
        if (!changedUris.empty())
          invalidateIncludeGraphCacheByUris(changedUris);
        if (!changedUris.empty()) {
          workspaceIndexHandleFileChanges(changedUris);
        }
        if (!changedUris.empty()) {
          std::vector<std::string> impacted;
          workspaceIndexCollectReverseIncludeClosure(changedUris, impacted,
                                                     4096);
          for (const auto &p : impacted) {
            if (!p.empty())
              includeImpactedPaths.insert(p);
          }
        }

        const std::string unitPath = getActiveUnitPath();
        const std::string unitDir =
            unitPath.empty() ? std::string()
                             : fs::path(unitPath).parent_path().string();
        const std::string unitDirN = normalize(unitDir);
        {
          std::lock_guard<std::mutex> lock(coreMutex);
          for (const auto &entry : core.documents)
            documentSnapshot.push_back(entry.second);
        }

        const int delay = 180;
        const int followupDelay = 1200;
        const bool fallbackRefreshAll = !changedUris.empty();
        for (const auto &doc : documentSnapshot) {
          std::string docPath = uriToPath(doc.uri);
          if (docPath.empty())
            docPath = doc.uri;
          const std::string docPathN = normalize(docPath);
          const bool underUnit =
              !unitDirN.empty() && isUnderOrEqual(unitDirN, docPathN);
          const bool directlyChanged =
              changedDocUris.find(doc.uri) != changedDocUris.end() ||
              changedPaths.find(docPathN) != changedPaths.end();
          const bool includeImpacted =
              includeImpactedPaths.find(docPathN) != includeImpactedPaths.end();
          if (underUnit || directlyChanged || includeImpacted ||
              fallbackRefreshAll) {
            scheduleDiagnosticsForText(doc, true, false, delay);
            if (!changedUris.empty()) {
              scheduleDiagnosticsForText(doc, false, true, -1, followupDelay);
            }
          }
        }
      }
      continue;
    }

    if (method == "nsf/setActiveUnit" && params) {
      const Json *uriValue = getObjectValue(*params, "uri");
      const Json *pathValue = getObjectValue(*params, "path");
      std::string uri;
      std::string unitPath;
      if (uriValue && uriValue->type == Json::Type::String) {
        uri = uriValue->s;
        unitPath = uriToPath(uri);
      } else if (pathValue && pathValue->type == Json::Type::String) {
        unitPath = pathValue->s;
        uri = pathToUri(unitPath);
      }
      setActiveUnit(uri, unitPath);
      continue;
    }

    if (method == "nsf/_debugIncludeContextUnits" && params) {
      const Json *uriValue = getObjectValue(*params, "uri");
      Json result = makeArray();
      if (uriValue && uriValue->type == Json::Type::String) {
        std::vector<std::string> units;
        workspaceIndexCollectIncludingUnits({uriValue->s}, units, 256);
        for (const auto &unit : units)
          result.a.push_back(makeString(unit));
      }
      if (id.type != Json::Type::Null)
        writeResponse(id, result);
      continue;
    }

    if (method == "shutdown") {
      writeResponse(id, makeNull());
      continue;
    }

    if (method == "exit") {
      stopRequestWorkers();
      stopMetricsThread();
      stopDiagnosticsThread();
      workspaceIndexShutdown();
      return 0;
    }

    if (method == "textDocument/didOpen" && params) {
      const Json *textDocument = getObjectValue(*params, "textDocument");
      if (textDocument) {
        const Json *uriValue = getObjectValue(*textDocument, "uri");
        const Json *textValue = getObjectValue(*textDocument, "text");
        const Json *versionValue = getObjectValue(*textDocument, "version");
        if (uriValue && textValue && uriValue->type == Json::Type::String &&
            textValue->type == Json::Type::String) {
          std::string normalized = normalizeDocumentText(textValue->s);
          int version = 0;
          if (versionValue && versionValue->type == Json::Type::Number)
            version =
                static_cast<int>(std::llround(getNumberValue(*versionValue)));
          Document document;
          document.uri = uriValue->s;
          document.text = std::move(normalized);
          document.version = version;
          {
            std::lock_guard<std::mutex> lock(coreMutex);
            document.epoch = ++documentEpochCounter;
            core.documents[uriValue->s] = document;
          }
          primeDocumentTextCache(document.uri, document.text);
          invalidateFullAstByUri(document.uri);
          invalidateIncludeGraphCacheByUri(document.uri);
          scheduleDiagnosticsForText(document);
        }
      }
      continue;
    }

    if (method == "textDocument/didChange" && params) {
      const Json *textDocument = getObjectValue(*params, "textDocument");
      const Json *changes = getObjectValue(*params, "contentChanges");
      if (textDocument && changes && changes->type == Json::Type::Array &&
          !changes->a.empty()) {
        const Json *uriValue = getObjectValue(*textDocument, "uri");
        const Json *versionValue = getObjectValue(*textDocument, "version");
        if (uriValue && uriValue->type == Json::Type::String) {
          std::string text;
          int oldVersion = 0;
          {
            std::lock_guard<std::mutex> lock(coreMutex);
            auto itDoc = core.documents.find(uriValue->s);
            if (itDoc == core.documents.end()) {
              Document created;
              created.uri = uriValue->s;
              created.epoch = ++documentEpochCounter;
              core.documents[uriValue->s] = created;
              itDoc = core.documents.find(uriValue->s);
            }
            text = itDoc->second.text;
            oldVersion = itDoc->second.version;
          }

          auto parsePosition = [&](const Json &pos, int &lineOut,
                                   int &chOut) -> bool {
            if (pos.type != Json::Type::Object)
              return false;
            const Json *lineV = getObjectValue(pos, "line");
            const Json *chV = getObjectValue(pos, "character");
            if (!lineV || !chV || lineV->type != Json::Type::Number ||
                chV->type != Json::Type::Number)
              return false;
            lineOut = static_cast<int>(getNumberValue(*lineV));
            chOut = static_cast<int>(getNumberValue(*chV));
            return true;
          };

          for (const auto &change : changes->a) {
            const Json *textValue = getObjectValue(change, "text");
            if (!textValue || textValue->type != Json::Type::String)
              continue;
            const std::string newText = normalizeDocumentText(textValue->s);

            const Json *rangeValue = getObjectValue(change, "range");
            if (!rangeValue || rangeValue->type == Json::Type::Null) {
              text = newText;
              continue;
            }
            if (rangeValue->type != Json::Type::Object) {
              text = newText;
              continue;
            }
            const Json *startValue = getObjectValue(*rangeValue, "start");
            const Json *endValue = getObjectValue(*rangeValue, "end");
            int startLine = 0;
            int startCh = 0;
            int endLine = 0;
            int endCh = 0;
            if (!startValue || !endValue ||
                !parsePosition(*startValue, startLine, startCh) ||
                !parsePosition(*endValue, endLine, endCh)) {
              text = newText;
              continue;
            }
            const size_t startOffset =
                positionToOffsetUtf16(text, startLine, startCh);
            const size_t endOffset =
                positionToOffsetUtf16(text, endLine, endCh);
            const size_t lo = std::min(startOffset, endOffset);
            const size_t hi = std::max(startOffset, endOffset);
            std::string updated;
            updated.reserve(text.size() - (hi - lo) + newText.size());
            updated.append(text, 0, lo);
            updated.append(newText);
            if (hi < text.size())
              updated.append(text, hi, std::string::npos);
            text = std::move(updated);
          }

          int nextVersion = oldVersion + 1;
          if (versionValue && versionValue->type == Json::Type::Number) {
            nextVersion =
                static_cast<int>(std::llround(getNumberValue(*versionValue)));
          }
          Document updatedDocument;
          {
            std::lock_guard<std::mutex> lock(coreMutex);
            Document &doc = core.documents[uriValue->s];
            doc.uri = uriValue->s;
            doc.text = text;
            doc.version = nextVersion;
            doc.epoch = ++documentEpochCounter;
            updatedDocument = doc;
          }
          primeDocumentTextCache(updatedDocument.uri, updatedDocument.text);
          invalidateFullAstByUri(updatedDocument.uri);
          invalidateIncludeGraphCacheByUri(updatedDocument.uri);
          scheduleDiagnosticsForText(updatedDocument, true, false, 60);
          scheduleDiagnosticsForText(updatedDocument, false, true, -1, 520);
        }
      }
      continue;
    }

    if (method == "textDocument/didClose" && params) {
      const Json *textDocument = getObjectValue(*params, "textDocument");
      if (textDocument) {
        const Json *uriValue = getObjectValue(*textDocument, "uri");
        if (uriValue && uriValue->type == Json::Type::String) {
          {
            std::lock_guard<std::mutex> lock(coreMutex);
            core.documents.erase(uriValue->s);
          }
          invalidateDocumentTextCacheByUri(uriValue->s);
          invalidateFastAstByUri(uriValue->s);
          invalidateFullAstByUri(uriValue->s);
          invalidateIncludeGraphCacheByUri(uriValue->s);
          cancelDiagnosticsAndPublishEmpty(uriValue->s);
        }
      }
      continue;
    }
    if (method == "$/cancelRequest" && params) {
      const Json *requestId = getObjectValue(*params, "id");
      if (requestId)
        markRequestCanceled(*requestId);
      continue;
    }

    if (idValue) {
      QueuedRequest request;
      request.method = method;
      request.id = id;
      request.hasId = true;
      request.priority = priorityForMethod(method);
      request.params = params ? *params : makeNull();
      if (method == "textDocument/inlayHint" && params) {
        const Json *textDocument = getObjectValue(*params, "textDocument");
        if (textDocument) {
          const Json *uriValue = getObjectValue(*textDocument, "uri");
          if (uriValue && uriValue->type == Json::Type::String)
            request.inlayUri = uriValue->s;
        }
      }
      enqueueRequest(std::move(request));
    }
  }
  stopRequestWorkers();
  stopMetricsThread();
  stopDiagnosticsThread();
  workspaceIndexShutdown();
  return 0;
}
