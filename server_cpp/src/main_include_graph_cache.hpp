#pragma once

#include "semantic_cache.hpp"
#include "server_documents.hpp"

#include <cstdint>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

struct IncludeGraphCacheMetricsSnapshot {
  uint64_t lookups = 0;
  uint64_t cacheHits = 0;
  uint64_t rebuilds = 0;
  uint64_t invalidations = 0;
};

// Entry-layer include-graph cache contract.
// Responsibilities: cache include closures for open-document workflows and
// expose cache invalidation/metrics helpers to main.cpp.
extern std::atomic<bool> gSemanticCacheEnabled;

SemanticCacheKey
makeSemanticCacheKeyForUri(const std::string &uri,
                           const std::vector<std::string> &workspaceFolders,
                           const std::vector<std::string> &includePaths,
                           const std::vector<std::string> &shaderExtensions);

std::vector<std::string> getIncludeGraphUrisCached(
    const std::string &rootUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions);

void invalidateIncludeGraphCacheByUri(const std::string &changedUri);

void invalidateIncludeGraphCacheByUris(
    const std::vector<std::string> &changedUris);

IncludeGraphCacheMetricsSnapshot takeIncludeGraphCacheMetricsSnapshot();

void invalidateAllIncludeGraphCaches();
