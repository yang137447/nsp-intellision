#include "main_occurrence_helpers.hpp"

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
#include "definition_location.hpp"
#include "declaration_query.hpp"
#include "deferred_doc_runtime.hpp"
#include "diagnostics.hpp"
#include "document_owner.hpp"
#include "document_runtime.hpp"
#include "expanded_source.hpp"
#include "fast_ast.hpp"
#include "full_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_docs.hpp"
#include "include_resolver.hpp"
#include "immediate_syntax_diagnostics.hpp"
#include "indeterminate_reasons.hpp"
#include "interactive_semantic_runtime.hpp"
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
#include "workspace_summary_runtime.hpp"


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

bool findParameterDeclarationInText(const std::string &text,
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

bool findFxBlockDeclarationInLine(const std::string &line,
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

bool findNamedDefinitionInText(const std::string &text,
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
      std::string uiType;
      std::string uiName;
      size_t uiStart = 0;
      size_t uiEnd = 0;
      if (findMetadataDeclarationHeaderPosShared(scanLine, uiType, uiName,
                                                 uiStart, uiEnd)) {
        pendingUiVarName = uiName;
        pendingUiVarLine = scanIndex;
        pendingUiVarStart = uiStart;
        pendingUiVarLineText = scanLine;
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

bool findNamedDefinitionInText(const std::string &text,
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

static std::string normalizeOccurrenceComparablePath(std::string value) {
  value = std::filesystem::path(value).lexically_normal().string();
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   char c = static_cast<char>(std::tolower(ch));
                   return c == '\\' ? '/' : c;
                 });
  while (value.size() > 1 && value.back() == '/')
    value.pop_back();
  return value;
}

static void appendUniqueOccurrence(
    std::vector<LocatedOccurrence> &locations,
    std::unordered_set<std::string> &seenKeys,
    const LocatedOccurrence &occurrence) {
  std::string occurrencePath = uriToPath(occurrence.uri);
  if (occurrencePath.empty())
    occurrencePath = occurrence.uri;
  const std::string key =
      normalizeOccurrenceComparablePath(occurrencePath) + "|" +
      std::to_string(occurrence.line) + "|" +
      std::to_string(occurrence.start) + "|" + std::to_string(occurrence.end);
  if (!seenKeys.insert(key).second)
    return;
  locations.push_back(occurrence);
}

static std::vector<LocatedOccurrence> collectOccurrencesInIncludeGraph(
    const std::string &unitPathOrUri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines) {
  (void)workspaceFolders;
  (void)includePaths;
  (void)shaderExtensions;
  std::vector<std::string> orderedPaths;
  workspaceSummaryRuntimeCollectIncludeClosureForUnit(unitPathOrUri, orderedPaths,
                                                      1024);
  std::vector<std::string> orderedUris;
  orderedUris.reserve(orderedPaths.size());
  for (const auto &path : orderedPaths) {
    if (!path.empty())
      orderedUris.push_back(pathToUri(path));
  }
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

static bool pathHasNsfExtensionForOccurrences(const std::string &path) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return ext == ".nsf";
}

static std::string resolveOccurrenceRootUnitPath(const std::string &uri) {
  const std::string activeUnitPath = getActiveUnitPath();
  if (!activeUnitPath.empty())
    return activeUnitPath;
  std::string currentPath = uriToPath(uri);
  if (currentPath.empty())
    return std::string();
  if (pathHasNsfExtensionForOccurrences(currentPath))
    return currentPath;
  std::vector<std::string> candidateUnits;
  workspaceSummaryRuntimeCollectIncludingUnits({uri}, candidateUnits, 2);
  if (candidateUnits.size() == 1)
    return candidateUnits.front();
  return std::string();
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

  std::vector<LocatedOccurrence> familyLocations;
  DeclCandidate localDecl;
  if (findBestDeclarationUpTo(currentText, word, currentText.size(), localDecl) &&
      localDecl.found) {
    DefinitionLocation localDefinition;
    localDefinition.uri = uri;
    localDefinition.line = localDecl.line;
    localDefinition.start = byteOffsetInLineToUtf16(
        localDecl.lineText, static_cast<int>(localDecl.nameBytePos));
    localDefinition.end = byteOffsetInLineToUtf16(
        localDecl.lineText,
        static_cast<int>(localDecl.nameBytePos + word.size()));
    collectConditionalFamilyOccurrencesInSingleDocument(
        uri, currentText, word, defines, localDefinition, familyLocations);
  }
  DefinitionLocation definition;
  if (familyLocations.empty() &&
      workspaceSummaryRuntimeFindDefinition(word, definition)) {
    std::string definitionText;
    if (loadDocumentText(definition.uri, documents, definitionText)) {
      collectConditionalFamilyOccurrencesInSingleDocument(
          definition.uri, definitionText, word, defines, definition,
          familyLocations);
    }
  }

  const std::string rootUnitPath = resolveOccurrenceRootUnitPath(uri);
  if (!rootUnitPath.empty()) {
    auto locations = collectOccurrencesInIncludeGraph(
        rootUnitPath, word, documents, workspaceFolders, includePaths,
        shaderExtensions, defines);
    if (!locations.empty()) {
      if (!familyLocations.empty()) {
        std::vector<LocatedOccurrence> merged;
        std::unordered_set<std::string> seenKeys;
        merged.reserve(locations.size() + familyLocations.size());
        for (const auto &occ : locations)
          appendUniqueOccurrence(merged, seenKeys, occ);
        for (const auto &occ : familyLocations)
          appendUniqueOccurrence(merged, seenKeys, occ);
        return merged;
      }
      return locations;
    }
  }

  if (!familyLocations.empty()) {
    return familyLocations;
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
