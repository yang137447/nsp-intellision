#include "interactive_visibility_runtime.hpp"

#include "workspace_summary_runtime.hpp"

#include <mutex>
#include <unordered_map>

namespace {

std::mutex gInteractiveVisibilityMutex;
std::unordered_map<std::string, InteractiveVisibleSymbolShard>
    gInteractiveVisibilityShards;

} // namespace

bool interactiveVisibilityRuntimeGet(const InteractiveVisibilityKey &key,
                                     InteractiveVisibleSymbolShard &out) {
  if (key.fullFingerprint.empty())
    return false;
  std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
  auto it = gInteractiveVisibilityShards.find(key.fullFingerprint);
  if (it == gInteractiveVisibilityShards.end())
    return false;
  out = it->second;
  return true;
}

void interactiveVisibilityRuntimePrewarm(const DocumentRuntime &runtime) {
  const std::string &fingerprint =
      runtime.interactiveVisibilityKey.fullFingerprint;
  if (fingerprint.empty())
    return;

  {
    std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
    if (gInteractiveVisibilityShards.find(fingerprint) !=
        gInteractiveVisibilityShards.end()) {
      return;
    }
  }

  InteractiveVisibleSymbolShard shard;
  shard.key = runtime.interactiveVisibilityKey;
  for (const auto &uri : runtime.activeUnitSnapshot.includeClosureUris) {
    std::vector<IndexedDefinition> defs;
    workspaceSummaryRuntimeQueryDefinitionsByUri(uri, defs);
    for (const auto &def : defs) {
      if (def.kind == 12) {
        shard.functions.push_back(def);
      } else if (def.kind == 13) {
        shard.globals.push_back(def);
      } else if (def.kind == 23) {
        shard.types.push_back(def);
      }
    }
  }

  std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
  if (gInteractiveVisibilityShards.find(fingerprint) ==
      gInteractiveVisibilityShards.end()) {
    gInteractiveVisibilityShards[fingerprint] = std::move(shard);
  }
}

bool interactiveVisibilityRuntimeCollectFunctions(
    const InteractiveVisibilityKey &key,
    std::vector<IndexedDefinition> &functionsOut) {
  functionsOut.clear();
  if (key.fullFingerprint.empty())
    return false;
  std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
  auto it = gInteractiveVisibilityShards.find(key.fullFingerprint);
  if (it == gInteractiveVisibilityShards.end())
    return false;
  functionsOut = it->second.functions;
  return !functionsOut.empty();
}

bool interactiveVisibilityRuntimeCollectTypes(
    const InteractiveVisibilityKey &key,
    std::vector<IndexedDefinition> &typesOut) {
  typesOut.clear();
  if (key.fullFingerprint.empty())
    return false;
  std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
  auto it = gInteractiveVisibilityShards.find(key.fullFingerprint);
  if (it == gInteractiveVisibilityShards.end())
    return false;
  typesOut = it->second.types;
  return !typesOut.empty();
}

void interactiveVisibilityRuntimeInvalidateKey(
    const InteractiveVisibilityKey &key) {
  if (key.fullFingerprint.empty())
    return;
  std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
  gInteractiveVisibilityShards.erase(key.fullFingerprint);
}

void interactiveVisibilityRuntimeInvalidateAll() {
  std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
  gInteractiveVisibilityShards.clear();
}
