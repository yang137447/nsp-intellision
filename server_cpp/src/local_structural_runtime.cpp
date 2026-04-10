#include "local_structural_runtime.hpp"

LocalStructuralSnapshot buildLocalStructuralSnapshot(
    const std::string &uri, const std::string &text,
    const std::vector<ChangedRange> &changedRanges, uint64_t documentEpoch,
    int documentVersion, const ImmediateSyntaxDiagnosticsOptions &options) {
  const ImmediateSyntaxDiagnosticsResult result =
      buildImmediateSyntaxDiagnostics(uri, text, changedRanges, options);

  LocalStructuralSnapshot snapshot;
  snapshot.documentEpoch = documentEpoch;
  snapshot.documentVersion = documentVersion;
  snapshot.ready = true;
  snapshot.diagnostics = result.diagnostics;
  snapshot.changedWindowStartLine = result.changedWindowStartLine;
  snapshot.changedWindowEndLine = result.changedWindowEndLine;
  snapshot.changedWindowOnly = result.changedWindowOnly;
  snapshot.ownsDiagnosticsPublish = true;
  snapshot.diagnosticsPublishLayer = "local-structural";
  snapshot.truncated = result.truncated;
  snapshot.elapsedMs = result.elapsedMs;
  return snapshot;
}
