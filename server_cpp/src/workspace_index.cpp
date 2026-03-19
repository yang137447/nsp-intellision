#include "workspace_index.hpp"

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

static bool readFileToString(const std::string &path, std::string &out);

static std::string toLowerCopy(const std::string &value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

static std::string normalizePathForCompare(const std::string &value) {
  fs::path path(value);
  std::string normalized = path.lexically_normal().string();
  if (normalized.empty())
    normalized = value;
  std::string out;
  out.reserve(normalized.size());
  for (unsigned char ch : normalized) {
    char c = static_cast<char>(std::tolower(ch));
    if (c == '\\')
      c = '/';
    out.push_back(c);
  }
  while (out.size() > 1 && out.back() == '/')
    out.pop_back();
  return out;
}

static bool isPathUnderOrEqual(const std::string &dir,
                               const std::string &path) {
  if (dir.empty())
    return false;
  if (path.size() < dir.size())
    return false;
  if (path.rfind(dir, 0) != 0)
    return false;
  if (path.size() == dir.size())
    return true;
  return path[dir.size()] == '/';
}

static bool extractIncludePathOutsideComments(const std::string &line,
                                              bool &inBlockCommentInOut,
                                              std::string &outIncludePath) {
  outIncludePath.clear();
  bool inString = false;
  bool escape = false;
  for (size_t i = 0; i < line.size();) {
    const char ch = line[i];
    const char next = (i + 1 < line.size()) ? line[i + 1] : '\0';

    if (inBlockCommentInOut) {
      if (ch == '*' && next == '/') {
        inBlockCommentInOut = false;
        i += 2;
        continue;
      }
      i++;
      continue;
    }

    if (!inString && ch == '/' && next == '*') {
      inBlockCommentInOut = true;
      i += 2;
      continue;
    }
    if (!inString && ch == '/' && next == '/') {
      break;
    }

    if (ch == '"' && !escape) {
      inString = !inString;
      i++;
      continue;
    }
    if (inString) {
      escape = (!escape && ch == '\\');
      i++;
      continue;
    }
    escape = false;

    if (ch != '#') {
      i++;
      continue;
    }

    size_t j = i + 1;
    while (j < line.size() &&
           std::isspace(static_cast<unsigned char>(line[j]))) {
      j++;
    }
    if (j + 7 > line.size() || line.compare(j, 7, "include") != 0) {
      i++;
      continue;
    }
    j += 7;
    while (j < line.size() &&
           std::isspace(static_cast<unsigned char>(line[j]))) {
      j++;
    }
    if (j >= line.size())
      return false;

    const char opener = line[j];
    const char closer = opener == '<' ? '>' : (opener == '"' ? '"' : '\0');
    if (closer == '\0')
      return false;
    j++;
    const size_t start = j;
    while (j < line.size() && line[j] != closer) {
      j++;
    }
    if (j >= line.size())
      return false;
    outIncludePath = line.substr(start, j - start);
    return !outIncludePath.empty();
  }
  return false;
}

static void
collectIncludeClosureFiles(const std::string &entryPath,
                           const std::vector<std::string> &workspaceFolders,
                           const std::vector<std::string> &includePaths,
                           const std::vector<std::string> &shaderExtensions,
                           std::vector<std::string> &outPaths) {
  std::error_code ec;
  fs::path start(entryPath);
  if (entryPath.empty() || !fs::exists(start, ec) ||
      !fs::is_regular_file(start, ec))
    return;

  const auto timeBudget = std::chrono::milliseconds(250);
  const size_t fileBudget = 512;
  const auto startTime = std::chrono::steady_clock::now();

  std::vector<std::string> stack;
  const std::string startPath = start.lexically_normal().string();
  stack.push_back(startPath);

  std::unordered_set<std::string> visited;
  visited.insert(normalizePathForCompare(startPath));

  while (!stack.empty()) {
    const std::string currentPath = std::move(stack.back());
    stack.pop_back();
    outPaths.push_back(currentPath);

    if (outPaths.size() >= fileBudget)
      break;
    if (std::chrono::steady_clock::now() - startTime > timeBudget)
      break;

    std::ifstream stream(currentPath, std::ios::binary);
    if (!stream)
      continue;
    std::string line;
    const std::string currentUri = pathToUri(currentPath);
    bool inBlockComment = false;
    while (std::getline(stream, line)) {
      if (outPaths.size() >= fileBudget)
        break;
      if (std::chrono::steady_clock::now() - startTime > timeBudget)
        break;
      std::string includePath;
      if (!extractIncludePathOutsideComments(line, inBlockComment, includePath))
        continue;
      auto candidates =
          resolveIncludeCandidates(currentUri, includePath, workspaceFolders,
                                   includePaths, shaderExtensions);
      for (const auto &candidate : candidates) {
        fs::path p(candidate);
        if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec))
          continue;
        const std::string normalizedCandidate = p.lexically_normal().string();
        const std::string key = normalizePathForCompare(normalizedCandidate);
        if (visited.find(key) != visited.end())
          break;
        visited.insert(key);
        stack.push_back(normalizedCandidate);
        break;
      }
    }
  }
}

static bool isAbsolutePath(const std::string &path) {
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':')
    return true;
  return path.rfind("\\\\", 0) == 0 || path.rfind("/", 0) == 0;
}

static std::string joinPath(const std::string &base, const std::string &child) {
  if (base.empty())
    return child;
  char sep = '\\';
  if (base.back() == '/' || base.back() == '\\')
    return base + child;
  return base + sep + child;
}

static void addUnique(std::vector<std::string> &items,
                      const std::string &value) {
  for (const auto &item : items) {
    if (item == value)
      return;
  }
  items.push_back(value);
}

static std::string computeIndexKey(const std::vector<std::string> &folders,
                                   const std::vector<std::string> &includePaths,
                                   const std::vector<std::string> &extensions,
                                   const std::string &modelsHash) {
  std::string key;
  auto appendList = [&](const char *tag,
                        const std::vector<std::string> &items) {
    key.append(tag);
    key.push_back(':');
    for (const auto &v : items) {
      key.append(toLowerCopy(v));
      key.push_back('|');
    }
    key.push_back(';');
  };
  appendList("folders", folders);
  appendList("includes", includePaths);
  appendList("exts", extensions);
  key.append("models:");
  key.append(modelsHash);
  key.push_back(';');
  key.append("includeParser:3;");
  return key;
}

static fs::path
indexCacheDir(const std::vector<std::string> &workspaceFolders) {
  if (!workspaceFolders.empty()) {
    fs::path p = fs::path(workspaceFolders[0]) / ".vscode" / "nsp";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
  }
  fs::path p = fs::current_path() / "server_cpp" / ".cache" / "nsf";
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

static uint64_t fnv1a64(const std::string &text) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char ch : text) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

static std::string hex16(uint64_t value) {
  static const char *digits = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = digits[value & 0xF];
    value >>= 4;
  }
  return out;
}

static uint64_t fnv1a64Append(uint64_t hash, const std::string &text) {
  for (unsigned char ch : text) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

static bool normalizeJsonTextForHash(const std::string &text,
                                     std::string &normalizedOut) {
  Json parsed;
  if (!parseJson(text, parsed))
    return false;
  normalizedOut.clear();
  normalizedOut.reserve(text.size());
  bool inString = false;
  bool escaped = false;
  for (char ch : text) {
    if (inString) {
      normalizedOut.push_back(ch);
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        inString = false;
      }
      continue;
    }
    if (ch == '"') {
      inString = true;
      escaped = false;
      normalizedOut.push_back(ch);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)))
      continue;
    normalizedOut.push_back(ch);
  }
  return true;
}

static bool computeModelsHash(std::string &hashOut, std::string &errorOut) {
  const fs::path resourcesDir = getExecutableDir() / "resources";
  const std::vector<std::string> relativePaths = {
      "builtins/intrinsics/base.json",
      "builtins/intrinsics/override.json",
      "methods/object_methods/base.json",
      "methods/object_methods/override.json",
      "language/keywords/base.json",
      "language/keywords/override.json",
      "types/object_types/base.json",
      "types/object_types/override.json",
      "types/object_families/base.json",
      "types/object_families/override.json",
      "types/type_overrides/base.json",
      "types/type_overrides/override.json",
  };

  uint64_t hash = 1469598103934665603ull;
  for (const auto &rel : relativePaths) {
    const fs::path targetPath = resourcesDir / fs::path(rel);
    std::string text;
    if (!readFileToString(targetPath.string(), text)) {
      errorOut = "model hash read failed: " + targetPath.string();
      return false;
    }
    std::string normalized;
    if (!normalizeJsonTextForHash(text, normalized)) {
      errorOut = "model hash parse failed: " + targetPath.string();
      return false;
    }
    hash = fnv1a64Append(hash, rel);
    hash = fnv1a64Append(hash, "\n");
    hash = fnv1a64Append(hash, normalized);
    hash = fnv1a64Append(hash, "\n");
  }

  hashOut = hex16(hash);
  errorOut.clear();
  return true;
}

static bool getModelsHash(std::string &hashOut, std::string &errorOut) {
  static std::once_flag once;
  static bool ok = false;
  static std::string cachedHash;
  static std::string cachedError;
  std::call_once(once, []() {
    ok = computeModelsHash(cachedHash, cachedError);
  });
  if (!ok) {
    hashOut.clear();
    errorOut = cachedError;
    return false;
  }
  hashOut = cachedHash;
  errorOut.clear();
  return true;
}

