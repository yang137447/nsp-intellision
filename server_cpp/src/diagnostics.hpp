#pragma once

#include "diagnostics_prerequisites.hpp"
#include "json.hpp"
#include "preprocessor_view.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Diagnostics build facade contract.
//
// Responsibilities:
// - assemble syntax, preprocessor, semantic, include, and final publish-ready
//   diagnostics for one document
// - carry active-unit context used to evaluate included files with the same
//   preprocessor environment as their root NSF unit
// - report build metadata, including indeterminate diagnostics and skipped
//   semantic rules whose prerequisites were not met
//
// Non-goals:
// - this facade does not publish diagnostics to LSP clients
// - it does not invent fallback semantic context when prerequisites are missing
struct DiagnosticsBuildOptions {
  bool enableExpensiveRules = true;
  int timeBudgetMs = 1200;
  int maxItems = 1200;
  bool semanticCacheEnabled = true;
  uint64_t documentEpoch = 0;
  std::string activeUnitUri;
  std::string activeUnitText;
  bool indeterminateEnabled = true;
  int indeterminateSeverity = 4;
  int indeterminateMaxItems = 200;
  bool indeterminateSuppressWhenErrors = true;
  bool typeConversionRiskWarningsEnabled = false;
  // Optional debug/audit cache scope for active-unit compiler macro analysis
  // inputs. Normal publish paths leave this empty.
  std::string compilerPrivateConstantCacheScope;
};

struct DiagnosticsBuildResult {
  Json diagnostics;
  bool truncated = false;
  bool heavyRulesSkipped = false;
  bool timedOut = false;
  uint64_t indeterminateTotal = 0;
  uint64_t indeterminateReasonRhsTypeEmpty = 0;
  uint64_t indeterminateReasonBudgetTimeout = 0;
  uint64_t indeterminateReasonHeavyRulesSkipped = 0;
  // Counts high-confidence semantic rule attempts skipped because active-unit,
  // include-closure, preprocessor, parser, scope, snapshot, or expression-type
  // prerequisites were unavailable. Skips are debug/audit metadata, not
  // user-visible diagnostics.
  DiagnosticsPrerequisiteSkipStats prerequisiteSkips;
  // Macro-state-machine health metrics for debug/audit reports. These counts
  // are derived from the same PreprocessorView consumed by diagnostics and do
  // not affect published diagnostics.
  PreprocessorMacroHealthMetrics macroHealth;
  double elapsedMs = 0.0;
};

struct SemanticCacheMetricsSnapshot {
  uint64_t snapshotHit = 0;
  uint64_t snapshotMiss = 0;
};

DiagnosticsBuildResult
buildDiagnosticsWithOptions(const std::string &uri, const std::string &text,
                            const std::vector<std::string> &workspaceFolders,
                            const std::vector<std::string> &includePaths,
                            const std::vector<std::string> &shaderExtensions,
                            const std::unordered_map<std::string, int> &defines,
                            const DiagnosticsBuildOptions &options);

Json buildDiagnostics(const std::string &uri, const std::string &text,
                      const std::vector<std::string> &workspaceFolders,
                      const std::vector<std::string> &includePaths,
                      const std::vector<std::string> &shaderExtensions,
                      const std::unordered_map<std::string, int> &defines);

SemanticCacheMetricsSnapshot takeSemanticCacheMetricsSnapshot();
