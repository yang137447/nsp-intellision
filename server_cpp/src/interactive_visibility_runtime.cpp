#include "interactive_visibility_runtime.hpp"

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

void interactiveVisibilityRuntimeInvalidateAll() {
  std::lock_guard<std::mutex> lock(gInteractiveVisibilityMutex);
  gInteractiveVisibilityShards.clear();
}
