#pragma once

#include "document_runtime.hpp"
#include "workspace_index.hpp"

#include <vector>

// Shared interactive visibility shard cache.
//
// This module is the runtime boundary for cross-file symbols that are visible
// under one InteractiveVisibilityKey.
// The current Task-2 implementation uses a process-global, mutex-protected
// skeleton cache that will later host shared-visible prewarmed shards.
//
// Task-2 scope:
// - define shard shape and lookup/invalidation APIs
// - provide a no-op-safe runtime skeleton without prewarm/build behavior

struct InteractiveVisibleSymbolShard {
  InteractiveVisibilityKey key;
  std::vector<IndexedDefinition> functions;
  std::vector<IndexedDefinition> globals;
  std::vector<IndexedDefinition> types;
};

bool interactiveVisibilityRuntimeGet(const InteractiveVisibilityKey &key,
                                     InteractiveVisibleSymbolShard &out);
void interactiveVisibilityRuntimeInvalidateAll();
