#include "workspace_index_internal.hpp"

#include "include_resolver.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

std::string toLowerCopy(const std::string &value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

std::string normalizePathForCompare(const std::string &value) {
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

bool isPathUnderOrEqual(const std::string &dir, const std::string &path) {
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

bool extractIncludePathOutsideComments(const std::string &line,
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

namespace {

uint64_t fileTimeToUInt(const fs::file_time_type &t) {
  using namespace std::chrono;
  auto s = duration_cast<seconds>(t.time_since_epoch());
  return static_cast<uint64_t>(s.count());
}

} // namespace

void collectIncludeClosureFiles(
    const std::string &entryPath,
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
        fs::path path(candidate);
        if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec))
          continue;
        const std::string normalizedCandidate = path.lexically_normal().string();
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

bool isAbsolutePath(const std::string &path) {
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':')
    return true;
  return path.rfind("\\\\", 0) == 0 || path.rfind("/", 0) == 0;
}

std::string joinPath(const std::string &base, const std::string &child) {
  if (base.empty())
    return child;
  char sep = '\\';
  if (base.back() == '/' || base.back() == '\\')
    return base + child;
  return base + sep + child;
}

void addUnique(std::vector<std::string> &items, const std::string &value) {
  for (const auto &item : items) {
    if (item == value)
      return;
  }
  items.push_back(value);
}

std::string computeIndexKey(const std::vector<std::string> &folders,
                            const std::vector<std::string> &includePaths,
                            const std::vector<std::string> &extensions,
                            const std::string &modelsHash) {
  std::string key;
  auto appendList = [&](const char *tag,
                        const std::vector<std::string> &items) {
    key.append(tag);
    key.push_back(':');
    for (const auto &value : items) {
      key.append(toLowerCopy(value));
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
  key.append("includeParser:5;");
  return key;
}

bool hasExtension(const fs::path &path,
                  const std::unordered_set<std::string> &extensions) {
  std::string ext = toLowerCopy(path.extension().string());
  return extensions.find(ext) != extensions.end();
}

bool parseFileToMeta(const fs::path &path,
                     const std::unordered_set<std::string> &extSet,
                     const std::vector<std::string> &workspaceFolders,
                     const std::vector<std::string> &includePaths,
                     const std::vector<std::string> &extensions,
                     std::string &keyOut, FileMeta &metaOut) {
  std::error_code ec;
  if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec))
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
      if (!extractIncludePathOutsideComments(line, inBlockComment, includePath))
        continue;
      auto candidates = resolveIncludeCandidates(
          uri, includePath, workspaceFolders, includePaths, extensions);
      std::error_code includeEc;
      for (const auto &candidate : candidates) {
        fs::path includeFile(candidate);
        if (!fs::exists(includeFile, includeEc) ||
            !fs::is_regular_file(includeFile, includeEc)) {
          continue;
        }
        const std::string includeKey =
            normalizePathForCompare(includeFile.string());
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
