#pragma once

#include "document_runtime.hpp"

#include <memory>
#include <string>

struct ServerRequestContext;

// Single-owner orchestration API for opened documents.
//
// Responsibilities:
// - serialize per-document runtime mutations behind one owner mutex
// - ensure didOpen/didChange/refresh flows all switch analysis context through
//   document_runtime.* before publishing new snapshots
// - prewarm interactive snapshots after document edits or context refreshes
//
// Non-goals:
// - does not answer LSP queries directly
// - does not own the semantic/deferred build logic; it only orders publication

// Registers an opened document with the owner and prewarms the current-doc
// interactive snapshot for the new analysis context.
void documentOwnerDidOpen(const Document &document,
                          const DocumentRuntimeUpdateOptions &options,
                          const ServerRequestContext &ctx);

// Applies a didChange payload for one opened document, updates the runtime key,
// and triggers current-doc interactive prewarm on the serialized owner path.
//
// didChange still routes through owner-side interactive prewarm so current-doc
// semantic state stays published for follow-up requests and runtime-debug
// inspection after the text update.
void documentOwnerDidChange(const Document &document,
                            const std::vector<ChangedRange> &changedRanges,
                            const DocumentRuntimeUpdateOptions &options,
                            const ServerRequestContext &ctx);

// Drops owner state and the associated document runtime entry for a closed uri.
void documentOwnerDidClose(const std::string &uri);

// Refreshes analysis context for all opened documents after active unit,
// configuration, resource model, or workspace summary changes.
//
// Stable-context reuse remains the responsibility of document_runtime.* and the
// interactive/deferred runtimes; callers should use this entry point instead of
// mutating document_runtime.* directly.
void documentOwnerRefreshAnalysisContext(
    const DocumentRuntimeUpdateOptions &options,
    const ServerRequestContext &ctx);

// Same as documentOwnerRefreshAnalysisContext, but scoped to the affected open
// uris (for example reverse-include or file-watch refreshes).
void documentOwnerRefreshAnalysisContextForUris(
    const std::vector<std::string> &uris,
    const DocumentRuntimeUpdateOptions &options,
    const ServerRequestContext &ctx);

// Returns a copy of the latest published runtime snapshot for one opened
// document. Callers must treat the returned copy as read-only.
bool documentOwnerGetRuntime(const std::string &uri, DocumentRuntime &runtimeOut);

// Publishes the latest immediate-syntax snapshot for a document if the version
// still matches the owner-held runtime.
void documentOwnerUpdateImmediateSyntaxSnapshot(
    const std::string &uri, const ImmediateSyntaxSnapshot &snapshot);

// Publishes an already-built interactive snapshot for the matching analysis key.
void documentOwnerStoreInteractiveSnapshot(
    const std::string &uri,
    const std::shared_ptr<const InteractiveSnapshot> &snapshot);

// Publishes an already-built deferred snapshot for the matching analysis key.
void documentOwnerStoreDeferredSnapshot(
    const std::string &uri,
    const std::shared_ptr<const DeferredDocSnapshot> &snapshot);
