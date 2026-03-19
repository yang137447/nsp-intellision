#include "semantic_cache.hpp"

#include <algorithm>
#include <sstream>

namespace {
void appendJoined(std::ostringstream &oss,
                  const std::vector<std::string> &items) {
  for (const auto &item : items) {
    oss << item.size() << ":" << item << ";";
  }
}

SemanticCacheManager gSemanticCacheManager;
} // namespace

std::shared_ptr<const SemanticSnapshot>
SemanticCacheManager::getSnapshot(const SemanticCacheKey &key,
                                  const std::string &uri,
                                  uint64_t documentEpoch) const {
  const std::string cacheKey = makeKeyFingerprint(key);
  std::lock_guard<std::mutex> lock(mutex);
  auto byKeyIt = snapshotsByKey.find(cacheKey);
  if (byKeyIt == snapshotsByKey.end()) {
    snapshotMissCount.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  auto uriIt = byKeyIt->second.find(uri);
  if (uriIt == byKeyIt->second.end() ||
      uriIt->second.documentEpoch != documentEpoch) {
    snapshotMissCount.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  snapshotHitCount.fetch_add(1, std::memory_order_relaxed);
  return std::make_shared<SemanticSnapshot>(uriIt->second);
}

void SemanticCacheManager::upsertSnapshot(const SemanticCacheKey &key,
                                          const SemanticSnapshot &snapshot) {
  const std::string cacheKey = makeKeyFingerprint(key);
  std::lock_guard<std::mutex> lock(mutex);
  auto byKeyIt = snapshotsByKey.find(cacheKey);
  if (byKeyIt != snapshotsByKey.end()) {
    auto rootIt = byKeyIt->second.find(snapshot.uri);
    if (rootIt != byKeyIt->second.end()) {
      for (const auto &depUri : rootIt->second.includeGraphUrisOrdered) {
        auto depIt = rootUrisByDependencyUri.find(depUri);
        if (depIt == rootUrisByDependencyUri.end())
          continue;
        depIt->second.erase(snapshot.uri);
        if (depIt->second.empty())
          rootUrisByDependencyUri.erase(depIt);
      }
    }
  }
  snapshotsByKey[cacheKey][snapshot.uri] = snapshot;
  for (const auto &depUri : snapshot.includeGraphUrisOrdered)
    rootUrisByDependencyUri[depUri].insert(snapshot.uri);
}

void SemanticCacheManager::invalidateUri(const std::string &uri) {
  std::lock_guard<std::mutex> lock(mutex);
  std::unordered_set<std::string> impactedRoots;
  auto depIt = rootUrisByDependencyUri.find(uri);
  if (depIt != rootUrisByDependencyUri.end()) {
    impactedRoots.insert(depIt->second.begin(), depIt->second.end());
    rootUrisByDependencyUri.erase(depIt);
  }
  impactedRoots.insert(uri);
  for (auto &entry : snapshotsByKey) {
    for (const auto &rootUri : impactedRoots) {
      auto rootIt = entry.second.find(rootUri);
      if (rootIt == entry.second.end())
        continue;
      for (const auto &depUri : rootIt->second.includeGraphUrisOrdered) {
        auto relatedDepIt = rootUrisByDependencyUri.find(depUri);
        if (relatedDepIt == rootUrisByDependencyUri.end())
          continue;
        relatedDepIt->second.erase(rootUri);
        if (relatedDepIt->second.empty())
          rootUrisByDependencyUri.erase(relatedDepIt);
      }
      entry.second.erase(rootIt);
    }
  }
}

SemanticCacheManagerStats SemanticCacheManager::consumeStats() {
  SemanticCacheManagerStats stats;
  stats.snapshotHit = snapshotHitCount.exchange(0, std::memory_order_relaxed);
  stats.snapshotMiss = snapshotMissCount.exchange(0, std::memory_order_relaxed);
  return stats;
}

std::string
SemanticCacheManager::makeKeyFingerprint(const SemanticCacheKey &key) const {
  std::ostringstream oss;
  appendJoined(oss, key.includePaths);
  oss << "|";
  appendJoined(oss, key.shaderExtensions);
  oss << "|";
  appendJoined(oss, key.workspaceFolders);
  oss << "|";
  oss << key.definesFingerprint << "|";
  oss << key.unitPath;
  return oss.str();
}

std::shared_ptr<const SemanticSnapshot>
semanticCacheGetSnapshot(const SemanticCacheKey &key, const std::string &uri,
                         uint64_t documentEpoch) {
  return gSemanticCacheManager.getSnapshot(key, uri, documentEpoch);
}

void semanticCacheUpsertSnapshot(const SemanticCacheKey &key,
                                 const SemanticSnapshot &snapshot) {
  gSemanticCacheManager.upsertSnapshot(key, snapshot);
}

void semanticCacheInvalidateUri(const std::string &uri) {
  gSemanticCacheManager.invalidateUri(uri);
}

SemanticCacheManagerStats semanticCacheConsumeStats() {
  return gSemanticCacheManager.consumeStats();
}
