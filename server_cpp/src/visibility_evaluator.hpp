#pragma once

#include <string>
#include <unordered_map>

enum class VisibilityEvalResult { Visible, Hidden, Unknown };

VisibilityEvalResult evaluateVisibilityCondition(
    const std::string &condition,
    const std::unordered_map<std::string, int> &defines);

const char *visibilityEvalResultToString(VisibilityEvalResult value);
