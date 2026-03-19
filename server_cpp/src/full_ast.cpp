#include "full_ast.hpp"

#include "ast_signature_index.hpp"
#include "server_parse.hpp"

#include <mutex>
#include <sstream>
#include <unordered_map>

namespace {

struct FullAstCacheEntry {
  uint64_t epoch = 0;
  uint64_t fingerprint = 0;
  bool hasEpoch = false;
  uint64_t functionCount = 0;
  uint64_t includeCount = 0;
  std::vector<std::string> includePaths;
  std::vector<AstFunctionSignatureEntry> functions;
  std::unordered_map<std::string, std::vector<size_t>> functionsByName;
};

std::mutex gFullAstMutex;
std::unordered_map<std::string, FullAstCacheEntry> gFullAstByUri;
FullAstMetricsSnapshot gFullAstMetrics;

uint64_t hashTextFnv1a(const std::string &text) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char ch : text) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

FullAstCacheEntry buildFullAst(const std::string &text) {
  FullAstCacheEntry built;
  std::istringstream stream(text);
  std::string lineText;
  indexFunctionSignatures(text, built.functions, built.functionsByName);
  built.functionCount = static_cast<uint64_t>(built.functions.size());
  while (std::getline(stream, lineText)) {
    std::string includePath;
    if (extractIncludePath(lineText, includePath)) {
      built.includeCount++;
      built.includePaths.push_back(includePath);
    }
  }
  return built;
}

bool ensureFullAstCached(const std::string &uri, const std::string &text,
                         uint64_t epoch) {
  const uint64_t fingerprint = hashTextFnv1a(text);
  {
    std::lock_guard<std::mutex> lock(gFullAstMutex);
    gFullAstMetrics.lookups++;
    auto it = gFullAstByUri.find(uri);
    if (it != gFullAstByUri.end()) {
      const bool epochMatches =
          it->second.hasEpoch && epoch > 0 && it->second.epoch == epoch;
      const bool textMatches = it->second.fingerprint == fingerprint;
      if (epochMatches || textMatches) {
        gFullAstMetrics.cacheHits++;
        return true;
      }
    }
  }

  FullAstCacheEntry rebuilt = buildFullAst(text);
  rebuilt.epoch = epoch;
  rebuilt.hasEpoch = epoch > 0;
  rebuilt.fingerprint = fingerprint;
  std::lock_guard<std::mutex> lock(gFullAstMutex);
  gFullAstMetrics.rebuilds++;
  gFullAstMetrics.functionsIndexed += rebuilt.functionCount;
  gFullAstMetrics.includesIndexed += rebuilt.includeCount;
  gFullAstByUri[uri] = std::move(rebuilt);
  return true;
}

bool findFullAstSignature(const FullAstCacheEntry &entry,
                          const std::string &name, int lineIndex,
                          int nameCharacter, std::string &labelOut,
                          std::vector<std::string> &parametersOut) {
  auto it = entry.functionsByName.find(name);
  if (it == entry.functionsByName.end() || it->second.empty())
    return false;
  const auto &indices = it->second;
  for (size_t index : indices) {
    const auto &item = entry.functions[index];
    if (item.line == lineIndex && item.character == nameCharacter) {
      labelOut = item.label;
      parametersOut = item.parameters;
      return true;
    }
  }
  for (size_t index : indices) {
    const auto &item = entry.functions[index];
    if (item.line == lineIndex) {
      labelOut = item.label;
      parametersOut = item.parameters;
      return true;
    }
  }
  const auto &fallback = entry.functions[indices.front()];
  labelOut = fallback.label;
  parametersOut = fallback.parameters;
  return true;
}

} // namespace

void updateFullAstForDocument(const std::string &uri, const std::string &text,
                              uint64_t epoch) {
  if (uri.empty())
    return;
  ensureFullAstCached(uri, text, epoch);
}

bool queryFullAstIncludes(const std::string &uri, const std::string &text,
                          uint64_t epoch,
                          std::vector<std::string> &includePathsOut) {
  includePathsOut.clear();
  if (uri.empty())
    return false;
  ensureFullAstCached(uri, text, epoch);
  std::lock_guard<std::mutex> lock(gFullAstMutex);
  auto it = gFullAstByUri.find(uri);
  if (it == gFullAstByUri.end())
    return false;
  includePathsOut = it->second.includePaths;
  return true;
}

bool queryFullAstFunctionSignature(const std::string &uri,
                                   const std::string &text, uint64_t epoch,
                                   const std::string &name, int lineIndex,
                                   int nameCharacter, std::string &labelOut,
                                   std::vector<std::string> &parametersOut) {
  labelOut.clear();
  parametersOut.clear();
  if (uri.empty() || name.empty())
    return false;
  ensureFullAstCached(uri, text, epoch);
  std::lock_guard<std::mutex> lock(gFullAstMutex);
  auto it = gFullAstByUri.find(uri);
  if (it == gFullAstByUri.end())
    return false;
  return findFullAstSignature(it->second, name, lineIndex, nameCharacter,
                              labelOut, parametersOut);
}

void invalidateFullAstByUri(const std::string &uri) {
  if (uri.empty())
    return;
  std::lock_guard<std::mutex> lock(gFullAstMutex);
  gFullAstMetrics.invalidations++;
  gFullAstByUri.erase(uri);
}

void invalidateFullAstByUris(const std::vector<std::string> &uris) {
  if (uris.empty())
    return;
  std::lock_guard<std::mutex> lock(gFullAstMutex);
  for (const auto &uri : uris) {
    if (uri.empty())
      continue;
    gFullAstMetrics.invalidations++;
    gFullAstByUri.erase(uri);
  }
}

FullAstMetricsSnapshot takeFullAstMetricsSnapshot() {
  std::lock_guard<std::mutex> lock(gFullAstMutex);
  FullAstMetricsSnapshot snapshot = gFullAstMetrics;
  snapshot.documentsCached = static_cast<uint64_t>(gFullAstByUri.size());
  gFullAstMetrics = FullAstMetricsSnapshot{};
  return snapshot;
}
