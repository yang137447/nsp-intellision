#pragma once

#include "immediate_syntax_diagnostics.hpp"
#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Local structural runtime owns the latest cheap, current-document structural
// truth for one analysis key.
//
// Responsibilities:
// - publish changed-window structural diagnostics as an explicit runtime layer
// - track the document version/epoch that this structural snapshot belongs to
// - preserve the changed-window metadata needed by diagnostics continuity
// - carry the diagnostics ownership marker for the local-structural layer
//
// Non-goals:
// - it does not build semantic diagnostics
// - it does not publish to LSP directly
struct LocalStructuralSnapshot {
  uint64_t documentEpoch = 0;
  int documentVersion = 0;
  bool ready = false;
  Json diagnostics;
  int changedWindowStartLine = 0;
  int changedWindowEndLine = 0;
  bool changedWindowOnly = false;
  std::string contextFingerprint;
  bool ownsDiagnosticsPublish = false;
  std::string diagnosticsPublishLayer;
  bool truncated = false;
  double elapsedMs = 0.0;
};

// Builds the latest local-structural snapshot for one document/version.
LocalStructuralSnapshot buildLocalStructuralSnapshot(
    const std::string &uri, const std::string &text,
    const std::vector<ChangedRange> &changedRanges, uint64_t documentEpoch,
    int documentVersion, const ImmediateSyntaxDiagnosticsOptions &options);
