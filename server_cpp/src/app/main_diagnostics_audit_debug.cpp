#include "main_diagnostics_audit_debug.hpp"

#include "lsp_helpers.hpp"
#include "server_documents.hpp"
#include "uri_utils.hpp"
#include "workspace_summary_runtime.hpp"

#include <cmath>
#include <filesystem>

namespace {

size_t readLimitParam(const Json *params, size_t fallback) {
  if (!params)
    return fallback;
  const Json *value = getObjectValue(*params, "limit");
  if (!value || value->type != Json::Type::Number)
    return fallback;
  const int requested = static_cast<int>(std::llround(getNumberValue(*value)));
  return requested > 0 ? static_cast<size_t>(requested) : fallback;
}

void readUriOrPathParam(const Json *params, const char *uriName,
                        const char *pathName, std::string &uriOut,
                        std::string &pathOut) {
  uriOut.clear();
  pathOut.clear();
  if (!params)
    return;
  const Json *uriValue = getObjectValue(*params, uriName);
  const Json *pathValue = getObjectValue(*params, pathName);
  if (uriValue && uriValue->type == Json::Type::String) {
    uriOut = uriValue->s;
    pathOut = uriToPath(uriOut);
    return;
  }
  if (pathValue && pathValue->type == Json::Type::String) {
    pathOut = pathValue->s;
    uriOut = pathToUri(pathOut);
  }
}

Json buildPrerequisiteSkipsJson(
    const DiagnosticsPrerequisiteSkipStats &stats) {
  Json result = makeObject();
  result.o["total"] = makeNumber(static_cast<double>(stats.total));
  result.o["active_unit_not_ready"] =
      makeNumber(static_cast<double>(stats.activeUnitNotReady));
  result.o["include_closure_not_ready"] =
      makeNumber(static_cast<double>(stats.includeClosureNotReady));
  result.o["preprocessor_context_unreliable"] =
      makeNumber(static_cast<double>(stats.preprocessorContextUnreliable));
  result.o["parser_region_unreliable"] =
      makeNumber(static_cast<double>(stats.parserRegionUnreliable));
  result.o["semantic_snapshot_unavailable"] =
      makeNumber(static_cast<double>(stats.semanticSnapshotUnavailable));
  result.o["local_scope_unreliable"] =
      makeNumber(static_cast<double>(stats.localScopeUnreliable));
  result.o["expression_type_unavailable"] =
      makeNumber(static_cast<double>(stats.expressionTypeUnavailable));
  return result;
}

} // namespace

Json buildDiagnosticsAuditIncludeClosureDebugResponse(const Json *params) {
  Json result = makeObject();
  std::string unitUri;
  std::string unitPath;
  readUriOrPathParam(params, "uri", "path", unitUri, unitPath);
  const size_t limit = readLimitParam(params, 1024);

  std::vector<std::string> paths;
  if (!unitPath.empty())
    workspaceSummaryRuntimeCollectIncludeClosureForUnit(unitPath, paths, limit);

  Json files = makeArray();
  for (const auto &path : paths) {
    Json item = makeObject();
    item.o["path"] = makeString(path);
    item.o["uri"] = makeString(pathToUri(path));
    item.o["extension"] =
        makeString(std::filesystem::path(path).extension().string());
    files.a.push_back(std::move(item));
  }

  result.o["unitUri"] = makeString(unitUri);
  result.o["unitPath"] = makeString(unitPath);
  result.o["ready"] = makeBool(workspaceSummaryRuntimeIsReady());
  result.o["indexingState"] = workspaceSummaryRuntimeGetIndexingState();
  result.o["files"] = std::move(files);
  return result;
}

Json buildDiagnosticsAuditDiagnosticsDebugResponse(
    const Json *params, const DiagnosticsAuditDebugContext &context,
    const std::string &fallbackActiveUnitUri,
    const std::string &fallbackActiveUnitPath) {
  Json result = makeObject();
  std::string uri;
  std::string path;
  std::string activeUnitUri;
  std::string activeUnitPath;
  readUriOrPathParam(params, "uri", "path", uri, path);
  readUriOrPathParam(params, "activeUnitUri", "activeUnitPath",
                     activeUnitUri, activeUnitPath);
  if (activeUnitUri.empty()) {
    activeUnitUri = fallbackActiveUnitUri;
    activeUnitPath = fallbackActiveUnitPath;
    if (activeUnitUri.empty() && !activeUnitPath.empty())
      activeUnitUri = pathToUri(activeUnitPath);
    if (activeUnitPath.empty() && !activeUnitUri.empty())
      activeUnitPath = uriToPath(activeUnitUri);
  }

  DiagnosticsBuildOptions options = context.diagnosticsOptions;
  if (params) {
    const Json *timeBudgetValue = getObjectValue(*params, "timeBudgetMs");
    if (timeBudgetValue && timeBudgetValue->type == Json::Type::Number) {
      const int requested =
          static_cast<int>(std::llround(getNumberValue(*timeBudgetValue)));
      if (requested >= 30)
        options.timeBudgetMs = requested;
    }
    const Json *maxItemsValue = getObjectValue(*params, "maxItems");
    if (maxItemsValue && maxItemsValue->type == Json::Type::Number) {
      const int requested =
          static_cast<int>(std::llround(getNumberValue(*maxItemsValue)));
      if (requested >= 20)
        options.maxItems = requested;
    }
    const Json *expensiveValue = getObjectValue(*params, "expensiveRules");
    if (expensiveValue && expensiveValue->type == Json::Type::Bool)
      options.enableExpensiveRules = expensiveValue->b;
    const Json *semanticCacheValue =
        getObjectValue(*params, "semanticCacheEnabled");
    if (semanticCacheValue && semanticCacheValue->type == Json::Type::Bool)
      options.semanticCacheEnabled = semanticCacheValue->b;
  }

  std::string text;
  const bool loaded = !uri.empty() &&
                      loadDocumentText(uri, context.documents, text);
  options.activeUnitUri = activeUnitUri;
  if (!activeUnitUri.empty()) {
    auto it = context.documents.find(activeUnitUri);
    if (it != context.documents.end()) {
      options.activeUnitText = it->second.text;
    } else if (!activeUnitPath.empty()) {
      readFileText(activeUnitPath, options.activeUnitText);
    }
  }

  result.o["uri"] = makeString(uri);
  result.o["path"] = makeString(path);
  result.o["activeUnitUri"] = makeString(activeUnitUri);
  result.o["activeUnitPath"] = makeString(activeUnitPath);
  result.o["loaded"] = makeBool(loaded);
  if (loaded) {
    DiagnosticsBuildResult buildResult = buildDiagnosticsWithOptions(
        uri, text, context.workspaceFolders, context.includePaths,
        context.shaderExtensions, context.defines, options);
    result.o["diagnostics"] = std::move(buildResult.diagnostics);
    result.o["truncated"] = makeBool(buildResult.truncated);
    result.o["heavyRulesSkipped"] = makeBool(buildResult.heavyRulesSkipped);
    result.o["timedOut"] = makeBool(buildResult.timedOut);
    result.o["indeterminateTotal"] =
        makeNumber(static_cast<double>(buildResult.indeterminateTotal));
    result.o["prerequisiteSkips"] =
        buildPrerequisiteSkipsJson(buildResult.prerequisiteSkips);
    result.o["elapsedMs"] = makeNumber(buildResult.elapsedMs);
  } else {
    result.o["diagnostics"] = makeArray();
  }
  return result;
}
