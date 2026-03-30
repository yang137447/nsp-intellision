#include "document_owner.hpp"
#include "interactive_semantic_runtime.hpp"
#include "server_request_handlers.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>
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
}

void documentOwnerDidChange(const Document &document,
                            const std::vector<ChangedRange> &changedRanges,
                            const DocumentRuntimeUpdateOptions &options,
                            const ServerRequestContext &ctx) {
  auto owner = getOrCreateOwnerState(document.uri);
  std::lock_guard<std::mutex> ownerLock(owner->mutex);
  documentRuntimeUpsert(document, changedRanges, options);
  interactiveSemanticRuntimePrewarm(document.uri, document, ctx);
}

void documentOwnerDidClose(const std::string &uri) {
  auto owner = findOwnerState(uri);
  if (owner) {
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    documentRuntimeErase(uri);
  } else {
    documentRuntimeErase(uri);
  }
  std::lock_guard<std::mutex> lock(gDocumentOwnerMapMutex);
  gDocumentOwners.erase(uri);
}

void documentOwnerRefreshAnalysisContext(
    const DocumentRuntimeUpdateOptions &options,
    const ServerRequestContext &ctx) {
  documentRuntimeRefreshAnalysisKeys(options);
  for (const auto &entry : ctx.documents) {
    auto owner = getOrCreateOwnerState(entry.first);
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    interactiveSemanticRuntimePrewarm(entry.first, entry.second, ctx);
  }
}

void documentOwnerRefreshAnalysisContextForUris(
    const std::vector<std::string> &uris,
    const DocumentRuntimeUpdateOptions &options,
    const ServerRequestContext &ctx) {
  documentRuntimeRefreshAnalysisKeysForUris(uris, options);
  for (const auto &uri : uris) {
    auto it = ctx.documents.find(uri);
    if (it == ctx.documents.end())
      continue;
    auto owner = getOrCreateOwnerState(uri);
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    interactiveSemanticRuntimePrewarm(uri, it->second, ctx);
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

void documentOwnerStoreInteractiveSnapshot(
    const std::string &uri,
    const std::shared_ptr<const InteractiveSnapshot> &snapshot) {
  auto owner = findOwnerState(uri);
  if (owner) {
    std::lock_guard<std::mutex> ownerLock(owner->mutex);
    documentRuntimeStoreInteractiveSnapshot(uri, snapshot);
    return;
  }
  documentRuntimeStoreInteractiveSnapshot(uri, snapshot);
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
