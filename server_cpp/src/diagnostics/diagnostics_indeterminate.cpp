#include "diagnostics_indeterminate.hpp"

#include "indeterminate_reasons.hpp"
#include "json.hpp"

#include <string>

bool hasDiagnosticErrorOrWarning(const Json &diagnostics) {
  for (const auto &diag : diagnostics.a) {
    const Json *severity = getObjectValue(diag, "severity");
    if (!severity || severity->type != Json::Type::Number)
      continue;
    const int value = static_cast<int>(getNumberValue(*severity));
    if (value == 1 || value == 2)
      return true;
  }
  return false;
}

bool isIndeterminateDiagnostic(const Json &diag) {
  const Json *code = getObjectValue(diag, "code");
  if (!code || code->type != Json::Type::String)
    return false;
  const std::string value = getStringValue(*code);
  return value.rfind("NSF_INDET_", 0) == 0;
}

void fillIndeterminateMetricsFromDiagnostics(const Json &diagnostics,
                                             DiagnosticsBuildResult &result) {
  uint64_t total = 0;
  uint64_t rhsTypeEmpty = 0;
  uint64_t budgetTimeout = 0;
  uint64_t heavyRulesSkipped = 0;
  for (const auto &diag : diagnostics.a) {
    if (!isIndeterminateDiagnostic(diag))
      continue;
    total++;
    const Json *data = getObjectValue(diag, "data");
    if (!data || data->type != Json::Type::Object)
      continue;
    const Json *reasonCode = getObjectValue(*data, "reasonCode");
    if (!reasonCode || reasonCode->type != Json::Type::String)
      continue;
    const std::string reason = getStringValue(*reasonCode);
    if (reason == IndeterminateReason::DiagnosticsRhsTypeEmpty)
      rhsTypeEmpty++;
    else if (reason == IndeterminateReason::DiagnosticsBudgetTimeout)
      budgetTimeout++;
    else if (reason == IndeterminateReason::DiagnosticsHeavyRulesSkipped)
      heavyRulesSkipped++;
  }
  result.indeterminateTotal = total;
  result.indeterminateReasonRhsTypeEmpty = rhsTypeEmpty;
  result.indeterminateReasonBudgetTimeout = budgetTimeout;
  result.indeterminateReasonHeavyRulesSkipped = heavyRulesSkipped;
}
