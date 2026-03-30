#pragma once

#include "json.hpp"
#include "preprocessor_view.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

// Responsibility: collect the expensive semantic/type diagnostics that augment
// the fast syntax and include checks owned by diagnostics.cpp.
// Inputs: current document text plus the active preprocessor view and workspace
// analysis context used by semantic queries.
// Output: appends diagnostics into `diags` and updates timeout/indeterminate
// counters in place; does not own final result assembly or publication.
void collectReturnAndTypeDiagnostics(
    const std::string &uri, const std::string &text,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const PreprocessorView &preprocessorView, Json &diags, int timeBudgetMs,
    size_t maxDiagnostics, bool &timedOut, bool indeterminateEnabled,
    int indeterminateSeverity, size_t indeterminateMaxItems,
    size_t &indeterminateCount);
