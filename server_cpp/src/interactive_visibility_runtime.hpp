#pragma once

#include "document_runtime.hpp"
#include "workspace_index.hpp"

#include <vector>

// Shared interactive visibility shard cache.
//
// This module is the runtime boundary for cross-file symbols that are visible
// under one InteractiveVisibilityKey.
//
// Current contract:
// - runtime state is a best-effort process-global shard cache
// - interactiveVisibilityRuntimeGet(...) lookup is keyed by
//   InteractiveVisibilityKey::fullFingerprint and requires a non-empty key
// - prewarm builds shard contents from the active-unit include closure
// - key-scoped invalidation is available for close-path cleanup; full
//   invalidation remains available for broader analysis-context refresh
//
// Non-goals:
// - does not own completion ordering policy
// - does not schedule workspace-summary refreshes by itself

struct InteractiveVisibleSymbolShard {
  InteractiveVisibilityKey key;
  std::vector<IndexedDefinition> functions;
  std::vector<IndexedDefinition> globals;
  std::vector<IndexedDefinition> types;
};

bool interactiveVisibilityRuntimeGet(const InteractiveVisibilityKey &key,
                                     InteractiveVisibleSymbolShard &out);
void interactiveVisibilityRuntimePrewarm(const DocumentRuntime &runtime);
bool interactiveVisibilityRuntimeCollectFunctions(
    const InteractiveVisibilityKey &key,
    std::vector<IndexedDefinition> &functionsOut);
void interactiveVisibilityRuntimeInvalidateKey(
    const InteractiveVisibilityKey &key);
void interactiveVisibilityRuntimeInvalidateAll();