static fs::path
indexV1LegacyCachePath(const std::vector<std::string> &workspaceFolders) {
  return indexCacheDir(workspaceFolders) / "index_v1.json";
}

static fs::path
indexV1CachePathForKey(const std::vector<std::string> &workspaceFolders,
                       const std::string &key) {
  const std::string name = "index_v1_" + hex16(fnv1a64(key)) + ".json";
  return indexCacheDir(workspaceFolders) / name;
}

static fs::path indexV2DirPath(const std::vector<std::string> &workspaceFolders,
                               const std::string &key) {
  return indexCacheDir(workspaceFolders) / ("index_v2_" + hex16(fnv1a64(key)));
}

static fs::path indexV2FilesDir(const fs::path &dir) { return dir / "files"; }

static fs::path indexV2ManifestPath(const fs::path &dir) {
  return dir / "manifest.json";
}

static std::string indexV2FileShardNameForPath(const std::string &path) {
  const std::string key = normalizePathForCompare(path);
  return hex16(fnv1a64(key)) + ".json";
}

static void ensureIndexV2Dirs(const fs::path &dir) {
  std::error_code ec;
  fs::create_directories(indexV2FilesDir(dir), ec);
}

static void sendIndexingEvent(const std::string &state, int token,
                              const std::string &kind, size_t visited,
                              size_t total,
                              const std::string &phase = std::string()) {
  Json params = makeObject();
  params.o["state"] = makeString(state);
  params.o["token"] = makeNumber(token);
  params.o["kind"] = makeString(kind);
  if (!phase.empty())
    params.o["phase"] = makeString(phase);
  const std::string unitPath = getActiveUnitPath();
  if (!unitPath.empty())
    params.o["unit"] = makeString(unitPath);
  if (visited > 0)
    params.o["visited"] = makeNumber(static_cast<double>(visited));
  if (total > 0)
    params.o["fileBudget"] = makeNumber(static_cast<double>(total));
  writeNotification("nsf/indexing", params);
}

