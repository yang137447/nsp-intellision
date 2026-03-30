#include "main_include_graph_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "active_unit.hpp"
#include "crash_handler.hpp"
#include "definition_location.hpp"
#include "declaration_query.hpp"
#include "deferred_doc_runtime.hpp"
#include "diagnostics.hpp"
#include "document_owner.hpp"
#include "document_runtime.hpp"
#include "expanded_source.hpp"
#include "fast_ast.hpp"
#include "full_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_docs.hpp"
#include "include_resolver.hpp"
#include "immediate_syntax_diagnostics.hpp"
#include "indeterminate_reasons.hpp"
#include "interactive_semantic_runtime.hpp"
#include "json.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "nsf_lexer.hpp"
#include "preprocessor_view.hpp"
#include "semantic_snapshot.hpp"
#include "semantic_cache.hpp"
#include "server_documents.hpp"
#include "server_occurrences.hpp"
#include "server_parse.hpp"
#include "server_request_handlers.hpp"
#include "server_settings.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"
#include "workspace_summary_runtime.hpp"


std::atomic<bool> gSemanticCacheEnabled{true};
std::mutex gIncludeGraphCacheMutex;
std::unordered_map<std::string, std::vector<std::string>>
    gIncludeGraphOrderedByRoot;
std::unordered_map<std::string, std::unordered_set<std::string>>
    gIncludeGraphRootsByUri;
IncludeGraphCacheMetricsSnapshot gIncludeGraphCacheMetrics;

SemanticCacheKey
makeSemanticCacheKeyForUri(const std::string &uri,
                           const std::vector<std::string> &workspaceFolders,
                           const std::vector<std::string> &includePaths,
                           const std::vector<std::string> &shaderExtensions) {
  SemanticCacheKey key;
  key.workspaceFolders = workspaceFolders;
  key.includePaths = includePaths;
  key.shaderExtensions = shaderExtensions;
  key.unitPath = uriToPath(uri);
  return key;
}

void eraseIncludeGraphRootLocked(const std::string &rootUri) {
  auto rootIt = gIncludeGraphOrderedByRoot.find(rootUri);
  if (rootIt == gIncludeGraphOrderedByRoot.end())
    return;
  for (const auto &depUri : rootIt->second) {
    auto depIt = gIncludeGraphRootsByUri.find(depUri);
    if (depIt == gIncludeGraphRootsByUri.end())
      continue;
    depIt->second.erase(rootUri);
    if (depIt->second.empty())
      gIncludeGraphRootsByUri.erase(depIt);
  }
  gIncludeGraphOrderedByRoot.erase(rootIt);
}

void collectIncludeGraphUrisUncached(
    const std::string &nodeUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited,
    std::vector<std::string> &orderedUris) {
  if (!visited.insert(nodeUri).second)
    return;
  orderedUris.push_back(nodeUri);
  std::string text;
  if (!loadDocumentText(nodeUri, documents, text))
    return;
  std::istringstream stream(text);
  std::string lineText;
  while (std::getline(stream, lineText)) {
    std::string includePath;
    if (!extractIncludePath(lineText, includePath))
      continue;
    auto candidates = resolveIncludeCandidates(
        nodeUri, includePath, workspaceFolders, includePaths, shaderExtensions);
    std::vector<std::string> candidateUris;
    candidateUris.reserve(candidates.size());
    for (const auto &candidate : candidates) {
      struct _stat statBuffer;
      if (_stat(candidate.c_str(), &statBuffer) != 0)
        continue;
      candidateUris.push_back(pathToUri(candidate));
    }
    prefetchDocumentTexts(candidateUris, documents);
    for (const auto &candidateUri : candidateUris) {
      collectIncludeGraphUrisUncached(candidateUri, documents, workspaceFolders,
                                      includePaths, shaderExtensions, visited,
                                      orderedUris);
      break;
    }
  }
}

