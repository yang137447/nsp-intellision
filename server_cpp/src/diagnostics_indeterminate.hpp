#pragma once

#include "diagnostics.hpp"

bool hasDiagnosticErrorOrWarning(const Json &diagnostics);

bool isIndeterminateDiagnostic(const Json &diag);

void fillIndeterminateMetricsFromDiagnostics(const Json &diagnostics,
                                             DiagnosticsBuildResult &result);
