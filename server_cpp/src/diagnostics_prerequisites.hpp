#pragma once

#include <cstdint>

// Shared prerequisite contract for semantic diagnostics rules.
//
// Responsibilities:
// - describe the analysis context a high-confidence semantic rule needs before
//   it can publish a diagnostic
// - centralize rule-to-prerequisite checks so individual diagnostics rules do
//   not guess whether missing context should still produce user-visible output
// - accumulate skipped-rule metadata for debug/audit reporting
//
// Non-goals:
// - this module does not build diagnostics or recover missing context
// - it does not provide fallback analysis paths when a prerequisite is missing
enum class DiagnosticsRuleKind {
  SemanticSource,
  ExpressionType,
  CallType,
  UndefinedIdentifier,
};

enum class DiagnosticsPrerequisiteKind {
  None,
  ActiveUnitReady,
  IncludeClosureReady,
  PreprocessorContextReliable,
  ParserRegionReliable,
  SemanticSnapshotAvailable,
  LocalScopeReliable,
  ExpressionTypeAvailable,
};

struct DiagnosticsPrerequisiteState {
  bool activeUnitReady = true;
  bool includeClosureReady = true;
  bool preprocessorContextReliable = true;
  bool parserRegionReliable = true;
  bool semanticSnapshotAvailable = true;
  bool localScopeReliable = true;
  bool expressionTypeAvailable = true;
};

struct DiagnosticsPrerequisiteSkipStats {
  uint64_t total = 0;
  uint64_t activeUnitNotReady = 0;
  uint64_t includeClosureNotReady = 0;
  uint64_t preprocessorContextUnreliable = 0;
  uint64_t parserRegionUnreliable = 0;
  uint64_t semanticSnapshotUnavailable = 0;
  uint64_t localScopeUnreliable = 0;
  uint64_t expressionTypeUnavailable = 0;
};

const char *diagnosticsRuleKindName(DiagnosticsRuleKind kind);

const char *
diagnosticsPrerequisiteKindName(DiagnosticsPrerequisiteKind kind);

bool diagnosticsRulePrerequisitesSatisfied(
    DiagnosticsRuleKind rule, const DiagnosticsPrerequisiteState &state,
    DiagnosticsPrerequisiteKind &missingOut);

void recordDiagnosticsPrerequisiteSkip(
    DiagnosticsPrerequisiteSkipStats &stats,
    DiagnosticsPrerequisiteKind missing);