static int64_t nowUnixMs() {
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

static size_t defaultWorkerCount() { return static_cast<size_t>(2); }

struct FileMeta {
  uint64_t mtime = 0;
  uint64_t size = 0;
  std::vector<IndexedDefinition> defs;
  std::vector<IndexedStruct> structs;
  std::vector<std::string> includes;
};

struct IndexStore {
  std::string key;
  std::unordered_map<std::string, FileMeta> filesByPath;
  std::unordered_map<std::string, std::vector<IndexedDefinition>> defsBySymbol;
  std::unordered_map<std::string, std::vector<IndexedStructMember>>
      structMembersOrdered;
  std::unordered_map<std::string, DefinitionLocation> bestDefBySymbol;
  std::unordered_map<std::string, DefinitionLocation> bestStructDefByName;
  std::unordered_map<std::string, std::string> bestTypeBySymbol;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      structMemberType;
};

static bool hasExtension(const fs::path &path,
                         const std::unordered_set<std::string> &extensions) {
  std::string ext = toLowerCopy(path.extension().string());
  return extensions.find(ext) != extensions.end();
}

static bool buildCodeMaskForLine(const std::string &lineText,
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

static bool readFileToString(const std::string &path, std::string &out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  out = buffer.str();
  return true;
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
  while (std::getline(stream, lineText)) {
    std::string includePath;
    bool includeBlock = inBlockComment;
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

static void
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

static bool isGlobalDeclDisqualifier(const std::vector<LexToken> &tokens) {
  for (const auto &t : tokens) {
    if (t.kind != LexToken::Kind::Punct)
      continue;
    if (t.text == ";" || t.text == "=" || t.text == "(" || t.text == "{" ||
        t.text == "}" || t.text == "<" || t.text == ">")
      return true;
  }
  return false;
}

static void extractDefinitions(const std::string &uri, const std::string &text,
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

    if (pendingUiVar) {
      if (trimmedRight == "<") {
        record(pendingUiName, pendingUiType, 8, pendingUiStartByte,
               pendingUiEndByte, pendingUiLine);
        pendingUiVar = false;
      } else if (!trimmedRight.empty() && trimmedRight.rfind("//", 0) != 0) {
        pendingUiVar = false;
      }
    }

    if (!inCbuffer && braceDepth == 0 && trimmed.rfind("cbuffer", 0) == 0) {
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

      if (trimmed.rfind("struct", 0) == 0 || trimmed.rfind("cbuffer", 0) == 0 ||
          trimmed.rfind("technique", 0) == 0 || trimmed.rfind("pass", 0) == 0) {
        for (size_t i = 0; i + 1 < tokens.size(); i++) {
          if (tokens[i].kind == LexToken::Kind::Identifier &&
              (tokens[i].text == "struct" || tokens[i].text == "cbuffer" ||
               tokens[i].text == "technique" || tokens[i].text == "pass") &&
              tokens[i + 1].kind == LexToken::Kind::Identifier) {
            record(tokens[i + 1].text, "", 23,
                   static_cast<int>(tokens[i + 1].start),
                   static_cast<int>(tokens[i + 1].end), lineIndex);
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
            if (!isGlobalDeclDisqualifier(tokens)) {
              pendingUiVar = true;
              pendingUiLine = lineIndex;
              pendingUiStartByte = static_cast<int>(tokens[nameIndex].start);
              pendingUiEndByte = static_cast<int>(tokens[nameIndex].end);
              pendingUiName = name;
              pendingUiType = type;
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

static void rebuildGlobals(IndexStore &store) {
  store.defsBySymbol.clear();
  store.structMembersOrdered.clear();
  store.bestDefBySymbol.clear();
  store.bestStructDefByName.clear();
  store.bestTypeBySymbol.clear();
  store.structMemberType.clear();

  const std::string unitPathRaw = getActiveUnitPath();
  const std::string unitPath = normalizePathForCompare(unitPathRaw);
  const std::string unitDir = normalizePathForCompare(
      unitPathRaw.empty() ? std::string()
                          : fs::path(unitPathRaw).parent_path().string());

  std::vector<std::string> orderedFiles;
  orderedFiles.reserve(store.filesByPath.size());
  for (const auto &filePair : store.filesByPath)
    orderedFiles.push_back(filePair.first);

  std::sort(orderedFiles.begin(), orderedFiles.end(),
            [&](const std::string &a, const std::string &b) {
              const std::string an = normalizePathForCompare(a);
              const std::string bn = normalizePathForCompare(b);
              auto score = [&](const std::string &p) -> int {
                if (!unitPath.empty() && p == unitPath)
                  return 0;
                if (!unitDir.empty() && isPathUnderOrEqual(unitDir, p))
                  return 1;
                return 2;
              };
              const int sa = score(an);
              const int sb = score(bn);
              if (sa != sb)
                return sa < sb;
              return an < bn;
            });

  for (const auto &pathKey : orderedFiles) {
    const auto itFile = store.filesByPath.find(pathKey);
    if (itFile == store.filesByPath.end())
      continue;
    const auto &meta = itFile->second;
    for (const auto &def : meta.defs) {
      store.defsBySymbol[def.name].push_back(def);
      DefinitionLocation loc{def.uri, def.line, def.start, def.end};
      if (def.kind == 23) {
        auto it = store.bestStructDefByName.find(def.name);
        if (it == store.bestStructDefByName.end())
          store.bestStructDefByName.emplace(def.name, loc);
      }
      auto it = store.bestDefBySymbol.find(def.name);
      if (it == store.bestDefBySymbol.end())
        store.bestDefBySymbol.emplace(def.name, loc);
      if (!def.type.empty()) {
        auto tit = store.bestTypeBySymbol.find(def.name);
        if (tit == store.bestTypeBySymbol.end())
          store.bestTypeBySymbol.emplace(def.name, def.type);
      }
    }
    for (const auto &st : meta.structs) {
      auto itOrdered = store.structMembersOrdered.find(st.name);
      if (itOrdered == store.structMembersOrdered.end()) {
        std::vector<IndexedStructMember> ordered;
        ordered.reserve(st.members.size());
        for (const auto &m : st.members) {
          if (!m.name.empty())
            ordered.push_back(m);
        }
        if (!ordered.empty())
          store.structMembersOrdered.emplace(st.name, std::move(ordered));
      }
      auto &memberMap = store.structMemberType[st.name];
      for (const auto &m : st.members) {
        if (!m.name.empty() && !m.type.empty())
          memberMap.emplace(m.name, m.type);
      }
    }
  }
}

static void buildReverseIncludes(
    const IndexStore &store,
    std::unordered_map<std::string, std::vector<std::string>> &outReverse) {
  outReverse.clear();
  for (const auto &pair : store.filesByPath) {
    const std::string includerKey = normalizePathForCompare(pair.first);
    for (const auto &dep : pair.second.includes) {
      if (dep.empty())
        continue;
      outReverse[dep].push_back(includerKey);
    }
  }
}

static Json serializeFileEntry(const std::string &path, const FileMeta &meta) {
  Json file = makeObject();
  file.o["path"] = makeString(path);
  file.o["mtime"] = makeNumber(static_cast<double>(meta.mtime));
  file.o["size"] = makeNumber(static_cast<double>(meta.size));

  Json defs = makeArray();
  for (const auto &def : meta.defs) {
    Json d = makeObject();
    d.o["name"] = makeString(def.name);
    d.o["type"] = makeString(def.type);
    d.o["uri"] = makeString(def.uri);
    d.o["line"] = makeNumber(def.line);
    d.o["start"] = makeNumber(def.start);
    d.o["end"] = makeNumber(def.end);
    d.o["kind"] = makeNumber(def.kind);
    defs.a.push_back(std::move(d));
  }
  file.o["defs"] = std::move(defs);

  Json structs = makeArray();
  for (const auto &st : meta.structs) {
    Json s = makeObject();
    s.o["name"] = makeString(st.name);
    Json members = makeArray();
    for (const auto &m : st.members) {
      Json mem = makeObject();
      mem.o["name"] = makeString(m.name);
      mem.o["type"] = makeString(m.type);
      members.a.push_back(std::move(mem));
    }
    s.o["members"] = std::move(members);
    structs.a.push_back(std::move(s));
  }
  file.o["structs"] = std::move(structs);

  Json includes = makeArray();
  for (const auto &inc : meta.includes) {
    includes.a.push_back(makeString(inc));
  }
  file.o["includes"] = std::move(includes);
  return file;
}

static bool deserializeFileEntry(const Json &file, std::string &outPath,
                                 FileMeta &outMeta) {
  if (file.type != Json::Type::Object)
    return false;
  const Json *pathV = getObjectValue(file, "path");
  if (!pathV || pathV->type != Json::Type::String)
    return false;
  outPath = pathV->s;
  FileMeta meta;
  const Json *mtimeV = getObjectValue(file, "mtime");
  const Json *sizeV = getObjectValue(file, "size");
  if (mtimeV && mtimeV->type == Json::Type::Number)
    meta.mtime = static_cast<uint64_t>(mtimeV->n);
  if (sizeV && sizeV->type == Json::Type::Number)
    meta.size = static_cast<uint64_t>(sizeV->n);

  const Json *defsV = getObjectValue(file, "defs");
  if (defsV && defsV->type == Json::Type::Array) {
    for (const auto &d : defsV->a) {
      if (d.type != Json::Type::Object)
        continue;
      IndexedDefinition def;
      const Json *nameV2 = getObjectValue(d, "name");
      def.name = nameV2 ? getStringValue(*nameV2) : "";
      const Json *typeV2 = getObjectValue(d, "type");
      def.type = typeV2 ? getStringValue(*typeV2) : "";
      const Json *uriV2 = getObjectValue(d, "uri");
      def.uri = uriV2 ? getStringValue(*uriV2) : "";
      const Json *lineV = getObjectValue(d, "line");
      const Json *startV = getObjectValue(d, "start");
      const Json *endV = getObjectValue(d, "end");
      const Json *kindV = getObjectValue(d, "kind");
      def.line = lineV ? static_cast<int>(getNumberValue(*lineV)) : 0;
      def.start = startV ? static_cast<int>(getNumberValue(*startV)) : 0;
      def.end = endV ? static_cast<int>(getNumberValue(*endV)) : def.start;
      def.kind = kindV ? static_cast<int>(getNumberValue(*kindV)) : 0;
      if (!def.name.empty() && !def.uri.empty())
        meta.defs.push_back(std::move(def));
    }
  }

  const Json *structsV = getObjectValue(file, "structs");
  if (structsV && structsV->type == Json::Type::Array) {
    for (const auto &s : structsV->a) {
      if (s.type != Json::Type::Object)
        continue;
      IndexedStruct st;
      const Json *nameV = getObjectValue(s, "name");
      st.name = nameV ? getStringValue(*nameV) : "";
      const Json *membersV = getObjectValue(s, "members");
      if (membersV && membersV->type == Json::Type::Array) {
        for (const auto &m : membersV->a) {
          if (m.type != Json::Type::Object)
            continue;
          IndexedStructMember mem;
          const Json *mn = getObjectValue(m, "name");
          const Json *mt = getObjectValue(m, "type");
          mem.name = mn ? getStringValue(*mn) : "";
          mem.type = mt ? getStringValue(*mt) : "";
          if (!mem.name.empty())
            st.members.push_back(std::move(mem));
        }
      }
      if (!st.name.empty() && !st.members.empty())
        meta.structs.push_back(std::move(st));
    }
  }

  const Json *includesV = getObjectValue(file, "includes");
  if (includesV && includesV->type == Json::Type::Array) {
    for (const auto &inc : includesV->a) {
      if (inc.type != Json::Type::String)
        continue;
      if (!inc.s.empty())
        meta.includes.push_back(inc.s);
    }
  }

  outMeta = std::move(meta);
  return true;
}

static Json serializeIndex(const IndexStore &store) {
  Json root = makeObject();
  root.o["key"] = makeString(store.key);
  Json files = makeArray();
  for (const auto &pair : store.filesByPath) {
    files.a.push_back(serializeFileEntry(pair.first, pair.second));
  }
  root.o["files"] = std::move(files);
  return root;
}

static bool deserializeIndex(const Json &root, IndexStore &store) {
  if (root.type != Json::Type::Object)
    return false;
  const Json *key = getObjectValue(root, "key");
  const Json *files = getObjectValue(root, "files");
  if (!key || key->type != Json::Type::String)
    return false;
  if (!files || files->type != Json::Type::Array)
    return false;
  store.key = key->s;
  store.filesByPath.clear();

  for (const auto &file : files->a) {
    std::string path;
    FileMeta meta;
    if (!deserializeFileEntry(file, path, meta))
      continue;
    const std::string key = normalizePathForCompare(path);
    if (key.empty())
      continue;
    store.filesByPath[key] = std::move(meta);
  }

  rebuildGlobals(store);
  return true;
}

static uint64_t fileTimeToUInt(const fs::file_time_type &t) {
  using namespace std::chrono;
  auto s = duration_cast<seconds>(t.time_since_epoch());
  return static_cast<uint64_t>(s.count());
}

class WorkspaceIndex {
public:
  void configure(const std::vector<std::string> &workspaceFolders,
                 const std::vector<std::string> &includePaths,
                 const std::vector<std::string> &shaderExtensions) {
    std::string modelsHash;
    std::string modelsHashError;
    if (!getModelsHash(modelsHash, modelsHashError)) {
      bool shouldNotify = false;
      {
        std::lock_guard<std::mutex> lock(mutex);
        configured = false;
        configuredKey.clear();
        configuredWorkspaceFolders.clear();
        configuredRoots.clear();
        configuredIncludePaths.clear();
        configuredExtensions.clear();
        pendingRebuild = false;
        pendingChangedUris.clear();
        ready = false;
        indexingEpoch++;
        indexingState = "Error";
        indexingReason = "modelsHashUnavailable";
        progressPhase = "Error";
        progressVisited = 0;
        progressTotal = 0;
        pendingQueuedTasks = 0;
        pendingRunningWorkers = 0;
        pendingDirtyFiles = 0;
        pendingProbeRemainingBudget = 0;
        indexingUpdatedAtMs = nowUnixMs();
        activeModelsHash.clear();
        activeModelsHashError = modelsHashError;
        shouldNotify = true;
      }
      if (shouldNotify)
        publishIndexingStateChanged(true);
      return;
    }

    std::vector<std::string> roots;
    for (const auto &inc : includePaths) {
      if (inc.empty())
        continue;
      if (isAbsolutePath(inc)) {
        addUnique(roots, inc);
      } else {
        for (const auto &folder : workspaceFolders) {
          if (!folder.empty())
            addUnique(roots, joinPath(folder, inc));
        }
      }
    }
    std::vector<std::string> exts = shaderExtensions;
    addUnique(exts, ".hlsli");
    addUnique(exts, ".h");

    std::string newKey = computeIndexKey(roots, includePaths, exts, modelsHash);
    bool shouldNotify = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (configuredKey == newKey && configured)
        return;
      const bool hadConfigured = configured;
      configured = true;
      configuredKey = newKey;
      configuredWorkspaceFolders = workspaceFolders;
      configuredRoots = std::move(roots);
      configuredIncludePaths = includePaths;
      configuredExtensions = std::move(exts);
      pendingRebuild = true;
      pendingRebuildReason = hadConfigured ? "configChange" : "startup";
      pendingChangedUris.clear();
      ready = false;
      indexingEpoch++;
      indexingState = "Reindexing";
      indexingReason = pendingRebuildReason;
      progressPhase = "Building";
      progressVisited = 0;
      progressTotal = 0;
      pendingQueuedTasks = 1;
      pendingRunningWorkers = 0;
      pendingDirtyFiles = 0;
      pendingProbeRemainingBudget = 0;
      indexingUpdatedAtMs = nowUnixMs();
      activeModelsHash = modelsHash;
      activeModelsHashError.clear();
      shouldNotify = true;
    }
    cv.notify_one();
    ensureThread();
    if (shouldNotify)
      publishIndexingStateChanged(true);
  }

  void handleFileChanges(const std::vector<std::string> &uris) {
    bool shouldNotify = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      size_t appended = 0;
      for (const auto &u : uris) {
        if (!u.empty()) {
          pendingChangedUris.push_back(u);
          appended++;
        }
      }
      if (appended > 0) {
        pendingQueuedTasks = pendingChangedUris.size();
        if (indexingState == "Idle")
          indexingState = "Updating";
        indexingReason = "fileWatch";
        progressPhase = "Updating";
        progressVisited = 0;
        progressTotal = appended;
        indexingUpdatedAtMs = nowUnixMs();
        shouldNotify = true;
      }
    }
    cv.notify_one();
    ensureThread();
    if (shouldNotify)
      publishIndexingStateChanged(false);
  }

  bool findDefinition(const std::string &symbol, DefinitionLocation &out) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = store.bestDefBySymbol.find(symbol);
    if (it == store.bestDefBySymbol.end())
      return false;
    out = it->second;
    return true;
  }

  bool findStructDefinition(const std::string &symbol,
                            DefinitionLocation &out) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = store.bestStructDefByName.find(symbol);
    if (it == store.bestStructDefByName.end())
      return false;
    out = it->second;
    return true;
  }

  bool findDefinitions(const std::string &symbol,
                       std::vector<IndexedDefinition> &outDefs, size_t limit) {
    outDefs.clear();
    if (limit == 0)
      return false;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = store.defsBySymbol.find(symbol);
    if (it == store.defsBySymbol.end())
      return false;
    outDefs.reserve(std::min(limit, it->second.size()));
    for (size_t i = 0; i < it->second.size() && outDefs.size() < limit; i++) {
      outDefs.push_back(it->second[i]);
    }
    return !outDefs.empty();
  }

  bool getStructFields(const std::string &structName,
                       std::vector<std::string> &outFields) {
    std::lock_guard<std::mutex> lock(mutex);
    outFields.clear();
    auto itOrdered = store.structMembersOrdered.find(structName);
    if (itOrdered == store.structMembersOrdered.end())
      return false;
    outFields.reserve(itOrdered->second.size());
    for (const auto &m : itOrdered->second) {
      if (!m.name.empty())
        outFields.push_back(m.name);
    }
    return !outFields.empty();
  }

  bool getStructMemberType(const std::string &structName,
                           const std::string &memberName,
                           std::string &outType) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = store.structMemberType.find(structName);
    if (it == store.structMemberType.end())
      return false;
    auto jt = it->second.find(memberName);
    if (jt == it->second.end())
      return false;
    outType = jt->second;
    return !outType.empty();
  }

  bool getSymbolType(const std::string &symbol, std::string &outType) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = store.bestTypeBySymbol.find(symbol);
    if (it == store.bestTypeBySymbol.end())
      return false;
    outType = it->second;
    return !outType.empty();
  }

  bool isReady() const {
    std::lock_guard<std::mutex> lock(mutex);
    return ready;
  }

  Json getIndexingStateSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return makeIndexingStateJsonLocked();
  }

  void setConcurrencyLimits(size_t workerCount, size_t queueCapacity) {
    std::lock_guard<std::mutex> lock(mutex);
    limitWorkerCount = std::max(static_cast<size_t>(1), workerCount);
    if (queueCapacity == 0)
      limitQueueCapacity = static_cast<size_t>(4096);
    else
      limitQueueCapacity = std::max(static_cast<size_t>(64), queueCapacity);
    indexingUpdatedAtMs = nowUnixMs();
  }

  void kickIndexing(const std::string &reason) {
    bool shouldNotify = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!configured)
        return;
      pendingRebuild = true;
      pendingChangedUris.clear();
      pendingRebuildReason = reason.empty() ? "manual" : reason;
      ready = false;
      indexingEpoch++;
      indexingState = "Validating";
      indexingReason = pendingRebuildReason;
      progressPhase = "Validating";
      progressVisited = 0;
      progressTotal = store.filesByPath.size();
      pendingQueuedTasks = 1;
      pendingRunningWorkers = 0;
      pendingDirtyFiles = 0;
      pendingProbeRemainingBudget = 0;
      indexingUpdatedAtMs = nowUnixMs();
      shouldNotify = true;
    }
    cv.notify_one();
    ensureThread();
    if (shouldNotify)
      publishIndexingStateChanged(true);
  }

  void collectReverseIncludeClosure(const std::vector<std::string> &uris,
                                    std::vector<std::string> &outPaths,
                                    size_t limit) const {
    outPaths.clear();
    if (limit == 0)
      return;
    std::unordered_set<std::string> visited;
    std::vector<std::string> stack;
    for (const auto &u : uris) {
      std::string path = uriToPath(u);
      if (path.empty())
        path = u;
      const std::string key = normalizePathForCompare(path);
      if (key.empty())
        continue;
      if (visited.insert(key).second)
        stack.push_back(key);
    }
    if (stack.empty())
      return;

    std::lock_guard<std::mutex> lock(mutex);
    while (!stack.empty() && outPaths.size() < limit) {
      const std::string current = std::move(stack.back());
      stack.pop_back();
      auto it = reverseIncludeByTarget.find(current);
      if (it == reverseIncludeByTarget.end())
        continue;
      for (const auto &dep : it->second) {
        if (dep.empty())
          continue;
        if (!visited.insert(dep).second)
          continue;
        outPaths.push_back(dep);
        if (outPaths.size() >= limit)
          break;
        stack.push_back(dep);
      }
    }
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      pendingRebuild = false;
      pendingChangedUris.clear();
    }
    cv.notify_one();
    if (thread.joinable())
      thread.join();
  }

private:
  void ensureThread() {
    if (threadStarted.exchange(true))
      return;
    thread = std::thread([this]() {
      try {
        run();
      } catch (...) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ready = false;
          pendingRunningWorkers = 0;
          pendingQueuedTasks = 0;
          indexingState = "Error";
          indexingReason = "workerException";
          indexingUpdatedAtMs = nowUnixMs();
        }
        publishIndexingStateChanged(true);
      }
    });
  }

  bool parseFileToMeta(const fs::path &path,
                       const std::unordered_set<std::string> &extSet,
                       const std::vector<std::string> &workspaceFolders,
                       const std::vector<std::string> &includePaths,
                       const std::vector<std::string> &extensions,
                       std::string &keyOut, FileMeta &metaOut) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
      return false;
    if (!hasExtension(path, extSet))
      return false;

    uint64_t mtime = 0;
    uint64_t size = 0;
    auto t = fs::last_write_time(path, ec);
    if (!ec)
      mtime = fileTimeToUInt(t);
    size = static_cast<uint64_t>(fs::file_size(path, ec));

    const std::string rawPath = path.lexically_normal().string();
    const std::string key = normalizePathForCompare(rawPath);
    if (key.empty())
      return false;

    std::string text;
    if (!readFileToString(rawPath, text))
      return false;
    std::string uri = pathToUri(rawPath);

    FileMeta meta;
    meta.mtime = mtime;
    meta.size = size;
    extractDefinitions(uri, text, meta.defs);
    extractStructMembers(uri, text, workspaceFolders, includePaths, extensions,
                         meta.structs);
    {
      std::unordered_set<std::string> includeSet;
      std::istringstream stream(text);
      std::string line;
      bool inBlockComment = false;
      while (std::getline(stream, line)) {
        std::string includePath;
        if (!extractIncludePathOutsideComments(line, inBlockComment,
                                               includePath))
          continue;
        auto candidates = resolveIncludeCandidates(
            uri, includePath, workspaceFolders, includePaths, extensions);
        std::error_code ec2;
        for (const auto &candidate : candidates) {
          fs::path incPath(candidate);
          if (!fs::exists(incPath, ec2) || !fs::is_regular_file(incPath, ec2))
            continue;
          const std::string includeKey =
              normalizePathForCompare(incPath.string());
          if (!includeKey.empty() && includeSet.insert(includeKey).second)
            meta.includes.push_back(includeKey);
          break;
        }
      }
    }

    keyOut = key;
    metaOut = std::move(meta);
    return true;
  }

  void
  indexFilesParallel(const std::vector<fs::path> &files,
                     const std::unordered_set<std::string> &extSet,
                     const std::vector<std::string> &workspaceFolders,
                     const std::vector<std::string> &includePaths,
                     const std::vector<std::string> &extensions,
                     std::unordered_map<std::string, FileMeta> &outByPath,
                     const std::function<void(size_t, size_t)> &onProgress) {
    outByPath.clear();
    const size_t total = files.size();
    if (total == 0)
      return;

    size_t workerLimitSnapshot = 1;
    size_t queueCapacitySnapshot = 4096;
    {
      std::lock_guard<std::mutex> lock(mutex);
      workerLimitSnapshot = limitWorkerCount;
      queueCapacitySnapshot = limitQueueCapacity;
    }
    const size_t workerCount =
        std::max(static_cast<size_t>(1), std::min(workerLimitSnapshot, total));
    const size_t queueCapacity =
        std::max(static_cast<size_t>(64), queueCapacitySnapshot);
    const size_t shardCount = std::max(static_cast<size_t>(8), workerCount * 2);

    std::vector<std::unordered_map<std::string, FileMeta>> shards(shardCount);
    std::vector<std::mutex> shardMutexes(shardCount);
    std::deque<fs::path> queue;
    std::mutex queueMutex;
    std::condition_variable queueNotEmpty;
    std::condition_variable queueNotFull;
    std::atomic<size_t> processed{0};
    std::atomic<size_t> activeWorkers{0};
    bool producerDone = false;

    auto updatePendingSnapshot = [&](size_t queued, size_t running) {
      std::lock_guard<std::mutex> lock(mutex);
      pendingQueuedTasks = queued;
      pendingRunningWorkers = running;
      indexingUpdatedAtMs = nowUnixMs();
    };

    if (workerCount <= 1) {
      outByPath.reserve(total);
      updatePendingSnapshot(total, 1);
      size_t done = 0;
      for (const auto &file : files) {
        std::string key;
        FileMeta meta;
        if (parseFileToMeta(file, extSet, workspaceFolders, includePaths,
                            extensions, key, meta)) {
          outByPath[key] = std::move(meta);
        }
        done++;
        if (done == total || done % 32 == 0)
          updatePendingSnapshot(total > done ? total - done : 0, 1);
        if (onProgress && (done == total || done % 16 == 0))
          onProgress(done, total);
      }
      updatePendingSnapshot(0, 0);
      return;
    }

    updatePendingSnapshot(total, 0);

    std::thread producer([&]() {
      try {
        for (const auto &file : files) {
          std::unique_lock<std::mutex> lock(queueMutex);
          queueNotFull.wait(lock,
                            [&]() { return queue.size() < queueCapacity; });
          queue.push_back(file);
          queueNotEmpty.notify_one();
        }
      } catch (...) {
      }
      {
        std::lock_guard<std::mutex> lock(queueMutex);
        producerDone = true;
      }
      queueNotEmpty.notify_all();
    });

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (size_t worker = 0; worker < workerCount; worker++) {
      workers.emplace_back([&]() {
        while (true) {
          fs::path filePath;
          {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueNotEmpty.wait(
                lock, [&]() { return producerDone || !queue.empty(); });
            if (queue.empty()) {
              if (producerDone)
                return;
              continue;
            }
            filePath = std::move(queue.front());
            queue.pop_front();
            activeWorkers.fetch_add(1);
            queueNotFull.notify_one();
          }

          std::string key;
          FileMeta meta;
          bool parsed = false;
          try {
            parsed = parseFileToMeta(filePath, extSet, workspaceFolders,
                                     includePaths, extensions, key, meta);
          } catch (...) {
            parsed = false;
          }
          if (parsed) {
            const size_t shard = std::hash<std::string>{}(key) % shardCount;
            std::lock_guard<std::mutex> shardLock(shardMutexes[shard]);
            shards[shard][key] = std::move(meta);
          }

          const size_t done = ++processed;
          const size_t runningNow = activeWorkers.fetch_sub(1) - 1;
          if (done == total || done % 16 == 0) {
            size_t queuedNow = 0;
            {
              std::lock_guard<std::mutex> lock(queueMutex);
              queuedNow = queue.size();
            }
            updatePendingSnapshot(queuedNow, runningNow);
          }
          if (onProgress && (done == total || done % 16 == 0))
            onProgress(done, total);
        }
      });
    }

    producer.join();
    for (auto &worker : workers)
      worker.join();
    updatePendingSnapshot(0, 0);

    size_t reserveSize = 0;
    for (const auto &shard : shards)
      reserveSize += shard.size();
    outByPath.reserve(reserveSize);
    for (auto &shard : shards) {
      for (auto &entry : shard)
        outByPath[entry.first] = std::move(entry.second);
    }
  }

  void run() {
    while (true) {
      std::string localKey;
      std::vector<std::string> workspaceFolders;
      std::vector<std::string> roots;
      std::vector<std::string> includePaths;
      std::vector<std::string> exts;
      bool doRebuild = false;
      std::string rebuildReason;
      std::vector<std::string> changed;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() {
          return stopping || pendingRebuild || !pendingChangedUris.empty();
        });
        if (stopping)
          break;
        doRebuild = pendingRebuild;
        pendingRebuild = false;
        rebuildReason = pendingRebuildReason;
        changed.swap(pendingChangedUris);
        pendingQueuedTasks = pendingChangedUris.size();
        localKey = configuredKey;
        workspaceFolders = configuredWorkspaceFolders;
        roots = configuredRoots;
        includePaths = configuredIncludePaths;
        exts = configuredExtensions;
      }

      if (doRebuild) {
        buildAll(localKey, workspaceFolders, includePaths, roots, exts,
                 rebuildReason);
        continue;
      }
      if (!changed.empty()) {
        applyFileChanges(changed);
        continue;
      }
    }
  }

  void buildAll(const std::string &key,
                const std::vector<std::string> &workspaceFolders,
                const std::vector<std::string> &includePaths,
                const std::vector<std::string> &roots,
                const std::vector<std::string> &extensions,
                const std::string &reason) {
    const int token = ++indexToken;
    sendIndexingEvent("begin", token, "backgroundIndex", 0, 0);
    {
      std::lock_guard<std::mutex> lock(mutex);
      pendingRunningWorkers = limitWorkerCount;
      pendingQueuedTasks = 0;
      pendingDirtyFiles = 0;
      pendingProbeRemainingBudget = 0;
      indexingState = reason == "gitStorm" ? "Validating" : "Reindexing";
      indexingReason = reason.empty() ? "manual" : reason;
      progressPhase = reason == "gitStorm" ? "Validating" : "Building";
      progressVisited = 0;
      progressTotal = 0;
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(true);

    std::unordered_set<std::string> extSet;
    for (const auto &e : extensions) {
      extSet.insert(toLowerCopy(e));
    }

    IndexStore newStore;
    newStore.key = key;
    bool cacheHit = false;

    {
      IndexStore cached;
      if (loadFromDisk(key, workspaceFolders, cached)) {
        newStore = std::move(cached);
        cacheHit = true;
      }
    }

    std::unordered_set<std::string> indexedPaths;

    const std::string unitPath = getActiveUnitPath();
    std::vector<std::string> includeClosure;
    collectIncludeClosureFiles(unitPath, workspaceFolders, includePaths,
                               extensions, includeClosure);
    if (!includeClosure.empty()) {
      sendIndexingEvent("update", token, "backgroundIndex", 0,
                        includeClosure.size(), "unitIncludeClosure");
      {
        std::lock_guard<std::mutex> lock(mutex);
        indexingState = "Reindexing";
        progressPhase = "Reindexing";
        progressVisited = 0;
        progressTotal = includeClosure.size();
        indexingUpdatedAtMs = nowUnixMs();
      }
      publishIndexingStateChanged(false);
    }
    std::vector<fs::path> includeClosureFiles;
    includeClosureFiles.reserve(includeClosure.size());
    for (const auto &p : includeClosure) {
      fs::path filePath(p);
      if (!hasExtension(filePath, extSet))
        continue;
      const std::string fileKey = normalizePathForCompare(filePath.string());
      if (indexedPaths.find(fileKey) != indexedPaths.end())
        continue;
      includeClosureFiles.push_back(filePath);
      indexedPaths.insert(fileKey);
    }
    std::unordered_map<std::string, FileMeta> includeClosureResults;
    indexFilesParallel(
        includeClosureFiles, extSet, workspaceFolders, includePaths, extensions,
        includeClosureResults, [&](size_t visited, size_t total) {
          sendIndexingEvent("update", token, "backgroundIndex", visited, total,
                            "unitIncludeClosure");
          std::lock_guard<std::mutex> lock(mutex);
          indexingState = "Reindexing";
          progressPhase = "Reindexing";
          progressVisited = visited;
          progressTotal = total;
          indexingUpdatedAtMs = nowUnixMs();
        });
    for (auto &entry : includeClosureResults) {
      newStore.filesByPath[entry.first] = std::move(entry.second);
    }
    publishIndexingStateChanged(false);

    std::vector<std::string> orderedRoots;
    if (!unitPath.empty()) {
      addUnique(orderedRoots, unitPath);
    }
    for (const auto &root : roots) {
      if (!root.empty())
        addUnique(orderedRoots, root);
    }

    if (cacheHit) {
      const size_t total = newStore.filesByPath.size();
      sendIndexingEvent("update", token, "backgroundIndex", 0, total,
                        "cacheValidate");
      {
        std::lock_guard<std::mutex> lock(mutex);
        indexingState = "Validating";
        progressPhase = "Validating";
        progressVisited = 0;
        progressTotal = total;
        indexingUpdatedAtMs = nowUnixMs();
      }
      publishIndexingStateChanged(false);
      size_t visited = 0;
      std::vector<fs::path> dirtyPaths;
      std::vector<std::string> keys;
      keys.reserve(newStore.filesByPath.size());
      for (const auto &pair : newStore.filesByPath)
        keys.push_back(pair.first);
      for (const auto &pathKey : keys) {
        std::error_code ec;
        fs::path p(pathKey);
        if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) {
          newStore.filesByPath.erase(pathKey);
          visited++;
          sendIndexingEvent("update", token, "backgroundIndex", visited, total,
                            "cacheValidate");
          {
            std::lock_guard<std::mutex> lock(mutex);
            indexingState = "Validating";
            progressPhase = "Validating";
            progressVisited = visited;
            progressTotal = total;
            indexingUpdatedAtMs = nowUnixMs();
          }
          publishIndexingStateChanged(false);
          continue;
        }
        if (!hasExtension(p, extSet)) {
          visited++;
          sendIndexingEvent("update", token, "backgroundIndex", visited, total,
                            "cacheValidate");
          {
            std::lock_guard<std::mutex> lock(mutex);
            indexingState = "Validating";
            progressPhase = "Validating";
            progressVisited = visited;
            progressTotal = total;
            indexingUpdatedAtMs = nowUnixMs();
          }
          publishIndexingStateChanged(false);
          continue;
        }
        uint64_t mtime = 0;
        uint64_t size = 0;
        auto t = fs::last_write_time(p, ec);
        if (!ec)
          mtime = fileTimeToUInt(t);
        ec.clear();
        size = static_cast<uint64_t>(fs::file_size(p, ec));
        auto itMeta = newStore.filesByPath.find(pathKey);
        if (itMeta != newStore.filesByPath.end()) {
          if (itMeta->second.mtime != mtime || itMeta->second.size != size) {
            dirtyPaths.push_back(p);
          }
        }
        visited++;
        sendIndexingEvent("update", token, "backgroundIndex", visited, total,
                          "cacheValidate");
        {
          std::lock_guard<std::mutex> lock(mutex);
          indexingState = "Validating";
          progressPhase = "Validating";
          progressVisited = visited;
          progressTotal = total;
          indexingUpdatedAtMs = nowUnixMs();
        }
        publishIndexingStateChanged(false);
      }

      if (!dirtyPaths.empty()) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          indexingState = "Reindexing";
          progressPhase = "Reindexing";
          progressVisited = 0;
          progressTotal = dirtyPaths.size();
          pendingDirtyFiles = dirtyPaths.size();
          indexingUpdatedAtMs = nowUnixMs();
        }
        publishIndexingStateChanged(false);
        std::unordered_map<std::string, FileMeta> dirtyResults;
        indexFilesParallel(
            dirtyPaths, extSet, workspaceFolders, includePaths, extensions,
            dirtyResults, [&](size_t done, size_t totalDirty) {
              sendIndexingEvent("update", token, "backgroundIndex", done,
                                totalDirty, "cacheReindex");
              std::lock_guard<std::mutex> lock(mutex);
              indexingState = "Reindexing";
              progressPhase = "Reindexing";
              progressVisited = done;
              progressTotal = totalDirty;
              pendingDirtyFiles = totalDirty > done ? totalDirty - done
                                                    : static_cast<size_t>(0);
              indexingUpdatedAtMs = nowUnixMs();
            });
        for (auto &entry : dirtyResults) {
          newStore.filesByPath[entry.first] = std::move(entry.second);
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          pendingDirtyFiles = 0;
        }
      }

      const auto probeTimeBudget = std::chrono::milliseconds(150);
      const size_t probeFileBudget = 400;
      const auto probeStart = std::chrono::steady_clock::now();
      size_t probed = 0;
      sendIndexingEvent("update", token, "backgroundIndex", 0, probeFileBudget,
                        "newFileProbe");
      {
        std::lock_guard<std::mutex> lock(mutex);
        indexingState = "Probing";
        progressPhase = "Probing";
        progressVisited = 0;
        progressTotal = probeFileBudget;
        pendingProbeRemainingBudget = probeFileBudget;
        indexingUpdatedAtMs = nowUnixMs();
      }
      publishIndexingStateChanged(false);
      for (const auto &root : orderedRoots) {
        if (root.empty())
          continue;
        if (std::chrono::steady_clock::now() - probeStart > probeTimeBudget)
          break;
        if (probed >= probeFileBudget)
          break;
        std::error_code ec;
        fs::path base(root);
        if (!fs::exists(base, ec))
          continue;
        if (fs::is_regular_file(base, ec)) {
          if (!hasExtension(base, extSet))
            continue;
          const std::string fileKey = normalizePathForCompare(base.string());
          if (indexedPaths.find(fileKey) != indexedPaths.end())
            continue;
          indexOneFile(base, extSet, newStore);
          indexedPaths.insert(fileKey);
          continue;
        }
        if (!fs::is_directory(base, ec))
          continue;

        fs::recursive_directory_iterator it(
            base, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator endIt;
        for (; it != endIt && !ec; it.increment(ec)) {
          if (std::chrono::steady_clock::now() - probeStart > probeTimeBudget)
            break;
          if (probed >= probeFileBudget)
            break;
          if (!it->is_regular_file(ec))
            continue;
          const fs::path filePath = it->path();
          if (!hasExtension(filePath, extSet))
            continue;
          probed++;
          const std::string fileKey =
              normalizePathForCompare(filePath.string());
          if (!fileKey.empty() && newStore.filesByPath.find(fileKey) ==
                                      newStore.filesByPath.end()) {
            indexOneFile(filePath, extSet, newStore);
          }
          sendIndexingEvent("update", token, "backgroundIndex", probed,
                            probeFileBudget, "newFileProbe");
          {
            std::lock_guard<std::mutex> lock(mutex);
            indexingState = "Probing";
            progressPhase = "Probing";
            progressVisited = probed;
            progressTotal = probeFileBudget;
            pendingProbeRemainingBudget =
                probeFileBudget > probed ? probeFileBudget - probed : 0;
            indexingUpdatedAtMs = nowUnixMs();
          }
          publishIndexingStateChanged(false);
        }
      }

      for (const auto &root : orderedRoots) {
        if (root.empty())
          continue;
        std::error_code ec;
        fs::path base(root);
        if (!fs::exists(base, ec) || !fs::is_regular_file(base, ec))
          continue;
        if (!hasExtension(base, extSet))
          continue;
        const std::string fileKey = normalizePathForCompare(base.string());
        if (indexedPaths.find(fileKey) != indexedPaths.end())
          continue;
        indexOneFile(base, extSet, newStore);
        indexedPaths.insert(fileKey);
      }
    } else {
      std::vector<fs::path> toIndexFiles;
      {
        std::lock_guard<std::mutex> lock(mutex);
        indexingState = "Reindexing";
        progressPhase = "Reindexing";
        progressVisited = 0;
        progressTotal = 0;
        indexingUpdatedAtMs = nowUnixMs();
      }
      publishIndexingStateChanged(false);
      std::unordered_set<std::string> foundPaths;
      for (const auto &root : orderedRoots) {
        if (root.empty())
          continue;
        std::error_code ec;
        fs::path base(root);
        if (!fs::exists(base, ec))
          continue;
        if (fs::is_regular_file(base, ec)) {
          if (!hasExtension(base, extSet))
            continue;
          const std::string fileKey = normalizePathForCompare(base.string());
          if (foundPaths.find(fileKey) != foundPaths.end())
            continue;
          toIndexFiles.push_back(base);
          foundPaths.insert(fileKey);
          continue;
        }
        if (!fs::is_directory(base, ec))
          continue;

        fs::recursive_directory_iterator it(
            base, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator endIt;
        for (; it != endIt && !ec; it.increment(ec)) {
          if (!it->is_regular_file(ec))
            continue;
          const fs::path filePath = it->path();
          if (!hasExtension(filePath, extSet))
            continue;
          const std::string fileKey =
              normalizePathForCompare(filePath.string());
          if (foundPaths.find(fileKey) != foundPaths.end())
            continue;
          toIndexFiles.push_back(filePath);
          foundPaths.insert(fileKey);
        }
      }

      std::unordered_map<std::string, FileMeta> fullResults;
      indexFilesParallel(toIndexFiles, extSet, workspaceFolders, includePaths,
                         extensions, fullResults,
                         [&](size_t done, size_t totalFiles) {
                           sendIndexingEvent("update", token, "backgroundIndex",
                                             done, totalFiles);
                           std::lock_guard<std::mutex> lock(mutex);
                           indexingState = "Reindexing";
                           progressPhase = "Reindexing";
                           progressVisited = done;
                           progressTotal = totalFiles;
                           indexingUpdatedAtMs = nowUnixMs();
                         });
      for (auto &entry : fullResults) {
        newStore.filesByPath[entry.first] = std::move(entry.second);
      }
      publishIndexingStateChanged(false);

      for (auto it = newStore.filesByPath.begin();
           it != newStore.filesByPath.end();) {
        if (foundPaths.find(normalizePathForCompare(it->first)) ==
            foundPaths.end()) {
          it = newStore.filesByPath.erase(it);
        } else {
          ++it;
        }
      }
    }

    rebuildGlobals(newStore);
    std::unordered_map<std::string, std::vector<std::string>> reverse;
    buildReverseIncludes(newStore, reverse);
    saveToDisk(newStore, workspaceFolders);
    {
      std::lock_guard<std::mutex> lock(mutex);
      store = std::move(newStore);
      reverseIncludeByTarget = std::move(reverse);
      ready = true;
      pendingQueuedTasks = pendingChangedUris.size();
      pendingRunningWorkers = 0;
      pendingDirtyFiles = 0;
      pendingProbeRemainingBudget = 0;
      indexingState = "Idle";
      indexingReason = reason.empty() ? "manual" : reason;
      progressPhase = "Idle";
      progressVisited = 0;
      progressTotal = 0;
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(true);

    sendIndexingEvent("end", token, "backgroundIndex", 0, 0);
  }

  void applyFileChanges(const std::vector<std::string> &uris) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      pendingRunningWorkers =
          std::max(static_cast<size_t>(1),
                   std::min(limitWorkerCount,
                            std::max(static_cast<size_t>(1), uris.size())));
      pendingQueuedTasks = 0;
      indexingState = "Updating";
      indexingReason = "fileWatch";
      progressPhase = "Updating";
      progressVisited = 0;
      progressTotal = uris.size();
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(false);

    IndexStore snapshot;
    std::vector<std::string> workspaceFoldersSnapshot;
    std::vector<std::string> includePathsSnapshot;
    std::vector<std::string> extensionsSnapshot;
    {
      std::lock_guard<std::mutex> lock(mutex);
      snapshot = store;
      workspaceFoldersSnapshot = configuredWorkspaceFolders;
      includePathsSnapshot = configuredIncludePaths;
      extensionsSnapshot = configuredExtensions;
    }

    std::unordered_set<std::string> extSet;
    for (const auto &e : extensionsSnapshot)
      extSet.insert(toLowerCopy(e));

    bool changed = false;
    std::vector<fs::path> reindexPaths;
    std::unordered_set<std::string> reindexKeys;
    size_t visited = 0;
    for (const auto &u : uris) {
      std::string path = uriToPath(u);
      if (path.empty())
        path = u;
      std::error_code ec;
      fs::path p(path);
      const std::string pathKey = normalizePathForCompare(p.string());
      if (!fs::exists(p, ec)) {
        auto it = snapshot.filesByPath.find(pathKey);
        if (it != snapshot.filesByPath.end()) {
          snapshot.filesByPath.erase(it);
          changed = true;
        }
        visited++;
        {
          std::lock_guard<std::mutex> lock(mutex);
          progressVisited = visited;
          progressTotal = uris.size();
          indexingUpdatedAtMs = nowUnixMs();
        }
        publishIndexingStateChanged(false);
        continue;
      }
      if (!fs::is_regular_file(p, ec))
        continue;
      if (!hasExtension(p, extSet))
        continue;
      if (reindexKeys.insert(pathKey).second)
        reindexPaths.push_back(p);
      changed = true;
      visited++;
      {
        std::lock_guard<std::mutex> lock(mutex);
        progressVisited = visited;
        progressTotal = uris.size();
        indexingUpdatedAtMs = nowUnixMs();
      }
      publishIndexingStateChanged(false);
    }

    if (!reindexPaths.empty()) {
      std::unordered_map<std::string, FileMeta> reindexed;
      indexFilesParallel(reindexPaths, extSet, workspaceFoldersSnapshot,
                         includePathsSnapshot, extensionsSnapshot, reindexed,
                         [&](size_t done, size_t total) {
                           sendIndexingEvent("update", 0, "backgroundIndex",
                                             done, total, "fileWatch");
                           std::lock_guard<std::mutex> lock(mutex);
                           progressVisited = done;
                           progressTotal = total;
                           indexingUpdatedAtMs = nowUnixMs();
                         });
      for (auto &entry : reindexed)
        snapshot.filesByPath[entry.first] = std::move(entry.second);
    }

    if (!changed) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        pendingRunningWorkers = 0;
        pendingQueuedTasks = pendingChangedUris.size();
        indexingState = pendingQueuedTasks > 0 ? "Updating" : "Idle";
        progressVisited = 0;
        progressTotal = 0;
        progressPhase = indexingState == "Idle" ? "Idle" : "Updating";
        indexingUpdatedAtMs = nowUnixMs();
      }
      publishIndexingStateChanged(false);
      return;
    }

    rebuildGlobals(snapshot);
    std::unordered_map<std::string, std::vector<std::string>> reverse;
    buildReverseIncludes(snapshot, reverse);
    saveToDisk(snapshot, workspaceFoldersSnapshot);
    {
      std::lock_guard<std::mutex> lock(mutex);
      store = std::move(snapshot);
      reverseIncludeByTarget = std::move(reverse);
      ready = true;
      pendingRunningWorkers = 0;
      pendingQueuedTasks = pendingChangedUris.size();
      pendingDirtyFiles = 0;
      pendingProbeRemainingBudget = 0;
      indexingState = pendingQueuedTasks > 0 ? "Updating" : "Idle";
      indexingReason = "fileWatch";
      progressPhase = indexingState == "Idle" ? "Idle" : "Updating";
      progressVisited = 0;
      progressTotal = 0;
      indexingUpdatedAtMs = nowUnixMs();
    }
    publishIndexingStateChanged(true);
  }

  void indexOneFile(const fs::path &path,
                    const std::unordered_set<std::string> &extSet,
                    IndexStore &target) {
    const std::string rawPath = path.lexically_normal().string();
    const std::string normalizedKey = normalizePathForCompare(rawPath);
    auto existing = target.filesByPath.find(normalizedKey);
    if (existing != target.filesByPath.end()) {
      std::error_code ec;
      uint64_t mtime = 0;
      uint64_t size = 0;
      auto t = fs::last_write_time(path, ec);
      if (!ec)
        mtime = fileTimeToUInt(t);
      size = static_cast<uint64_t>(fs::file_size(path, ec));
      if (existing->second.mtime == mtime && existing->second.size == size)
        return;
    }
    std::string key;
    FileMeta meta;
    if (!parseFileToMeta(path, extSet, configuredWorkspaceFolders,
                         configuredIncludePaths, configuredExtensions, key,
                         meta)) {
      return;
    }
    target.filesByPath[key] = std::move(meta);
  }

  bool loadFromDisk(const std::string &key,
                    const std::vector<std::string> &workspaceFolders,
                    IndexStore &out) {
    auto tryLoadIndexV2 = [&](IndexStore &loadedOut) -> bool {
      const fs::path dir = indexV2DirPath(workspaceFolders, key);
      const fs::path manifestPath = indexV2ManifestPath(dir);
      std::error_code ec;
      if (!fs::exists(manifestPath, ec))
        return false;
      std::string content;
      if (!readFileToString(manifestPath.string(), content))
        return false;
      Json root;
      if (!parseJson(content, root))
        return false;
      if (root.type != Json::Type::Object)
        return false;
      const Json *verV = getObjectValue(root, "version");
      const Json *keyV = getObjectValue(root, "key");
      const Json *filesV = getObjectValue(root, "files");
      const int ver = verV ? static_cast<int>(getNumberValue(*verV)) : 0;
      if (ver != 2)
        return false;
      if (!keyV || keyV->type != Json::Type::String || keyV->s != key)
        return false;
      if (!filesV || filesV->type != Json::Type::Array)
        return false;

      IndexStore loaded;
      loaded.key = key;
      loaded.filesByPath.clear();
      const fs::path filesDir = indexV2FilesDir(dir);
      for (const auto &entry : filesV->a) {
        if (entry.type != Json::Type::Object)
          continue;
        const Json *pathV = getObjectValue(entry, "path");
        const Json *shardV = getObjectValue(entry, "shard");
        if (!pathV || pathV->type != Json::Type::String)
          continue;
        if (!shardV || shardV->type != Json::Type::String)
          continue;
        const fs::path shardPath = filesDir / shardV->s;
        if (!fs::exists(shardPath, ec))
          continue;
        std::string shardContent;
        if (!readFileToString(shardPath.string(), shardContent))
          continue;
        Json shardRoot;
        if (!parseJson(shardContent, shardRoot))
          continue;
        std::string parsedPath;
        FileMeta meta;
        if (!deserializeFileEntry(shardRoot, parsedPath, meta))
          continue;
        const std::string key = normalizePathForCompare(pathV->s);
        if (key.empty())
          continue;
        loaded.filesByPath[key] = std::move(meta);
      }
      rebuildGlobals(loaded);
      loadedOut = std::move(loaded);
      return true;
    };

    auto tryLoadIndexV1 = [&](const fs::path &p,
                              IndexStore &loadedOut) -> bool {
      std::error_code ec;
      if (!fs::exists(p, ec))
        return false;
      std::string content;
      if (!readFileToString(p.string(), content))
        return false;
      Json root;
      if (!parseJson(content, root))
        return false;
      IndexStore loaded;
      if (!deserializeIndex(root, loaded))
        return false;
      if (loaded.key != key)
        return false;
      loadedOut = std::move(loaded);
      return true;
    };

    IndexStore loaded;
    if (tryLoadIndexV2(loaded)) {
      out = std::move(loaded);
      return true;
    }

    const fs::path shardV1Path = indexV1CachePathForKey(workspaceFolders, key);
    if (tryLoadIndexV1(shardV1Path, loaded)) {
      out = loaded;
      saveToDisk(out, workspaceFolders);
      return true;
    }

    const fs::path legacyV1Path = indexV1LegacyCachePath(workspaceFolders);
    if (tryLoadIndexV1(legacyV1Path, loaded)) {
      out = loaded;
      saveToDisk(out, workspaceFolders);
      return true;
    }

    return false;
  }

  void saveToDisk(const IndexStore &store,
                  const std::vector<std::string> &workspaceFolders) {
    const fs::path dir = indexV2DirPath(workspaceFolders, store.key);
    ensureIndexV2Dirs(dir);
    const fs::path filesDir = indexV2FilesDir(dir);
    const fs::path manifestPath = indexV2ManifestPath(dir);

    std::unordered_map<std::string, std::tuple<uint64_t, uint64_t, std::string>>
        oldIndex;
    {
      std::error_code ec;
      if (fs::exists(manifestPath, ec)) {
        std::string content;
        if (readFileToString(manifestPath.string(), content)) {
          Json root;
          if (parseJson(content, root) && root.type == Json::Type::Object) {
            const Json *verV = getObjectValue(root, "version");
            const Json *keyV = getObjectValue(root, "key");
            const Json *filesV = getObjectValue(root, "files");
            const int ver = verV ? static_cast<int>(getNumberValue(*verV)) : 0;
            if (ver == 2 && keyV && keyV->type == Json::Type::String &&
                keyV->s == store.key && filesV &&
                filesV->type == Json::Type::Array) {
              for (const auto &e : filesV->a) {
                if (e.type != Json::Type::Object)
                  continue;
                const Json *pathV = getObjectValue(e, "path");
                const Json *mtimeV = getObjectValue(e, "mtime");
                const Json *sizeV = getObjectValue(e, "size");
                const Json *shardV = getObjectValue(e, "shard");
                if (!pathV || pathV->type != Json::Type::String)
                  continue;
                if (!shardV || shardV->type != Json::Type::String)
                  continue;
                const uint64_t mtime =
                    mtimeV ? static_cast<uint64_t>(getNumberValue(*mtimeV)) : 0;
                const uint64_t size =
                    sizeV ? static_cast<uint64_t>(getNumberValue(*sizeV)) : 0;
                oldIndex.emplace(pathV->s,
                                 std::make_tuple(mtime, size, shardV->s));
              }
            }
          }
        }
      }
    }

    Json manifest = makeObject();
    manifest.o["version"] = makeNumber(2);
    manifest.o["key"] = makeString(store.key);
    Json files = makeArray();
    std::unordered_set<std::string> keptPaths;
    keptPaths.reserve(store.filesByPath.size());

    std::error_code ec;
    for (const auto &pair : store.filesByPath) {
      const std::string &path = pair.first;
      const FileMeta &meta = pair.second;
      const std::string shardName = indexV2FileShardNameForPath(path);
      const fs::path shardPath = filesDir / shardName;
      bool shouldWrite = true;
      auto itOld = oldIndex.find(path);
      if (itOld != oldIndex.end()) {
        const auto &[oldMtime, oldSize, oldShard] = itOld->second;
        if (oldMtime == meta.mtime && oldSize == meta.size) {
          if (fs::exists(shardPath, ec))
            shouldWrite = false;
          ec.clear();
        }
      }
      if (shouldWrite) {
        Json fileRoot = serializeFileEntry(path, meta);
        std::string content = serializeJson(fileRoot);
        std::ofstream out(shardPath.string(),
                          std::ios::binary | std::ios::trunc);
        if (out) {
          out.write(content.data(),
                    static_cast<std::streamsize>(content.size()));
        }
      }

      Json entry = makeObject();
      entry.o["path"] = makeString(path);
      entry.o["mtime"] = makeNumber(static_cast<double>(meta.mtime));
      entry.o["size"] = makeNumber(static_cast<double>(meta.size));
      entry.o["shard"] = makeString(shardName);
      files.a.push_back(std::move(entry));
      keptPaths.insert(path);
    }

    for (const auto &pair : oldIndex) {
      if (keptPaths.find(pair.first) != keptPaths.end())
        continue;
      const std::string &oldShard = std::get<2>(pair.second);
      fs::remove(filesDir / oldShard, ec);
      ec.clear();
    }

    manifest.o["files"] = std::move(files);
    {
      std::string content = serializeJson(manifest);
      std::ofstream out(manifestPath.string(),
                        std::ios::binary | std::ios::trunc);
      if (out) {
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
      }
    }

    fs::path baseDir = indexCacheDir(workspaceFolders);
    std::vector<std::pair<fs::path, uint64_t>> dirs;
    fs::directory_iterator dit(baseDir, ec);
    fs::directory_iterator endIt;
    for (; dit != endIt && !ec; dit.increment(ec)) {
      if (!dit->is_directory(ec))
        continue;
      const fs::path d = dit->path();
      const std::string name = d.filename().string();
      if (name.rfind("index_v2_", 0) != 0)
        continue;
      std::error_code ec2;
      fs::path mp = indexV2ManifestPath(d);
      auto t = fs::exists(mp, ec2) ? fs::last_write_time(mp, ec2)
                                   : fs::last_write_time(d, ec2);
      if (ec2) {
        ec2.clear();
        continue;
      }
      dirs.emplace_back(d, fileTimeToUInt(t));
    }
    ec.clear();

    const size_t keep = 8;
    if (dirs.size() > keep) {
      std::sort(dirs.begin(), dirs.end(), [](const auto &a, const auto &b) {
        return a.second < b.second;
      });
      for (size_t i = 0; i + keep < dirs.size(); i++) {
        fs::remove_all(dirs[i].first, ec);
        ec.clear();
      }
    }
  }

