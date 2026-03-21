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
  struct FunctionInfo {
    struct LocalInfo {
      std::string name;
      std::string type;
      size_t offset = 0;
      int depth = 0;
    };

    std::string name;
    int line = -1;
    int character = -1;
    std::string label;
    std::vector<std::string> parameters;
    std::vector<std::pair<std::string, std::string>> parameterInfos;
    std::string returnType;
    int signatureEndLine = -1;
    int bodyStartLine = -1;
    int bodyEndLine = -1;
    std::vector<LocalInfo> locals;
    bool hasBody = false;
  };

  struct FieldInfo {
    std::string name;
    std::string type;
    int line = -1;
  };

  struct StructInfo {
    std::string name;
    int line = -1;
    std::vector<FieldInfo> fields;
  };

  struct GlobalInfo {
    std::string name;
    std::string type;
    int line = -1;
  };

  std::string uri;
  uint64_t documentEpoch = 0;
  std::vector<std::string> includeGraphUrisOrdered;
  bool includeGraphComplete = false;
  std::vector<FunctionInfo> functions;
  std::unordered_map<std::string, std::vector<size_t>> functionsByName;
  std::vector<StructInfo> structs;
  std::unordered_map<std::string, size_t> structByName;
  std::vector<GlobalInfo> globals;
  std::unordered_map<std::string, size_t> globalByName;
  bool semanticDataComplete = false;
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
  void clear();
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

void semanticCacheInvalidateAll();

SemanticCacheManagerStats semanticCacheConsumeStats();
