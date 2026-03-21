#include "server_settings.hpp"

#include <string>

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
    int &indexingWorkerCount, int &indexingQueueCapacity) {
  const Json *target = nullptr;
  if (settings.type == Json::Type::Object) {
    target = getObjectValue(settings, "nsf");
    if (!target)
      target = &settings;
  }
  if (!target || target->type != Json::Type::Object)
    return;
  const Json *incPaths = getObjectValue(*target, "intellisionPath");
  if (incPaths && incPaths->type == Json::Type::Array) {
    includePaths.clear();
    for (const auto &item : incPaths->a) {
      includePaths.push_back(getStringValue(item));
    }
  }
  const Json *extensions = getObjectValue(*target, "shaderFileExtensions");
  if (extensions && extensions->type == Json::Type::Array) {
    shaderExtensions.clear();
    for (const auto &item : extensions->a) {
      shaderExtensions.push_back(getStringValue(item));
    }
  }
  const Json *defines = getObjectValue(*target, "defines");
  if (defines && defines->type == Json::Type::Array) {
    preprocessorDefines.clear();
    for (const auto &item : defines->a) {
      std::string value = getStringValue(item);
      if (value.rfind("-D", 0) == 0)
        value = value.substr(2);
      size_t eq = value.find('=');
      std::string name = eq == std::string::npos ? value : value.substr(0, eq);
      std::string rhs = eq == std::string::npos ? "1" : value.substr(eq + 1);
      if (name.empty())
        continue;
      int parsed = 1;
      try {
        parsed = std::stoi(rhs);
      } catch (...) {
        parsed = 1;
      }
      preprocessorDefines[name] = parsed;
    }
  }
  const Json *inlayHints = getObjectValue(*target, "inlayHints");
  if (inlayHints && inlayHints->type == Json::Type::Object) {
    const Json *enabled = getObjectValue(*inlayHints, "enabled");
    if (enabled && enabled->type == Json::Type::Bool)
      inlayHintsEnabled = enabled->b;
    const Json *parameterNames = getObjectValue(*inlayHints, "parameterNames");
    if (parameterNames && parameterNames->type == Json::Type::Bool)
      inlayHintsParameterNamesEnabled = parameterNames->b;
  }
  const Json *semanticTokens = getObjectValue(*target, "semanticTokens");
  if (semanticTokens && semanticTokens->type == Json::Type::Object) {
    const Json *enabled = getObjectValue(*semanticTokens, "enabled");
    if (enabled && enabled->type == Json::Type::Bool)
      semanticTokensEnabled = enabled->b;
  }
  const Json *diagnostics = getObjectValue(*target, "diagnostics");
  if (diagnostics && diagnostics->type == Json::Type::Object) {
    const Json *mode = getObjectValue(*diagnostics, "mode");
    if (mode && mode->type == Json::Type::String) {
      const std::string value = getStringValue(*mode);
      if (value == "basic") {
        diagnosticsExpensiveRulesEnabled = false;
        diagnosticsTimeBudgetMs = 700;
        diagnosticsMaxItems = 600;
        diagnosticsWorkerCount = 1;
        diagnosticsAutoWorkerCount = true;
        diagnosticsFastEnabled = true;
        diagnosticsFastDelayMs = 120;
        diagnosticsFastTimeBudgetMs = 120;
        diagnosticsFastMaxItems = 120;
        diagnosticsFullEnabled = false;
        diagnosticsFullDelayMs = 900;
        diagnosticsFullExpensiveRulesEnabled = false;
        diagnosticsFullTimeBudgetMs = 700;
        diagnosticsFullMaxItems = 600;
        diagnosticsIndeterminateEnabled = true;
        diagnosticsIndeterminateSeverity = 4;
        diagnosticsIndeterminateMaxItems = 100;
        diagnosticsIndeterminateSuppressWhenErrors = true;
      } else if (value == "full") {
        diagnosticsExpensiveRulesEnabled = true;
        diagnosticsTimeBudgetMs = 2000;
        diagnosticsMaxItems = 2000;
        diagnosticsWorkerCount = 2;
        diagnosticsAutoWorkerCount = true;
        diagnosticsFastEnabled = true;
        diagnosticsFastDelayMs = 60;
        diagnosticsFastTimeBudgetMs = 240;
        diagnosticsFastMaxItems = 400;
        diagnosticsFullEnabled = true;
        diagnosticsFullDelayMs = 250;
        diagnosticsFullExpensiveRulesEnabled = true;
        diagnosticsFullTimeBudgetMs = 2000;
        diagnosticsFullMaxItems = 2000;
        diagnosticsIndeterminateEnabled = true;
        diagnosticsIndeterminateSeverity = 4;
        diagnosticsIndeterminateMaxItems = 400;
        diagnosticsIndeterminateSuppressWhenErrors = true;
      } else {
        diagnosticsExpensiveRulesEnabled = true;
        diagnosticsTimeBudgetMs = 1200;
        diagnosticsMaxItems = 1200;
        diagnosticsWorkerCount = 2;
        diagnosticsAutoWorkerCount = true;
        diagnosticsFastEnabled = true;
        diagnosticsFastDelayMs = 90;
        diagnosticsFastTimeBudgetMs = 180;
        diagnosticsFastMaxItems = 240;
        diagnosticsFullEnabled = true;
        diagnosticsFullDelayMs = 700;
        diagnosticsFullExpensiveRulesEnabled = true;
        diagnosticsFullTimeBudgetMs = 1200;
        diagnosticsFullMaxItems = 1200;
        diagnosticsIndeterminateEnabled = true;
        diagnosticsIndeterminateSeverity = 4;
        diagnosticsIndeterminateMaxItems = 200;
        diagnosticsIndeterminateSuppressWhenErrors = true;
      }
    }
    const Json *expensiveRules = getObjectValue(*diagnostics, "expensiveRules");
    if (expensiveRules && expensiveRules->type == Json::Type::Bool) {
      diagnosticsExpensiveRulesEnabled = expensiveRules->b;
      diagnosticsFullExpensiveRulesEnabled = expensiveRules->b;
    }
    const Json *timeBudgetMs = getObjectValue(*diagnostics, "timeBudgetMs");
    if (timeBudgetMs && timeBudgetMs->type == Json::Type::Number) {
      int parsed = static_cast<int>(getNumberValue(*timeBudgetMs));
      parsed = parsed < 30 ? 30 : parsed;
      diagnosticsTimeBudgetMs = parsed;
      diagnosticsFullTimeBudgetMs = parsed;
    }
    const Json *maxItems = getObjectValue(*diagnostics, "maxItems");
    if (maxItems && maxItems->type == Json::Type::Number) {
      int parsed = static_cast<int>(getNumberValue(*maxItems));
      parsed = parsed < 20 ? 20 : parsed;
      diagnosticsMaxItems = parsed;
      diagnosticsFullMaxItems = parsed;
    }
    const Json *workerCount = getObjectValue(*diagnostics, "workerCount");
    if (workerCount && workerCount->type == Json::Type::Number) {
      int parsed = static_cast<int>(getNumberValue(*workerCount));
      diagnosticsWorkerCount = parsed < 1 ? 1 : parsed;
    }
    const Json *autoWorkerCount =
        getObjectValue(*diagnostics, "autoWorkerCount");
    if (autoWorkerCount && autoWorkerCount->type == Json::Type::Bool) {
      diagnosticsAutoWorkerCount = autoWorkerCount->b;
    }
    const Json *fast = getObjectValue(*diagnostics, "fast");
    if (fast && fast->type == Json::Type::Object) {
      const Json *enabled = getObjectValue(*fast, "enabled");
      if (enabled && enabled->type == Json::Type::Bool)
        diagnosticsFastEnabled = enabled->b;
      const Json *delayMs = getObjectValue(*fast, "delayMs");
      if (delayMs && delayMs->type == Json::Type::Number) {
        int parsed = static_cast<int>(getNumberValue(*delayMs));
        diagnosticsFastDelayMs = parsed < 0 ? 0 : parsed;
      }
      const Json *fastBudgetMs = getObjectValue(*fast, "timeBudgetMs");
      if (fastBudgetMs && fastBudgetMs->type == Json::Type::Number) {
        int parsed = static_cast<int>(getNumberValue(*fastBudgetMs));
        diagnosticsFastTimeBudgetMs = parsed < 30 ? 30 : parsed;
      }
      const Json *fastMaxItems = getObjectValue(*fast, "maxItems");
      if (fastMaxItems && fastMaxItems->type == Json::Type::Number) {
        int parsed = static_cast<int>(getNumberValue(*fastMaxItems));
        diagnosticsFastMaxItems = parsed < 20 ? 20 : parsed;
      }
    }
    const Json *full = getObjectValue(*diagnostics, "full");
    if (full && full->type == Json::Type::Object) {
      const Json *enabled = getObjectValue(*full, "enabled");
      if (enabled && enabled->type == Json::Type::Bool)
        diagnosticsFullEnabled = enabled->b;
      const Json *delayMs = getObjectValue(*full, "delayMs");
      if (delayMs && delayMs->type == Json::Type::Number) {
        int parsed = static_cast<int>(getNumberValue(*delayMs));
        diagnosticsFullDelayMs = parsed < 0 ? 0 : parsed;
      }
      const Json *fullExpensiveRules = getObjectValue(*full, "expensiveRules");
      if (fullExpensiveRules && fullExpensiveRules->type == Json::Type::Bool)
        diagnosticsFullExpensiveRulesEnabled = fullExpensiveRules->b;
      const Json *fullBudgetMs = getObjectValue(*full, "timeBudgetMs");
      if (fullBudgetMs && fullBudgetMs->type == Json::Type::Number) {
        int parsed = static_cast<int>(getNumberValue(*fullBudgetMs));
        diagnosticsFullTimeBudgetMs = parsed < 30 ? 30 : parsed;
      }
      const Json *fullMaxItems = getObjectValue(*full, "maxItems");
      if (fullMaxItems && fullMaxItems->type == Json::Type::Number) {
        int parsed = static_cast<int>(getNumberValue(*fullMaxItems));
        diagnosticsFullMaxItems = parsed < 20 ? 20 : parsed;
      }
    }
    const Json *indeterminate = getObjectValue(*diagnostics, "indeterminate");
    if (indeterminate && indeterminate->type == Json::Type::Object) {
      const Json *enabled = getObjectValue(*indeterminate, "enabled");
      if (enabled && enabled->type == Json::Type::Bool)
        diagnosticsIndeterminateEnabled = enabled->b;
      const Json *severity = getObjectValue(*indeterminate, "severity");
      if (severity && severity->type == Json::Type::Number) {
        int parsed = static_cast<int>(getNumberValue(*severity));
        if (parsed < 1)
          parsed = 1;
        if (parsed > 4)
          parsed = 4;
        diagnosticsIndeterminateSeverity = parsed;
      }
      const Json *maxItems = getObjectValue(*indeterminate, "maxItems");
      if (maxItems && maxItems->type == Json::Type::Number) {
        int parsed = static_cast<int>(getNumberValue(*maxItems));
        if (parsed < 0)
          parsed = 0;
        diagnosticsIndeterminateMaxItems = parsed;
      }
      const Json *suppressWhenErrors =
          getObjectValue(*indeterminate, "suppressWhenErrors");
      if (suppressWhenErrors && suppressWhenErrors->type == Json::Type::Bool)
        diagnosticsIndeterminateSuppressWhenErrors = suppressWhenErrors->b;
    }
  }
  const Json *indexing = getObjectValue(*target, "indexing");
  if (indexing && indexing->type == Json::Type::Object) {
    const Json *workerCount = getObjectValue(*indexing, "workerCount");
    if (workerCount && workerCount->type == Json::Type::Number) {
      int parsed = static_cast<int>(getNumberValue(*workerCount));
      if (parsed < 1)
        parsed = 1;
      indexingWorkerCount = parsed;
    }
    const Json *queueCapacity = getObjectValue(*indexing, "queueCapacity");
    if (queueCapacity && queueCapacity->type == Json::Type::Number) {
      int parsed = static_cast<int>(getNumberValue(*queueCapacity));
      if (parsed < 64)
        parsed = 64;
      indexingQueueCapacity = parsed;
    }
  }
}