private:
  Json makeIndexingStateJsonLocked() const {
    Json root = makeObject();
    root.o["epoch"] = makeNumber(static_cast<double>(indexingEpoch));
    root.o["state"] = makeString(indexingState);
    root.o["reason"] = makeString(indexingReason);
    if (!activeModelsHash.empty())
      root.o["modelsHash"] = makeString(activeModelsHash);
    if (!activeModelsHashError.empty())
      root.o["error"] = makeString(activeModelsHashError);
    root.o["updatedAtMs"] =
        makeNumber(static_cast<double>(indexingUpdatedAtMs));

    Json pending = makeObject();
    pending.o["queuedTasks"] =
        makeNumber(static_cast<double>(pendingQueuedTasks));
    pending.o["runningWorkers"] =
        makeNumber(static_cast<double>(pendingRunningWorkers));
    pending.o["dirtyFiles"] =
        makeNumber(static_cast<double>(pendingDirtyFiles));
    pending.o["probeRemainingBudget"] =
        makeNumber(static_cast<double>(pendingProbeRemainingBudget));
    root.o["pending"] = std::move(pending);

    Json progress = makeObject();
    progress.o["phase"] = makeString(progressPhase);
    progress.o["visited"] = makeNumber(static_cast<double>(progressVisited));
    progress.o["total"] = makeNumber(static_cast<double>(progressTotal));
    root.o["progress"] = std::move(progress);

    Json limits = makeObject();
    limits.o["workerCount"] = makeNumber(static_cast<double>(limitWorkerCount));
    limits.o["queueCapacity"] =
        makeNumber(static_cast<double>(limitQueueCapacity));
    root.o["limits"] = std::move(limits);
    return root;
  }

  void publishIndexingStateChanged(bool force) {
    Json payload;
    bool shouldSend = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const int64_t now = nowUnixMs();
      if (force || now - lastStatePushAtMs >= 150) {
        lastStatePushAtMs = now;
        payload = makeIndexingStateJsonLocked();
        shouldSend = true;
      }
    }
    if (shouldSend)
      writeNotification("nsf/indexingStateChanged", payload);
  }

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::thread thread;
  std::atomic<bool> threadStarted{false};
  std::atomic<int> indexToken{0};
  bool stopping = false;

  bool configured = false;
  bool pendingRebuild = false;
  std::string pendingRebuildReason = "startup";
  bool ready = false;
  std::string configuredKey;
  std::vector<std::string> configuredWorkspaceFolders;
  std::vector<std::string> configuredRoots;
  std::vector<std::string> configuredIncludePaths;
  std::vector<std::string> configuredExtensions;
  std::vector<std::string> pendingChangedUris;
  std::string activeModelsHash;
  std::string activeModelsHashError;
  int64_t indexingEpoch = 0;
  std::string indexingState = "Idle";
  std::string indexingReason = "startup";
  int64_t indexingUpdatedAtMs = 0;
  size_t pendingQueuedTasks = 0;
  size_t pendingRunningWorkers = 0;
  size_t pendingDirtyFiles = 0;
  size_t pendingProbeRemainingBudget = 0;
  std::string progressPhase = "Idle";
  size_t progressVisited = 0;
  size_t progressTotal = 0;
  size_t limitWorkerCount = defaultWorkerCount();
  size_t limitQueueCapacity = 4096;
  int64_t lastStatePushAtMs = 0;
  IndexStore store;
  std::unordered_map<std::string, std::vector<std::string>>
      reverseIncludeByTarget;
};

