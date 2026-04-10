#pragma once

#include "global_context_runtime.hpp"
#include "json.hpp"

#include <string>
#include <unordered_map>
#include <vector>

// Low-level changed-window structural analysis used by
// local_structural_runtime.*.
//
// Responsibilities:
// - scan the current document text for cheap structural diagnostics
// - stay independent from runtime ownership / publish-layer policy
//
// Non-goals:
// - it does not own snapshot state
// - it does not decide which diagnostics layer currently owns publication
struct ImmediateSyntaxDiagnosticsOptions {
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  std::unordered_map<std::string, int> defines;
  std::string activeUnitUri;
  std::string activeUnitText;
  int maxItems = 240;
  int changedWindowPaddingLines = 2;
};

struct ImmediateSyntaxDiagnosticsResult {
  Json diagnostics;
  bool truncated = false;
  int changedWindowStartLine = 0;
  int changedWindowEndLine = 0;
  bool changedWindowOnly = false;
  double elapsedMs = 0.0;
};

ImmediateSyntaxDiagnosticsResult buildImmediateSyntaxDiagnostics(
    const std::string &uri, const std::string &text,
    const std::vector<ChangedRange> &changedRanges,
    const ImmediateSyntaxDiagnosticsOptions &options);
