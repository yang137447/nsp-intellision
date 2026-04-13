#include "document_owner.hpp"
#include "interactive_visibility_runtime.hpp"
#include "interactive_semantic_runtime.hpp"
#include "main_did_change_classification.hpp"
#include "server_request_handlers.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct DocumentOwnerState {
  std::mutex mutex;
};

std::mutex gDocumentOwnerMapMutex;
std::unordered_map<std::string, std::shared_ptr<DocumentOwnerState>>
    gDocumentOwners;

std::shared_ptr<DocumentOwnerState>
getOrCreateOwnerState(const std::string &uri) {
  std::lock_guard<std::mutex> lock(gDocumentOwnerMapMutex);
  auto it = gDocumentOwners.find(uri);
  if (it != gDocumentOwners.end())
    return it->second;
  auto owner = std::make_shared<DocumentOwnerState>();
  gDocumentOwners[uri] = owner;
  return owner;
}

std::shared_ptr<DocumentOwnerState>
findOwnerState(const std::string &uri) {
  std::lock_guard<std::mutex> lock(gDocumentOwnerMapMutex);
  auto it = gDocumentOwners.find(uri);
  if (it == gDocumentOwners.end())
    return nullptr;
  return it->second;
}

} // namespace

void documentOwnerDidOpen(const Document &document,
                          const DocumentRuntimeUpdateOptions &options,
                          const ServerRequestContext &ctx) {
  auto owner = getOrCreateOwnerState(document.uri);
  std::lock_guard<std::mutex> ownerLock(owner->mutex);
  documentRuntimeUpsert(document, {}, options);
  interactiveSemanticRuntimePrewarm(document.uri, document, ctx);
  DocumentRuntime runtime;
  if (documentRuntimeGet(document.uri, runtime))
    interactiveVisibilityRuntimePrewarm(runtime);
}

void documentOwnerDidChange(const Document &document,
                            const std::vector<ChangedRange> &changedRanges,
                            const DocumentRuntimeUpdateOptions &options,
                            const ServerRequestContext &ctx) {
  const auto startedAt = std::chrono::steady_clock::now();
  auto owner = getOrCreateOwnerState(document.uri);
  std::lock_guard<std::mutex> ownerLock(owner->mutex);
  documentRuntimeUpsert(document, changedRanges, options);
  bool shouldPrewarm = true;
  DocumentRuntime runtime;
  if (documentRuntimeGet(document.uri, runtime)) {
    const bool commentOnlyEdit =
        isCommentOnlyEditForDidChange(document.text, changedRanges);
    shouldPrewarm =
        !((runtime.syntaxOnlyEditHint || commentOnlyEdit) &&
          runtime.lastGoodCurrentDocSemanticSnapshot != nullptr);
  }
  if (shouldPrewarm)
    interactiveSemanticRuntimePrewarm(document.uri, document, ctx);
  if (documentRuntimeGet(document.uri, runtime))
    interactiveVisibilityRuntimePrewarm(runtime);
  recordInteractiveOwnerDidChange(
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - startedAt)
          .count());
}

void documentOwnerDidClose(const std::string &uri) {
  InteractiveVisibilityKey visibilityKey;
  auto owner = findOwnerState(uri);
  if (owner) {
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    DocumentRuntime runtime;
    if (documentRuntimeGet(uri, runtime))
      visibilityKey = runtime.interactiveVisibilityKey;
    documentRuntimeErase(uri);
  } else {
    DocumentRuntime runtime;
    if (documentRuntimeGet(uri, runtime))
      visibilityKey = runtime.interactiveVisibilityKey;
    documentRuntimeErase(uri);
  }
  {
    std::lock_guard<std::mutex> lock(gDocumentOwnerMapMutex);
    gDocumentOwners.erase(uri);
  }
  if (!visibilityKey.fullFingerprint.empty() &&
      !documentRuntimeAnyUsesInteractiveVisibilityFingerprint(
          visibilityKey.fullFingerprint)) {
    interactiveVisibilityRuntimeInvalidateKey(visibilityKey);
  }
}

void documentOwnerRefreshAnalysisContext(
    const DocumentRuntimeUpdateOptions &options,
    const ServerRequestContext &ctx) {
  interactiveVisibilityRuntimeInvalidateAll();
  const auto globalContextSnapshot =
      globalContextRuntimeRefresh(options.globalContextOptions);
  documentRuntimeRefreshAnalysisKeys(options, globalContextSnapshot);
  for (const auto &entry : ctx.documents) {
    auto owner = getOrCreateOwnerState(entry.first);
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    interactiveSemanticRuntimePrewarm(entry.first, entry.second, ctx);
    DocumentRuntime runtime;
    if (documentRuntimeGet(entry.first, runtime))
      interactiveVisibilityRuntimePrewarm(runtime);
  }
}

