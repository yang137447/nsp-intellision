#include "diagnostics_prerequisites.hpp"

namespace {

bool requireCommonSemanticPrerequisites(
    const DiagnosticsPrerequisiteState &state,
    DiagnosticsPrerequisiteKind &missingOut) {
  if (!state.activeUnitReady) {
    missingOut = DiagnosticsPrerequisiteKind::ActiveUnitReady;
    return false;
  }
  if (!state.includeClosureReady) {
    missingOut = DiagnosticsPrerequisiteKind::IncludeClosureReady;
    return false;
  }
  if (!state.preprocessorContextReliable) {
    missingOut = DiagnosticsPrerequisiteKind::PreprocessorContextReliable;
    return false;
  }
  if (!state.parserRegionReliable) {
    missingOut = DiagnosticsPrerequisiteKind::ParserRegionReliable;
    return false;
  }
  if (!state.semanticSnapshotAvailable) {
    missingOut = DiagnosticsPrerequisiteKind::SemanticSnapshotAvailable;
    return false;
  }
  return true;
}

bool requireLocalScope(const DiagnosticsPrerequisiteState &state,
                       DiagnosticsPrerequisiteKind &missingOut) {
  if (!state.localScopeReliable) {
    missingOut = DiagnosticsPrerequisiteKind::LocalScopeReliable;
    return false;
  }
  return true;
}

bool requireExpressionType(const DiagnosticsPrerequisiteState &state,
                           DiagnosticsPrerequisiteKind &missingOut) {
  if (!state.expressionTypeAvailable) {
    missingOut = DiagnosticsPrerequisiteKind::ExpressionTypeAvailable;
    return false;
  }
  return true;
}

} // namespace

const char *diagnosticsRuleKindName(DiagnosticsRuleKind kind) {
  switch (kind) {
  case DiagnosticsRuleKind::SemanticSource:
    return "semantic-source";
  case DiagnosticsRuleKind::ExpressionType:
    return "expression-type";
  case DiagnosticsRuleKind::CallType:
    return "call-type";
  case DiagnosticsRuleKind::UndefinedIdentifier:
    return "undefined-identifier";
  }
  return "unknown";
}

const char *
diagnosticsPrerequisiteKindName(DiagnosticsPrerequisiteKind kind) {
  switch (kind) {
  case DiagnosticsPrerequisiteKind::None:
    return "none";
  case DiagnosticsPrerequisiteKind::ActiveUnitReady:
    return "active_unit_not_ready";
  case DiagnosticsPrerequisiteKind::IncludeClosureReady:
    return "include_closure_not_ready";
  case DiagnosticsPrerequisiteKind::PreprocessorContextReliable:
    return "preprocessor_context_unreliable";
  case DiagnosticsPrerequisiteKind::ParserRegionReliable:
    return "parser_region_unreliable";
  case DiagnosticsPrerequisiteKind::SemanticSnapshotAvailable:
    return "semantic_snapshot_unavailable";
  case DiagnosticsPrerequisiteKind::LocalScopeReliable:
    return "local_scope_unreliable";
  case DiagnosticsPrerequisiteKind::ExpressionTypeAvailable:
    return "expression_type_unavailable";
  }
  return "unknown";
}

bool diagnosticsRulePrerequisitesSatisfied(
    DiagnosticsRuleKind rule, const DiagnosticsPrerequisiteState &state,
    DiagnosticsPrerequisiteKind &missingOut) {
  missingOut = DiagnosticsPrerequisiteKind::None;
  if (!requireCommonSemanticPrerequisites(state, missingOut))
    return false;

  switch (rule) {
  case DiagnosticsRuleKind::SemanticSource:
    return requireLocalScope(state, missingOut);
  case DiagnosticsRuleKind::ExpressionType:
    return requireLocalScope(state, missingOut) &&
           requireExpressionType(state, missingOut);
  case DiagnosticsRuleKind::CallType:
    return requireLocalScope(state, missingOut) &&
           requireExpressionType(state, missingOut);
  case DiagnosticsRuleKind::UndefinedIdentifier:
    return requireLocalScope(state, missingOut);
  }
  missingOut = DiagnosticsPrerequisiteKind::None;
  return true;
}

void recordDiagnosticsPrerequisiteSkip(
    DiagnosticsPrerequisiteSkipStats &stats,
    DiagnosticsPrerequisiteKind missing) {
  if (missing == DiagnosticsPrerequisiteKind::None)
    return;
  stats.total++;
  switch (missing) {
  case DiagnosticsPrerequisiteKind::None:
    break;
  case DiagnosticsPrerequisiteKind::ActiveUnitReady:
    stats.activeUnitNotReady++;
    break;
  case DiagnosticsPrerequisiteKind::IncludeClosureReady:
    stats.includeClosureNotReady++;
    break;
  case DiagnosticsPrerequisiteKind::PreprocessorContextReliable:
    stats.preprocessorContextUnreliable++;
    break;
  case DiagnosticsPrerequisiteKind::ParserRegionReliable:
    stats.parserRegionUnreliable++;
    break;
  case DiagnosticsPrerequisiteKind::SemanticSnapshotAvailable:
    stats.semanticSnapshotUnavailable++;
    break;
  case DiagnosticsPrerequisiteKind::LocalScopeReliable:
    stats.localScopeUnreliable++;
    break;
  case DiagnosticsPrerequisiteKind::ExpressionTypeAvailable:
    stats.expressionTypeUnavailable++;
    break;
  }
}
