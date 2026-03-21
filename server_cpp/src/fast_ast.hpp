#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FastAstMetricsSnapshot {
  uint64_t lookups = 0;
  uint64_t cacheHits = 0;
  uint64_t cacheReused = 0;
  uint64_t rebuilds = 0;
  uint64_t functionsIndexed = 0;
};

bool queryFastAstFunctionSignature(const std::string &uri,
                                   const std::string &text, uint64_t epoch,
                                   const std::string &name, int lineIndex,
                                   int nameCharacter, std::string &labelOut,
                                   std::vector<std::string> &parametersOut);

bool queryFastAstLocalType(const std::string &uri, const std::string &text,
                           uint64_t epoch, const std::string &name,
                           size_t maxOffset, std::string &typeNameOut);

void invalidateFastAstByUri(const std::string &uri);

void invalidateFastAstByUris(const std::vector<std::string> &uris);

void invalidateAllFastAstCaches();

FastAstMetricsSnapshot takeFastAstMetricsSnapshot();