void documentOwnerRefreshAnalysisContextForUris(
    const std::vector<std::string> &uris,
    const DocumentRuntimeUpdateOptions &options,
    const ServerRequestContext &ctx) {
  std::vector<InteractiveVisibilityKey> staleVisibilityKeys;
  staleVisibilityKeys.reserve(uris.size());
  std::unordered_set<std::string> staleFingerprints;
  staleFingerprints.reserve(uris.size());
  for (const auto &uri : uris) {
    DocumentRuntime runtime;
    if (!documentRuntimeGet(uri, runtime))
      continue;
    const std::string &fingerprint =
        runtime.interactiveVisibilityKey.fullFingerprint;
    if (fingerprint.empty() || !staleFingerprints.insert(fingerprint).second)
      continue;
    staleVisibilityKeys.push_back(runtime.interactiveVisibilityKey);
  }

  const auto globalContextSnapshot =
      globalContextRuntimeRefresh(options.globalContextOptions);
  documentRuntimeRefreshAnalysisKeysForUris(uris, options,
                                            globalContextSnapshot);
  for (const auto &key : staleVisibilityKeys)
    interactiveVisibilityRuntimeInvalidateKey(key);

  for (const auto &uri : uris) {
    auto it = ctx.documents.find(uri);
    if (it == ctx.documents.end())
      continue;
    auto owner = getOrCreateOwnerState(uri);
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    interactiveSemanticRuntimePrewarm(uri, it->second, ctx);
    DocumentRuntime runtime;
    if (documentRuntimeGet(uri, runtime))
      interactiveVisibilityRuntimePrewarm(runtime);
  }
}

bool documentOwnerGetRuntime(const std::string &uri,
                             DocumentRuntime &runtimeOut) {
  auto owner = findOwnerState(uri);
  if (owner) {
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    return documentRuntimeGet(uri, runtimeOut);
  }
  return documentRuntimeGet(uri, runtimeOut);
}

void documentOwnerUpdateImmediateSyntaxSnapshot(
    const std::string &uri, const ImmediateSyntaxSnapshot &snapshot) {
  auto owner = findOwnerState(uri);
  if (owner) {
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    documentRuntimeUpdateImmediateSyntaxSnapshot(uri, snapshot);
    return;
  }
  documentRuntimeUpdateImmediateSyntaxSnapshot(uri, snapshot);
}

void documentOwnerUpdateLastDiagnosticsPublishLayer(
    const std::string &uri, uint64_t documentEpoch, int documentVersion,
    const std::string &layer) {
  auto owner = findOwnerState(uri);
  if (owner) {
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    documentRuntimeUpdateLastDiagnosticsPublishLayer(uri, documentEpoch,
                                                     documentVersion, layer);
    return;
  }
  documentRuntimeUpdateLastDiagnosticsPublishLayer(uri, documentEpoch,
                                                   documentVersion, layer);
}

void documentOwnerStoreCurrentDocSemanticSnapshot(
    const std::string &uri,
    const std::shared_ptr<const InteractiveSnapshot> &snapshot) {
  auto owner = findOwnerState(uri);
  if (owner) {
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    documentRuntimeStoreCurrentDocSemanticSnapshot(uri, snapshot);
    return;
  }
  documentRuntimeStoreCurrentDocSemanticSnapshot(uri, snapshot);
}

void documentOwnerStoreDeferredSnapshot(
    const std::string &uri,
    const std::shared_ptr<const DeferredDocSnapshot> &snapshot) {
  auto owner = findOwnerState(uri);
  if (owner) {
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    documentRuntimeStoreDeferredSnapshot(uri, snapshot);
    return;
  }
  documentRuntimeStoreDeferredSnapshot(uri, snapshot);
}

void documentOwnerMergeAndStoreDeferredSnapshot(
    const std::string &uri,
    const std::shared_ptr<const DeferredDocSnapshot> &snapshot) {
  auto owner = findOwnerState(uri);
  if (owner) {
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    documentRuntimeMergeAndStoreDeferredSnapshot(uri, snapshot);
    return;
  }
  documentRuntimeMergeAndStoreDeferredSnapshot(uri, snapshot);
}
