#pragma once

#include "json.hpp"

#include <string>
#include <unordered_map>
#include <vector>

void applySettingsFromJson(
    const Json &settings, std::vector<std::string> &includePaths,
    std::vector<std::string> &shaderExtensions,
    std::unordered_map<std::string, int> &preprocessorDefines,
    bool &inlayHintsEnabled, bool &inlayHintsParameterNamesEnabled,
    bool &semanticTokensEnabled, bool &diagnosticsExpensiveRulesEnabled,
    int &diagnosticsTimeBudgetMs, int &diagnosticsMaxItems,
    bool &diagnosticsFastEnabled, int &diagnosticsFastDelayMs,
    int &diagnosticsFastTimeBudgetMs, int &diagnosticsFastMaxItems,
    bool &diagnosticsFullEnabled, int &diagnosticsFullDelayMs,
    bool &diagnosticsFullExpensiveRulesEnabled,
    int &diagnosticsFullTimeBudgetMs, int &diagnosticsFullMaxItems,
    int &diagnosticsWorkerCount, bool &diagnosticsAutoWorkerCount,
    bool &semanticCacheEnabled,
    bool &diagnosticsIndeterminateEnabled,
    int &diagnosticsIndeterminateSeverity,
    int &diagnosticsIndeterminateMaxItems,
    bool &diagnosticsIndeterminateSuppressWhenErrors,
    int &indexingWorkerCount, int &indexingQueueCapacity);
