#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FullAstMetricsSnapshot {
  uint64_t lookups = 0;
  uint64_t cacheHits = 0;
  uint64_t rebuilds = 0;
  uint64_t invalidations = 0;
  uint64_t functionsIndexed = 0;
  uint64_t includesIndexed = 0;
  uint64_t documentsCached = 0;
};

void updateFullAstForDocument(const std::string &uri, const std::string &text,
                              uint64_t epoch);

bool queryFullAstIncludes(const std::string &uri, const std::string &text,
                          uint64_t epoch,
                          std::vector<std::string> &includePathsOut);

void invalidateFullAstByUri(const std::string &uri);

void invalidateFullAstByUris(const std::vector<std::string> &uris);

void invalidateAllFullAstCaches();

FullAstMetricsSnapshot takeFullAstMetricsSnapshot();
