#include "server_documents.hpp"

#include "uri_utils.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace {

struct CachedDiskDocument {
  std::string text;
  std::string normalizedPath;
};

std::mutex gDocumentCacheMutex;
std::unordered_map<std::string, CachedDiskDocument> gDocumentCacheByUri;
std::unordered_map<std::string, std::unordered_set<std::string>>
    gDocumentCacheUrisByPath;
std::atomic<int> gDocumentPrefetchMaxConcurrency{4};

std::string normalizePathForCache(const std::string &path) {
  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  std::transform(
      normalized.begin(), normalized.end(), normalized.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  while (normalized.size() > 1 && normalized.back() == '/')
    normalized.pop_back();
  return normalized;
}

void eraseCachedUriLocked(const std::string &uri) {
  auto it = gDocumentCacheByUri.find(uri);
  if (it == gDocumentCacheByUri.end())
    return;
  if (!it->second.normalizedPath.empty()) {
    auto pathIt = gDocumentCacheUrisByPath.find(it->second.normalizedPath);
    if (pathIt != gDocumentCacheUrisByPath.end()) {
      pathIt->second.erase(uri);
      if (pathIt->second.empty())
        gDocumentCacheUrisByPath.erase(pathIt);
    }
  }
  gDocumentCacheByUri.erase(it);
}

} // namespace

std::string normalizeDocumentText(const std::string &text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (char ch : text) {
    if (ch != '\r')
      normalized.push_back(ch);
  }
  return normalized;
}

bool readFileText(const std::string &path, std::string &text) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  if (!stream.good() && !stream.eof())
    return false;
  text = normalizeDocumentText(buffer.str());
  return true;
}

bool loadDocumentText(
    const std::string &uri,
    const std::unordered_map<std::string, Document> &documents,
    std::string &text) {
  auto docIt = documents.find(uri);
  if (docIt != documents.end()) {
    text = docIt->second.text;
    primeDocumentTextCache(uri, text);
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(gDocumentCacheMutex);
    auto cacheIt = gDocumentCacheByUri.find(uri);
    if (cacheIt != gDocumentCacheByUri.end()) {
      text = cacheIt->second.text;
      return true;
    }
  }
  const std::string path = uriToPath(uri);
  if (path.empty())
    return false;
  if (!readFileText(path, text))
    return false;
  std::lock_guard<std::mutex> lock(gDocumentCacheMutex);
  eraseCachedUriLocked(uri);
  CachedDiskDocument entry;
  entry.text = text;
  entry.normalizedPath = normalizePathForCache(path);
  gDocumentCacheByUri[uri] = std::move(entry);
  gDocumentCacheUrisByPath[normalizePathForCache(path)].insert(uri);
  return true;
}

void prefetchDocumentTexts(
    const std::vector<std::string> &uris,
    const std::unordered_map<std::string, Document> &documents) {
  if (uris.empty())
    return;
  std::unordered_set<std::string> deduped;
  deduped.reserve(uris.size());
  std::vector<std::string> uniqueUris;
  uniqueUris.reserve(uris.size());
  for (const auto &uri : uris) {
    if (uri.empty() || !deduped.insert(uri).second)
      continue;
    uniqueUris.push_back(uri);
  }
  if (uniqueUris.empty())
    return;
  int configured = gDocumentPrefetchMaxConcurrency.load();
  if (configured < 1)
    configured = 1;
  const size_t workerCount =
      std::min(static_cast<size_t>(configured), uniqueUris.size());
  if (workerCount <= 1) {
    std::string ignored;
    for (const auto &uri : uniqueUris)
      loadDocumentText(uri, documents, ignored);
    return;
  }

  std::atomic<size_t> nextIndex{0};
  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (size_t worker = 0; worker < workerCount; worker++) {
    workers.emplace_back([&]() {
      std::string ignored;
      while (true) {
        const size_t index = nextIndex.fetch_add(1);
        if (index >= uniqueUris.size())
          break;
        loadDocumentText(uniqueUris[index], documents, ignored);
      }
    });
  }
  for (auto &worker : workers)
    worker.join();
}

void setDocumentPrefetchMaxConcurrency(int value) {
  if (value < 1)
    value = 1;
  gDocumentPrefetchMaxConcurrency.store(value);
}

void primeDocumentTextCache(const std::string &uri, const std::string &text) {
  if (uri.empty())
    return;
  std::lock_guard<std::mutex> lock(gDocumentCacheMutex);
  eraseCachedUriLocked(uri);
  CachedDiskDocument entry;
  entry.text = text;
  const std::string path = uriToPath(uri);
  if (!path.empty())
    entry.normalizedPath = normalizePathForCache(path);
  if (!entry.normalizedPath.empty())
    gDocumentCacheUrisByPath[entry.normalizedPath].insert(uri);
  gDocumentCacheByUri[uri] = std::move(entry);
}

void invalidateDocumentTextCacheByUri(const std::string &uri) {
  if (uri.empty())
    return;
  std::lock_guard<std::mutex> lock(gDocumentCacheMutex);
  eraseCachedUriLocked(uri);
}

void invalidateDocumentTextCacheByPath(const std::string &path) {
  if (path.empty())
    return;
  std::lock_guard<std::mutex> lock(gDocumentCacheMutex);
  const std::string normalizedPath = normalizePathForCache(path);
  auto pathIt = gDocumentCacheUrisByPath.find(normalizedPath);
  if (pathIt == gDocumentCacheUrisByPath.end())
    return;
  std::vector<std::string> uris(pathIt->second.begin(), pathIt->second.end());
  gDocumentCacheUrisByPath.erase(pathIt);
  for (const auto &uri : uris) {
    auto cacheIt = gDocumentCacheByUri.find(uri);
    if (cacheIt == gDocumentCacheByUri.end())
      continue;
    if (cacheIt->second.normalizedPath == normalizedPath)
      gDocumentCacheByUri.erase(cacheIt);
  }
}

void invalidateDocumentTextCacheByUris(const std::vector<std::string> &uris) {
  if (uris.empty())
    return;
  std::lock_guard<std::mutex> lock(gDocumentCacheMutex);
  for (const auto &uri : uris) {
    if (uri.empty())
      continue;
    eraseCachedUriLocked(uri);
    const std::string path = uriToPath(uri);
    if (!path.empty()) {
      const std::string normalizedPath = normalizePathForCache(path);
      auto pathIt = gDocumentCacheUrisByPath.find(normalizedPath);
      if (pathIt != gDocumentCacheUrisByPath.end()) {
        std::vector<std::string> urisByPath(pathIt->second.begin(),
                                            pathIt->second.end());
        gDocumentCacheUrisByPath.erase(pathIt);
        for (const auto &cachedUri : urisByPath) {
          auto cacheIt = gDocumentCacheByUri.find(cachedUri);
          if (cacheIt == gDocumentCacheByUri.end())
            continue;
          if (cacheIt->second.normalizedPath == normalizedPath)
            gDocumentCacheByUri.erase(cacheIt);
        }
      }
    }
  }
}
