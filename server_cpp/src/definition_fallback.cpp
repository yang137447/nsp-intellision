#include "definition_fallback.hpp"

#include "active_unit.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "nsf_lexer.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

static void addUniqueRoot(std::vector<std::string> &roots,
                          const std::string &value) {
  if (value.empty())
    return;
  if (std::find(roots.begin(), roots.end(), value) == roots.end())
    roots.push_back(value);
}

static bool hasExtension(const std::filesystem::path &path,
                         const std::vector<std::string> &extensions) {
  using CharT = std::filesystem::path::value_type;
  auto lower = [](CharT ch) -> CharT {
    if constexpr (std::is_same_v<CharT, wchar_t>) {
      return static_cast<CharT>(std::towlower(ch));
    } else {
      return static_cast<CharT>(std::tolower(static_cast<unsigned char>(ch)));
    }
  };
  auto equalsIgnoreCase = [&](const CharT *lhs, const CharT *rhs) -> bool {
    if (!lhs || !rhs)
      return false;
    size_t i = 0;
    while (lhs[i] != 0 && rhs[i] != 0) {
      if (lower(lhs[i]) != lower(rhs[i]))
        return false;
      i++;
    }
    return lhs[i] == 0 && rhs[i] == 0;
  };

  std::filesystem::path extPath = path.extension();
  const CharT *ext = extPath.c_str();
  if (!ext || ext[0] == 0)
    return false;

  for (const auto &candidate : extensions) {
    std::filesystem::path candPath(candidate);
    if (equalsIgnoreCase(ext, candPath.c_str()))
      return true;
  }
  return false;
}

static std::string pathToPortableString(const std::filesystem::path &path) {
  try {
#ifdef _WIN32
    const auto raw = path.u8string();
    return std::string(raw.begin(), raw.end());
#else
    return path.string();
#endif
  } catch (const std::exception &) {
    return std::string();
  } catch (...) {
    return std::string();
  }
}

static bool isIdentifierBoundary(char ch) {
  return !(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_');
}

static bool findWordToken(const std::string &line, const std::string &token,
                          size_t &posOut) {
  if (token.empty())
    return false;
  size_t pos = line.find(token);
  while (pos != std::string::npos) {
    bool leftOk = (pos == 0) || isIdentifierBoundary(line[pos - 1]);
    bool rightOk = (pos + token.size() >= line.size()) ||
                   isIdentifierBoundary(line[pos + token.size()]);
    if (leftOk && rightOk) {
      posOut = pos;
      return true;
    }
    pos = line.find(token, pos + token.size());
  }
  return false;
}

static std::atomic<int> gIndexingTokenCounter{0};

static void sendIndexingEvent(const std::string &state, int token,
                              const std::string &kind,
                              const std::string &symbol, size_t visited,
                              size_t fileBudget) {
  Json params = makeObject();
  params.o["state"] = makeString(state);
  params.o["token"] = makeNumber(token);
  params.o["kind"] = makeString(kind);
  const std::string unitPath = getActiveUnitPath();
  if (!unitPath.empty())
    params.o["unit"] = makeString(unitPath);
  if (!symbol.empty())
    params.o["symbol"] = makeString(symbol);
  if (visited > 0)
    params.o["visited"] = makeNumber(static_cast<double>(visited));
  if (fileBudget > 0)
    params.o["fileBudget"] = makeNumber(static_cast<double>(fileBudget));
  writeNotification("nsf/indexing", params);
}

struct IndexingScope {
  int token = 0;
  std::string kind;
  std::string symbol;
  size_t fileBudget = 0;
  std::chrono::steady_clock::time_point lastUpdate;

  IndexingScope(const std::string &kindIn, const std::string &symbolIn,
                size_t fileBudgetIn)
      : token(++gIndexingTokenCounter), kind(kindIn), symbol(symbolIn),
        fileBudget(fileBudgetIn), lastUpdate(std::chrono::steady_clock::now()) {
    sendIndexingEvent("begin", token, kind, symbol, 0, fileBudget);
  }

  void update(size_t visited) {
    const auto now = std::chrono::steady_clock::now();
    if (now - lastUpdate < std::chrono::milliseconds(150))
      return;
    lastUpdate = now;
    sendIndexingEvent("update", token, kind, symbol, visited, fileBudget);
  }

  ~IndexingScope() {
    sendIndexingEvent("end", token, kind, symbol, 0, fileBudget);
  }
};

static bool findStructLikeDefinitionInFile(const std::filesystem::path &path,
                                           const std::string &symbol,
                                           int &lineOut, int &startOut,
                                           int &endOut) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::string line;
  int lineIndex = 0;
  const std::string targets[] = {"struct", "cbuffer", "class", "typedef"};
  while (std::getline(stream, line)) {
    std::string trimmed = trimLeftCopy(line);
    if (trimmed.rfind("//", 0) == 0) {
      lineIndex++;
      continue;
    }
    bool matchesPrefix = false;
    for (const auto &kw : targets) {
      if (trimmed.rfind(kw, 0) == 0) {
        matchesPrefix = true;
        break;
      }
    }
    if (!matchesPrefix) {
      lineIndex++;
      continue;
    }
    size_t pos = std::string::npos;
    if (findWordToken(line, symbol, pos)) {
      lineOut = lineIndex;
      startOut = byteOffsetInLineToUtf16(line, static_cast<int>(pos));
      endOut = startOut + static_cast<int>(symbol.size());
      return true;
    }
    lineIndex++;
  }
  return false;
}