static WorkspaceIndex &getIndex() {
  static WorkspaceIndex idx;
  return idx;
}

void workspaceIndexConfigure(const std::vector<std::string> &workspaceFolders,
                             const std::vector<std::string> &includePaths,
                             const std::vector<std::string> &shaderExtensions) {
  getIndex().configure(workspaceFolders, includePaths, shaderExtensions);
}

void workspaceIndexHandleFileChanges(const std::vector<std::string> &uris) {
  getIndex().handleFileChanges(uris);
}

bool workspaceIndexFindDefinition(const std::string &symbol,
                                  DefinitionLocation &outLocation) {
  return getIndex().findDefinition(symbol, outLocation);
}

bool workspaceIndexFindStructDefinition(const std::string &symbol,
                                        DefinitionLocation &outLocation) {
  return getIndex().findStructDefinition(symbol, outLocation);
}

bool workspaceIndexFindDefinitions(const std::string &symbol,
                                   std::vector<IndexedDefinition> &outDefs,
                                   size_t limit) {
  return getIndex().findDefinitions(symbol, outDefs, limit);
}

bool workspaceIndexGetStructFields(const std::string &structName,
                                   std::vector<std::string> &outFields) {
  return getIndex().getStructFields(structName, outFields);
}

bool workspaceIndexGetStructMemberType(const std::string &structName,
                                       const std::string &memberName,
                                       std::string &outType) {
  return getIndex().getStructMemberType(structName, memberName, outType);
}

bool workspaceIndexGetSymbolType(const std::string &symbol,
                                 std::string &outType) {
  return getIndex().getSymbolType(symbol, outType);
}

bool workspaceIndexIsReady() { return getIndex().isReady(); }

Json workspaceIndexGetIndexingState() {
  return getIndex().getIndexingStateSnapshot();
}

void workspaceIndexKickIndexing(const std::string &reason) {
  getIndex().kickIndexing(reason);
}

void workspaceIndexSetConcurrencyLimits(size_t workerCount,
                                        size_t queueCapacity) {
  getIndex().setConcurrencyLimits(workerCount, queueCapacity);
}

void workspaceIndexCollectReverseIncludeClosure(
    const std::vector<std::string> &uris, std::vector<std::string> &outPaths,
    size_t limit) {
  getIndex().collectReverseIncludeClosure(uris, outPaths, limit);
}

void workspaceIndexShutdown() { getIndex().shutdown(); }
