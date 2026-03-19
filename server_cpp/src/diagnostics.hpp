#pragma once

#include "json.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct DiagnosticsBuildOptions {
  bool enableExpensiveRules = true;
  int timeBudgetMs = 1200;
  int maxItems = 1200;
  bool semanticCacheEnabled = true;
  uint64_t documentEpoch = 0;
  bool indeterminateEnabled = true;
  int indeterminateSeverity = 4;
  int indeterminateMaxItems = 200;
  bool indeterminateSuppressWhenErrors = true;
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