static bool
findFunctionOrVariableDefinitionInFile(const std::filesystem::path &path,
                                       const std::string &symbol, int &lineOut,
                                       int &startOut, int &endOut) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::string line;
  int lineIndex = 0;
  int braceDepth = 0;
  bool pendingCbuffer = false;
  bool inCbuffer = false;
  int cbufferBraceDepth = 0;
  bool inBlockComment = false;
  bool pendingUiVar = false;
  int pendingUiVarLine = -1;
  int pendingUiVarStart = -1;
  int pendingUiVarEnd = -1;
  std::string pendingUiVarLineText;
  while (std::getline(stream, line)) {
    std::string trimmed = trimLeftCopy(line);
    if (trimmed.rfind("//", 0) == 0) {
      lineIndex++;
      continue;
    }

    if (inBlockComment) {
      size_t end = line.find("*/");
      if (end != std::string::npos) {
        inBlockComment = false;
        std::string tail = line.substr(end + 2);
        line = tail;
        trimmed = trimLeftCopy(line);
      } else {
        lineIndex++;
        continue;
      }
    }
    size_t blockStart = line.find("/*");
    if (blockStart != std::string::npos) {
      size_t blockEnd = line.find("*/", blockStart + 2);
      if (blockEnd == std::string::npos) {
        inBlockComment = true;
        line = line.substr(0, blockStart);
        trimmed = trimLeftCopy(line);
      } else {
        std::string head = line.substr(0, blockStart);
        std::string tail = line.substr(blockEnd + 2);
        line = head + tail;
        trimmed = trimLeftCopy(line);
      }
    }

    if (!inCbuffer && braceDepth == 0 && trimmed.rfind("cbuffer", 0) == 0) {
      pendingCbuffer = true;
    }

    if (inCbuffer && cbufferBraceDepth == 1 &&
        line.find(symbol) != std::string::npos) {
      const auto tokens = lexLineTokens(line);
      for (size_t i = 0; i < tokens.size(); i++) {
        const auto &token = tokens[i];
        if (token.kind != LexToken::Kind::Identifier || token.text != symbol)
          continue;
        if (i > 0) {
          const auto &prev = tokens[i - 1];
          if (prev.kind == LexToken::Kind::Punct &&
              (prev.text == "." || prev.text == "->" || prev.text == "::"))
            continue;
        }
        lineOut = lineIndex;
        startOut = byteOffsetInLineToUtf16(line, static_cast<int>(token.start));
        endOut = byteOffsetInLineToUtf16(line, static_cast<int>(token.end));
        return true;
      }
    }

    if (braceDepth == 0) {
      std::string trimmedRight = trimmed;
      while (!trimmedRight.empty() &&
             std::isspace(static_cast<unsigned char>(trimmedRight.back()))) {
        trimmedRight.pop_back();
      }
      if (pendingUiVar) {
        if (trimmedRight == "<") {
          lineOut = pendingUiVarLine;
          startOut =
              byteOffsetInLineToUtf16(pendingUiVarLineText, pendingUiVarStart);
          endOut =
              byteOffsetInLineToUtf16(pendingUiVarLineText, pendingUiVarEnd);
          return true;
        }
        if (!trimmedRight.empty() && trimmedRight.rfind("//", 0) != 0) {
          pendingUiVar = false;
          pendingUiVarLine = -1;
          pendingUiVarStart = -1;
          pendingUiVarEnd = -1;
          pendingUiVarLineText.clear();
        }
      }

      if (trimmed.rfind("#define", 0) == 0) {
        size_t pos = std::string::npos;
        if (findWordToken(line, symbol, pos)) {
          lineOut = lineIndex;
          startOut = byteOffsetInLineToUtf16(line, static_cast<int>(pos));
          endOut = startOut + static_cast<int>(symbol.size());
          return true;
        }
      }

      const auto tokens = lexLineTokens(line);
      if (!pendingUiVar && tokens.size() >= 2 && tokens[0].text != "#") {
        size_t typeIndex = std::string::npos;
        for (size_t t = 0; t < tokens.size(); t++) {
          if (tokens[t].kind != LexToken::Kind::Identifier)
            continue;
          if (isQualifierToken(tokens[t].text))
            continue;
          if (tokens[t].text == "return" || tokens[t].text == "if" ||
              tokens[t].text == "for" || tokens[t].text == "while" ||
              tokens[t].text == "switch" || tokens[t].text == "struct" ||
              tokens[t].text == "cbuffer")
            break;
          typeIndex = t;
          break;
        }
        if (typeIndex != std::string::npos) {
          size_t nameIndex = std::string::npos;
          for (size_t t = typeIndex + 1; t < tokens.size(); t++) {
            if (tokens[t].kind != LexToken::Kind::Identifier)
              continue;
            if (isQualifierToken(tokens[t].text))
              continue;
            nameIndex = t;
            break;
          }
          if (nameIndex != std::string::npos &&
              tokens[nameIndex].text == symbol) {
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
              pendingUiVar = true;
              pendingUiVarLine = lineIndex;
              pendingUiVarStart = static_cast<int>(tokens[nameIndex].start);
              pendingUiVarEnd = static_cast<int>(tokens[nameIndex].end);
              pendingUiVarLineText = line;
            }
          }
        }
      }

      if (tokens.size() >= 2 && tokens[0].text != "#") {
        auto isBlockType = [](const std::string &t) {
          return t == "texture" || t == "Texture" || t == "Texture2D" ||
                 t == "Texture3D" || t == "TextureCube" ||
                 t == "SamplerState" || t == "SamplerComparisonState" ||
                 t == "BlendState" || t == "DepthStencilState" ||
                 t == "RasterizerState";
        };
        size_t typeIndex = std::string::npos;
        for (size_t t = 0; t < tokens.size(); t++) {
          if (tokens[t].kind != LexToken::Kind::Identifier)
            continue;
          if (isQualifierToken(tokens[t].text))
            continue;
          typeIndex = t;
          break;
        }
        if (typeIndex != std::string::npos &&
            isBlockType(tokens[typeIndex].text)) {
          size_t nameIndex = std::string::npos;
          for (size_t t = typeIndex + 1; t < tokens.size(); t++) {
            if (tokens[t].kind == LexToken::Kind::Identifier &&
                !isQualifierToken(tokens[t].text)) {
              nameIndex = t;
              break;
            }
          }
          if (nameIndex != std::string::npos &&
              tokens[nameIndex].kind == LexToken::Kind::Identifier &&
              tokens[nameIndex].text == symbol) {
            bool hasAssignBefore = false;
            for (size_t j = 0; j < nameIndex; j++) {
              if (tokens[j].kind == LexToken::Kind::Punct &&
                  tokens[j].text == "=") {
                hasAssignBefore = true;
                break;
              }
            }
            if (!hasAssignBefore) {
              lineOut = lineIndex;
              startOut = byteOffsetInLineToUtf16(
                  line, static_cast<int>(tokens[nameIndex].start));
              endOut = byteOffsetInLineToUtf16(
                  line, static_cast<int>(tokens[nameIndex].end));
              return true;
            }
          }
        }
      }
      for (size_t i = 0; i < tokens.size(); i++) {
        const auto &token = tokens[i];
        if (token.kind != LexToken::Kind::Identifier || token.text != symbol)
          continue;
        if (i > 0) {
          const auto &prev = tokens[i - 1];
          if (prev.kind == LexToken::Kind::Punct &&
              (prev.text == "." || prev.text == "->" || prev.text == "::"))
            continue;
        }
        bool hasAssignBefore = false;
        for (size_t j = 0; j < i; j++) {
          if (tokens[j].kind == LexToken::Kind::Punct &&
              tokens[j].text == "=") {
            hasAssignBefore = true;
            break;
          }
        }
        if (hasAssignBefore)
          continue;

        bool isFunction = false;
        if (i + 1 < tokens.size() &&
            tokens[i + 1].kind == LexToken::Kind::Punct &&
            tokens[i + 1].text == "(") {
          isFunction = true;
        }

        if (isFunction) {
          int parenDepth = 0;
          bool sawClose = false;
          size_t closeIndex = 0;
          for (size_t j = i + 1; j < tokens.size(); j++) {
            if (tokens[j].kind == LexToken::Kind::Punct) {
              if (tokens[j].text == "(") {
                parenDepth++;
              } else if (tokens[j].text == ")") {
                parenDepth--;
                if (parenDepth == 0) {
                  sawClose = true;
                  closeIndex = j;
                  break;
                }
              }
            }
          }
          if (sawClose) {
            for (size_t k = closeIndex + 1; k < tokens.size(); k++) {
              if (tokens[k].kind == LexToken::Kind::Punct) {
                if (tokens[k].text == ":" || tokens[k].text == "{" ||
                    tokens[k].text == ";") {
                  lineOut = lineIndex;
                  startOut = byteOffsetInLineToUtf16(
                      line, static_cast<int>(token.start));
                  endOut = byteOffsetInLineToUtf16(line,
                                                   static_cast<int>(token.end));
                  return true;
                }
              }
            }
          }
          lineIndex++;
          continue;
        }

        bool hasSemi = false;
        for (const auto &t : tokens) {
          if (t.kind == LexToken::Kind::Punct && t.text == ";") {
            hasSemi = true;
            break;
          }
        }
        if (!hasSemi) {
          lineIndex++;
          continue;
        }

        size_t typeIndex = std::string::npos;
        for (size_t t = 0; t < tokens.size(); t++) {
          if (tokens[t].kind != LexToken::Kind::Identifier)
            continue;
          if (isQualifierToken(tokens[t].text))
            continue;
          if (tokens[t].text == "return" || tokens[t].text == "if" ||
              tokens[t].text == "for" || tokens[t].text == "while" ||
              tokens[t].text == "switch")
            break;
          typeIndex = t;
          break;
        }
        if (typeIndex == std::string::npos) {
          lineIndex++;
          continue;
        }

        size_t j = typeIndex + 1;
        while (j < tokens.size()) {
          if (tokens[j].kind == LexToken::Kind::Identifier) {
            if (isQualifierToken(tokens[j].text)) {
              j++;
              continue;
            }
            if (tokens[j].text == symbol) {
              lineOut = lineIndex;
              startOut =
                  byteOffsetInLineToUtf16(line, static_cast<int>(token.start));
              endOut =
                  byteOffsetInLineToUtf16(line, static_cast<int>(token.end));
              return true;
            }
            j++;
            while (j < tokens.size() &&
                   !(tokens[j].kind == LexToken::Kind::Punct &&
                     (tokens[j].text == "," || tokens[j].text == ";"))) {
              j++;
            }
            if (j < tokens.size() && tokens[j].kind == LexToken::Kind::Punct &&
                tokens[j].text == ",") {
              j++;
              continue;
            }
            break;
          }
          if (tokens[j].kind == LexToken::Kind::Punct && tokens[j].text == ";")
            break;
          j++;
        }
      }
    }

    bool inLineComment = false;
    for (size_t c = 0; c < line.size(); c++) {
      if (!inLineComment && c + 1 < line.size() && line[c] == '/' &&
          line[c + 1] == '/') {
        inLineComment = true;
      }
      if (inLineComment)
        break;
      if (line[c] == '{') {
        braceDepth++;
        if (pendingCbuffer) {
          inCbuffer = true;
          cbufferBraceDepth = 1;
          pendingCbuffer = false;
        } else if (inCbuffer) {
          cbufferBraceDepth++;
        }
      } else if (line[c] == '}') {
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
  return false;
}

bool findDefinitionByWorkspaceScan(const std::string &symbol,
                                   const std::vector<std::string> &roots,
                                   const std::vector<std::string> &extensions,
                                   DefinitionLocation &outLocation) {
  return findDefinitionByWorkspaceScan(symbol, roots, extensions, outLocation,
                                       true);
}

bool findDefinitionByWorkspaceScan(const std::string &symbol,
                                   const std::vector<std::string> &roots,
                                   const std::vector<std::string> &extensions,
                                   DefinitionLocation &outLocation,
                                   bool consultIndex) {
  if (consultIndex && workspaceIndexFindDefinition(symbol, outLocation))
    return true;
  namespace fs = std::filesystem;
  auto startTime = std::chrono::steady_clock::now();
  const auto timeBudget = std::chrono::milliseconds(1200);
  const size_t fileBudget = 3500;
  size_t visitedFiles = 0;
  IndexingScope indexing("workspaceScan", symbol, fileBudget);

  std::vector<std::string> orderedRoots;
  const std::string unitPath = getActiveUnitPath();
  if (!unitPath.empty())
    addUniqueRoot(orderedRoots, unitPath);
  for (const auto &root : roots) {
    if (!root.empty())
      addUniqueRoot(orderedRoots, root);
  }

  for (const auto &root : orderedRoots) {
    if (root.empty())
      continue;
    std::error_code ec;
    fs::path base(root);
    if (!fs::exists(base, ec))
      continue;
    if (fs::is_regular_file(base, ec)) {
      if (!hasExtension(base, extensions))
        continue;
      int line = -1;
      int start = -1;
      int end = -1;
      if (findStructLikeDefinitionInFile(base, symbol, line, start, end) ||
          findFunctionOrVariableDefinitionInFile(base, symbol, line, start,
                                                 end)) {
        const std::string filePath = pathToPortableString(base);
        if (filePath.empty())
          continue;
        outLocation.uri = pathToUri(filePath);
        outLocation.line = line;
        outLocation.start = start;
        outLocation.end = end;
        return true;
      }
      continue;
    }
    if (!fs::is_directory(base, ec))
      continue;

    fs::recursive_directory_iterator it(
        base, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator endIt;
    for (; it != endIt && !ec; it.increment(ec)) {
      if (visitedFiles >= fileBudget)
        return false;
      auto elapsed = std::chrono::steady_clock::now() - startTime;
      if (elapsed > timeBudget)
        return false;

      if (!it->is_regular_file(ec))
        continue;
      const fs::path filePath = it->path();
      if (!hasExtension(filePath, extensions))
        continue;

      visitedFiles++;
      indexing.update(visitedFiles);
      int line = -1;
      int start = -1;
      int end = -1;
      if (findStructLikeDefinitionInFile(filePath, symbol, line, start, end) ||
          findFunctionOrVariableDefinitionInFile(filePath, symbol, line, start,
                                                 end)) {
        const std::string hitPath = pathToPortableString(filePath);
        if (hitPath.empty())
          continue;
        outLocation.uri = pathToUri(hitPath);
        outLocation.line = line;
        outLocation.start = start;
        outLocation.end = end;
        return true;
      }
    }
  }

  return false;
}

bool findStructDefinitionByWorkspaceScan(
    const std::string &symbol, const std::vector<std::string> &roots,
    const std::vector<std::string> &extensions,
    DefinitionLocation &outLocation) {
  if (workspaceIndexFindStructDefinition(symbol, outLocation))
    return true;
  namespace fs = std::filesystem;
  auto startTime = std::chrono::steady_clock::now();
  const auto timeBudget = std::chrono::milliseconds(1200);
  const size_t fileBudget = 3500;
  size_t visitedFiles = 0;
  IndexingScope indexing("structScan", symbol, fileBudget);

  std::vector<std::string> orderedRoots;
  const std::string unitPath = getActiveUnitPath();
  if (!unitPath.empty())
    addUniqueRoot(orderedRoots, unitPath);
  for (const auto &root : roots) {
    if (!root.empty())
      addUniqueRoot(orderedRoots, root);
  }

  for (const auto &root : orderedRoots) {
    if (root.empty())
      continue;
    std::error_code ec;
    fs::path base(root);
    if (!fs::exists(base, ec))
      continue;
    if (fs::is_regular_file(base, ec)) {
      if (!hasExtension(base, extensions))
        continue;
      int line = -1;
      int start = -1;
      int end = -1;
      if (findStructLikeDefinitionInFile(base, symbol, line, start, end)) {
        const std::string filePath = pathToPortableString(base);
        if (filePath.empty())
          continue;
        outLocation.uri = pathToUri(filePath);
        outLocation.line = line;
        outLocation.start = start;
        outLocation.end = end;
        return true;
      }
      continue;
    }
    if (!fs::is_directory(base, ec))
      continue;

    fs::recursive_directory_iterator it(
        base, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator endIt;
    for (; it != endIt && !ec; it.increment(ec)) {
      if (visitedFiles >= fileBudget)
        return false;
      auto elapsed = std::chrono::steady_clock::now() - startTime;
      if (elapsed > timeBudget)
        return false;

      if (!it->is_regular_file(ec))
        continue;
      const fs::path filePath = it->path();
      if (!hasExtension(filePath, extensions))
        continue;

      visitedFiles++;
      indexing.update(visitedFiles);
      int line = -1;
      int start = -1;
      int end = -1;
      if (findStructLikeDefinitionInFile(filePath, symbol, line, start, end)) {
        const std::string hitPath = pathToPortableString(filePath);
        if (hitPath.empty())
          continue;
        outLocation.uri = pathToUri(hitPath);
        outLocation.line = line;
        outLocation.start = start;
        outLocation.end = end;
        return true;
      }
    }
  }

  return false;
}
