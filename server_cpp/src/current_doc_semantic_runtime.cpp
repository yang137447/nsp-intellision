#include "current_doc_semantic_runtime.hpp"

#include "semantic_snapshot.hpp"
#include "server_request_handlers.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

namespace {

uint64_t currentTimeMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool isSnapshotStillCurrent(const AnalysisSnapshotKey &candidate,
                            const AnalysisSnapshotKey &current) {
  return candidate.fullFingerprint == current.fullFingerprint;
}

bool isSnapshotStaleEligible(const AnalysisSnapshotKey &candidate,
                             const AnalysisSnapshotKey &current) {
  return candidate.stableContextFingerprint == current.stableContextFingerprint;
}

struct RuntimeSemanticInputs {
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  std::unordered_map<std::string, int> defines;
};

const std::shared_ptr<const InteractiveSnapshot> &
currentDocSemanticSnapshotView(const DocumentRuntime &runtime) {
  return runtime.currentDocSemanticSnapshot;
}

const std::shared_ptr<const InteractiveSnapshot> &
lastGoodCurrentDocSemanticSnapshotView(const DocumentRuntime &runtime) {
  return runtime.lastGoodCurrentDocSemanticSnapshot;
}

RuntimeSemanticInputs getRuntimeSemanticInputs(const DocumentRuntime &runtime,
                                               const ServerRequestContext &ctx) {
  RuntimeSemanticInputs inputs;
  if (!runtime.activeUnitSnapshot.workspaceFolders.empty() ||
      !runtime.activeUnitSnapshot.includePaths.empty() ||
      !runtime.activeUnitSnapshot.shaderExtensions.empty() ||
      !runtime.activeUnitSnapshot.defines.empty()) {
    inputs.workspaceFolders = runtime.activeUnitSnapshot.workspaceFolders;
    inputs.includePaths = runtime.activeUnitSnapshot.includePaths;
    inputs.shaderExtensions = runtime.activeUnitSnapshot.shaderExtensions;
    inputs.defines = runtime.activeUnitSnapshot.defines;
    return inputs;
  }
  inputs.workspaceFolders = ctx.workspaceFolders;
  inputs.includePaths = ctx.includePaths;
  inputs.shaderExtensions = ctx.shaderExtensions;
  inputs.defines = ctx.preprocessorDefines;
  return inputs;
}

} // namespace

std::shared_ptr<const InteractiveSnapshot>
currentDocSemanticRuntimeGetCurrentSnapshot(const DocumentRuntime &runtime,
                                           const Document &doc) {
  const auto &current = currentDocSemanticSnapshotView(runtime);
  if (!current)
    return nullptr;
  if (current->documentEpoch != doc.epoch || current->documentVersion != doc.version)
    return nullptr;
  if (!isSnapshotStillCurrent(current->key, runtime.analysisSnapshotKey))
    return nullptr;
  return current;
}

std::shared_ptr<const InteractiveSnapshot>
currentDocSemanticRuntimePromoteLastGoodSnapshot(const Document &doc,
                                                 const DocumentRuntime &runtime) {
  const auto &lastGood = lastGoodCurrentDocSemanticSnapshotView(runtime);
  if (!lastGood)
    return nullptr;
  if (!isSnapshotStaleEligible(lastGood->key, runtime.analysisSnapshotKey))
    return nullptr;

  auto promoted = std::make_shared<InteractiveSnapshot>(*lastGood);
  promoted->key = runtime.analysisSnapshotKey;
  promoted->documentEpoch = doc.epoch;
  promoted->documentVersion = doc.version;
  promoted->builtAtMs = currentTimeMs();
  return promoted;
}

std::shared_ptr<const InteractiveSnapshot>
currentDocSemanticRuntimeBuildSnapshot(const std::string &uri,
                                       const Document &doc,
                                       const ServerRequestContext &ctx,
                                       const DocumentRuntime &runtime) {
  const RuntimeSemanticInputs semanticInputs =
      getRuntimeSemanticInputs(runtime, ctx);
  auto semanticSnapshot = getSemanticSnapshotView(
      uri, doc.text, doc.epoch, semanticInputs.workspaceFolders,
      semanticInputs.includePaths, semanticInputs.shaderExtensions,
      semanticInputs.defines);
  if (!semanticSnapshot)
    return nullptr;

  auto snapshot = std::make_shared<InteractiveSnapshot>();
  snapshot->key = runtime.analysisSnapshotKey;
  snapshot->documentEpoch = doc.epoch;
  snapshot->documentVersion = doc.version;
  snapshot->semanticSnapshot = std::move(semanticSnapshot);
  snapshot->builtAtMs = currentTimeMs();
  return snapshot;
}

void currentDocSemanticRuntimeCollectEligibleSnapshots(
    const DocumentRuntime &runtime,
    std::shared_ptr<const InteractiveSnapshot> &lastGoodOut,
    std::shared_ptr<const DeferredDocSnapshot> &deferredOut) {
  lastGoodOut.reset();
  deferredOut.reset();

  const auto &lastGood = lastGoodCurrentDocSemanticSnapshotView(runtime);
  if (lastGood &&
      isSnapshotStaleEligible(lastGood->key, runtime.analysisSnapshotKey)) {
    lastGoodOut = lastGood;
  }
  if (runtime.deferredDocSnapshot &&
      isSnapshotStaleEligible(runtime.deferredDocSnapshot->key,
                              runtime.analysisSnapshotKey)) {
    deferredOut = runtime.deferredDocSnapshot;
  }
}
