#include "diagnostics_runtime.hpp"

#include "diagnostics_emit.hpp"
#include "global_context_runtime.hpp"
#include "lsp_helpers.hpp"

#include <unordered_set>

namespace {

bool tryGetDiagnosticLineSpan(const Json &diagnostic, int &startLineOut,
                              int &endLineOut) {
  const Json *rangeValue = getObjectValue(diagnostic, "range");
  if (!rangeValue || rangeValue->type != Json::Type::Object)
    return false;
  const Json *startValue = getObjectValue(*rangeValue, "start");
  const Json *endValue = getObjectValue(*rangeValue, "end");
  if (!startValue || startValue->type != Json::Type::Object || !endValue ||
      endValue->type != Json::Type::Object) {
    return false;
  }
  const Json *startLineValue = getObjectValue(*startValue, "line");
  const Json *endLineValue = getObjectValue(*endValue, "line");
  if (!startLineValue || startLineValue->type != Json::Type::Number ||
      !endLineValue || endLineValue->type != Json::Type::Number) {
    return false;
  }
  startLineOut = static_cast<int>(startLineValue->n);
  endLineOut = static_cast<int>(endLineValue->n);
  return true;
}

void appendLastGoodDiagnosticsOutsideChangedWindow(
    Json &target, const Json &source, int changedWindowStartLine,
    int changedWindowEndLine) {
  if (target.type != Json::Type::Array)
    target = makeArray();
  if (source.type != Json::Type::Array)
    return;
  std::unordered_set<std::string> seen;
  seen.reserve(target.a.size() + source.a.size());
  for (const auto &item : target.a)
    seen.insert(serializeJson(item));
  for (const auto &item : source.a) {
    int diagnosticStartLine = 0;
    int diagnosticEndLine = 0;
    if (tryGetDiagnosticLineSpan(item, diagnosticStartLine,
                                 diagnosticEndLine) &&
        diagnosticEndLine >= changedWindowStartLine &&
        diagnosticStartLine <= changedWindowEndLine) {
      continue;
    }
    const std::string key = serializeJson(item);
    if (seen.insert(key).second)
      target.a.push_back(item);
  }
}

bool diagnosticsRuntimeGlobalContextReady(const DocumentRuntime &runtime) {
  const GlobalContextSnapshot *globalContext = runtime.globalContextSnapshot.get();
  if (globalContext)
    return globalContextRuntimeIsReady(*globalContext);
  return runtime.analysisSnapshotKey.activeUnitPath.empty();
}

Json getLastGoodFullDiagnostics(const DocumentRuntime *runtimeBeforeBuild) {
  if (!runtimeBeforeBuild || !runtimeBeforeBuild->deferredDocSnapshot ||
      !runtimeBeforeBuild->deferredDocSnapshot->hasFullDiagnostics) {
    return makeArray();
  }
  return runtimeBeforeBuild->deferredDocSnapshot->fullDiagnostics;
}

} // namespace

std::string
diagnosticsRuntimePublishLayerName(DiagnosticsPublishLayer layer) {
  switch (layer) {
  case DiagnosticsPublishLayer::LocalStructural:
    return "local-structural";
  case DiagnosticsPublishLayer::CurrentDocSemantic:
    return "current-doc-semantic";
  case DiagnosticsPublishLayer::GlobalContext:
    return "global-context";
  case DiagnosticsPublishLayer::None:
  default:
    return "";
  }
}

DiagnosticsPublishDecision diagnosticsRuntimeBuildLocalStructuralPublish(
    const DocumentRuntime *runtimeBeforeBuild,
    const LocalStructuralSnapshot &localStructuralSnapshot) {
  DiagnosticsPublishDecision decision;
  decision.layer = DiagnosticsPublishLayer::LocalStructural;
  decision.diagnostics = localStructuralSnapshot.diagnostics;
  const std::string uri =
      runtimeBeforeBuild ? runtimeBeforeBuild->analysisSnapshotKey.documentUri
                         : "";
  if (!localStructuralSnapshot.changedWindowOnly) {
    dedupeDiagnosticsForUri(uri, decision.diagnostics);
    return decision;
  }
  appendLastGoodDiagnosticsOutsideChangedWindow(
      decision.diagnostics, getLastGoodFullDiagnostics(runtimeBeforeBuild),
      localStructuralSnapshot.changedWindowStartLine,
      localStructuralSnapshot.changedWindowEndLine);
  dedupeDiagnosticsForUri(uri, decision.diagnostics);
  return decision;
}

DiagnosticsPublishDecision diagnosticsRuntimeBuildSemanticPublish(
    const DocumentRuntime &runtimeAfterBuild,
    const DocumentRuntime *runtimeBeforeBuild,
    const Json &builtDiagnostics) {
  DiagnosticsPublishDecision decision;
  const std::string uri = runtimeAfterBuild.analysisSnapshotKey.documentUri;
  if (diagnosticsRuntimeGlobalContextReady(runtimeAfterBuild)) {
    decision.layer = DiagnosticsPublishLayer::GlobalContext;
    decision.diagnostics = builtDiagnostics;
    dedupeDiagnosticsForUri(uri, decision.diagnostics);
    return decision;
  }

  decision.layer = DiagnosticsPublishLayer::CurrentDocSemantic;
  decision.diagnostics = builtDiagnostics;
  const Json lastGoodFull = getLastGoodFullDiagnostics(runtimeBeforeBuild);
  if (lastGoodFull.type == Json::Type::Array && !lastGoodFull.a.empty()) {
    decision.diagnostics = lastGoodFull;
  } else if (decision.diagnostics.type != Json::Type::Array) {
    decision.diagnostics = makeArray();
  }
  dedupeDiagnosticsForUri(uri, decision.diagnostics);
  return decision;
}
