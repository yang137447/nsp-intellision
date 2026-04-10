#pragma once

#include "document_runtime.hpp"

#include <memory>
#include <string>

struct Document;
struct ServerRequestContext;

// Explicit current-document semantic runtime boundary.
//
// Responsibilities:
// - own the publish/readiness rules for current-doc semantic snapshots
// - treat DocumentRuntime.currentDocSemanticSnapshot / lastGoodCurrentDocSemanticSnapshot
//   as the authoritative current-doc semantic source
// - decide when stale-eligible last-good semantic state can be promoted into the
//   current document version
// - build current-doc semantic snapshots from the shared semantic snapshot layer
//
// Non-goals:
// - does not answer hover/completion/signature/definition requests directly
// - does not schedule request lanes or background workers

// Returns the explicitly published current-doc semantic snapshot when it still
// matches the current document version and analysis key.
std::shared_ptr<const InteractiveSnapshot>
currentDocSemanticRuntimeGetCurrentSnapshot(const DocumentRuntime &runtime,
                                           const Document &doc);

// Returns a promoted current-doc semantic snapshot sourced from stale-eligible
// last-good state, or nullptr if the edit cannot safely reuse that semantic
// state. The returned snapshot is updated to the current document version/key
// and is intended to be published immediately as explicit current-doc runtime
// readiness.
std::shared_ptr<const InteractiveSnapshot>
currentDocSemanticRuntimePromoteLastGoodSnapshot(const Document &doc,
                                                 const DocumentRuntime &runtime);

// Builds a fresh current-doc semantic snapshot for the current document/runtime
// context. Callers are responsible for publishing the result if it still matches
// the current runtime.
std::shared_ptr<const InteractiveSnapshot>
currentDocSemanticRuntimeBuildSnapshot(const std::string &uri,
                                       const Document &doc,
                                       const ServerRequestContext &ctx,
                                       const DocumentRuntime &runtime);

// Collects stale-eligible current-doc last-good and deferred semantic snapshots
// for interactive query merge order.
void currentDocSemanticRuntimeCollectEligibleSnapshots(
    const DocumentRuntime &runtime,
    std::shared_ptr<const InteractiveSnapshot> &lastGoodOut,
    std::shared_ptr<const DeferredDocSnapshot> &deferredOut);
