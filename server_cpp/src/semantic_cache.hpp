#pragma once

#include "type_eval.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SemanticCacheKey {
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  std::vector<std::string> workspaceFolders;
  std::string definesFingerprint;
  std::string unitPath;
};

struct SemanticSnapshot {
  std::string uri;
  uint64_t documentEpoch = 0;
  std::vector<std::string> includeGraphUrisOrdered;
};

struct SemanticCacheManagerStats {
  uint64_t snapshotHit = 0;
  uint64_t snapshotMiss = 0;
};

class SemanticCacheManager {
public:
  std::shared_ptr<const SemanticSnapshot>
  getSnapshot(const SemanticCacheKey &key, const std::string &uri,
              uint64_t documentEpoch) const;

  void upsertSnapshot(const SemanticCacheKey &key,
                      const SemanticSnapshot &snapshot);
  void invalidateUri(const std::string &uri);
  SemanticCacheManagerStats consumeStats();

private:
  std::string makeKeyFingerprint(const SemanticCacheKey &key) const;

  mutable std::mutex mutex;
  mutable std::atomic<uint64_t> snapshotHitCount{0};
  mutable std::atomic<uint64_t> snapshotMissCount{0};
  std::unordered_map<std::string,
                     std::unordered_map<std::string, SemanticSnapshot>>
      snapshotsByKey;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      rootUrisByDependencyUri;
};

std::shared_ptr<const SemanticSnapshot>
semanticCacheGetSnapshot(const SemanticCacheKey &key, const std::string &uri,
                         uint64_t documentEpoch);

void semanticCacheUpsertSnapshot(const SemanticCacheKey &key,
                                 const SemanticSnapshot &snapshot);

void semanticCacheInvalidateUri(const std::string &uri);

SemanticCacheManagerStats semanticCacheConsumeStats();