static std::vector<std::string> getIncludeGraphUrisCachedImpl(
    const std::string &rootUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions, bool allowSemanticCache) {
  uint64_t rootEpoch = 0;
  auto rootIt = documents.find(rootUri);
  if (rootIt != documents.end())
    rootEpoch = rootIt->second.epoch;
  if (allowSemanticCache &&
      gSemanticCacheEnabled.load(std::memory_order_relaxed)) {
    SemanticCacheKey key = makeSemanticCacheKeyForUri(
        rootUri, workspaceFolders, includePaths, shaderExtensions);
    auto snapshot = semanticCacheGetSnapshot(key, rootUri, rootEpoch);
    if (snapshot && snapshot->includeGraphComplete)
      return snapshot->includeGraphUrisOrdered;
  }
  {
    std::lock_guard<std::mutex> lock(gIncludeGraphCacheMutex);
    gIncludeGraphCacheMetrics.lookups++;
    auto cached = gIncludeGraphOrderedByRoot.find(rootUri);
    if (cached != gIncludeGraphOrderedByRoot.end()) {
      gIncludeGraphCacheMetrics.cacheHits++;
      return cached->second;
    }
  }

  std::unordered_set<std::string> visited;
  std::vector<std::string> orderedUris;
  collectIncludeGraphUrisUncached(rootUri, documents, workspaceFolders,
                                  includePaths, shaderExtensions, visited,
                                  orderedUris);

  {
    std::lock_guard<std::mutex> lock(gIncludeGraphCacheMutex);
    gIncludeGraphCacheMetrics.rebuilds++;
    eraseIncludeGraphRootLocked(rootUri);
    gIncludeGraphOrderedByRoot[rootUri] = orderedUris;
    for (const auto &depUri : orderedUris)
      gIncludeGraphRootsByUri[depUri].insert(rootUri);
  }
  if (allowSemanticCache &&
      gSemanticCacheEnabled.load(std::memory_order_relaxed)) {
    SemanticCacheKey key = makeSemanticCacheKeyForUri(
        rootUri, workspaceFolders, includePaths, shaderExtensions);
    SemanticSnapshot created;
    created.uri = rootUri;
    created.documentEpoch = rootEpoch;
    created.includeGraphUrisOrdered = orderedUris;
    created.includeGraphComplete = true;
    semanticCacheUpsertSnapshot(key, created);
  }
  return orderedUris;
}

std::vector<std::string> getIncludeGraphUrisCached(
    const std::string &rootUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions) {
  return getIncludeGraphUrisCachedImpl(rootUri, documents, workspaceFolders,
                                       includePaths, shaderExtensions, true);
}

void invalidateIncludeGraphCacheByUri(const std::string &changedUri) {
  if (changedUri.empty())
    return;
  semanticCacheInvalidateUri(changedUri);
  std::lock_guard<std::mutex> lock(gIncludeGraphCacheMutex);
  gIncludeGraphCacheMetrics.invalidations++;
  std::vector<std::string> impactedRoots;
  auto depIt = gIncludeGraphRootsByUri.find(changedUri);
  if (depIt != gIncludeGraphRootsByUri.end())
    impactedRoots.assign(depIt->second.begin(), depIt->second.end());
  if (gIncludeGraphOrderedByRoot.find(changedUri) !=
      gIncludeGraphOrderedByRoot.end()) {
    impactedRoots.push_back(changedUri);
  }
  std::sort(impactedRoots.begin(), impactedRoots.end());
  impactedRoots.erase(std::unique(impactedRoots.begin(), impactedRoots.end()),
                      impactedRoots.end());
  for (const auto &rootUri : impactedRoots)
    eraseIncludeGraphRootLocked(rootUri);
}

void invalidateIncludeGraphCacheByUris(
    const std::vector<std::string> &changedUris) {
  for (const auto &changedUri : changedUris)
    invalidateIncludeGraphCacheByUri(changedUri);
}

IncludeGraphCacheMetricsSnapshot takeIncludeGraphCacheMetricsSnapshot() {
  std::lock_guard<std::mutex> lock(gIncludeGraphCacheMutex);
  IncludeGraphCacheMetricsSnapshot snapshot = gIncludeGraphCacheMetrics;
  gIncludeGraphCacheMetrics = IncludeGraphCacheMetricsSnapshot{};
  return snapshot;
}

void invalidateAllIncludeGraphCaches() {
  std::lock_guard<std::mutex> lock(gIncludeGraphCacheMutex);
  gIncludeGraphCacheMetrics.invalidations++;
  gIncludeGraphOrderedByRoot.clear();
  gIncludeGraphRootsByUri.clear();
}
