#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "active_unit.hpp"
#include "crash_handler.hpp"
#include "definition_location.hpp"
#include "declaration_query.hpp"
#include "deferred_doc_runtime.hpp"
#include "diagnostics.hpp"
#include "document_owner.hpp"
#include "document_runtime.hpp"
#include "expanded_source.hpp"
#include "fast_ast.hpp"
#include "full_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_docs.hpp"
#include "include_resolver.hpp"
#include "immediate_syntax_diagnostics.hpp"
#include "indeterminate_reasons.hpp"
#include "interactive_semantic_runtime.hpp"
#include "json.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "nsf_lexer.hpp"
#include "preprocessor_view.hpp"
#include "semantic_snapshot.hpp"
#include "semantic_cache.hpp"
#include "server_documents.hpp"
#include "server_occurrences.hpp"
#include "server_parse.hpp"
#include "server_request_handlers.hpp"
#include "server_settings.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"
#include "workspace_summary_runtime.hpp"
#include "main_background_refresh.hpp"
#include "main_did_change_classification.hpp"
#include "main_include_graph_cache.hpp"
#include "main_occurrence_helpers.hpp"


int main(int argc, char **argv) {
  installCrashHandler("nsf_lsp_crash.log");
#ifdef _WIN32
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--debug-wait") {
      waitForDebugger();
    }
  }
#endif
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  getHlslBuiltinNames();
  ServerRequestContext core;
  std::mutex coreMutex;
  core.shaderExtensions = {".nsf", ".hlsl", ".hlsli", ".fx", ".usf",
                           ".ush"};
  core.semanticLegend = createDefaultSemanticTokenLegend();
  std::unordered_map<std::string, int> preprocessorDefines;
  auto applySettings = [&](const Json &settings) {
    std::lock_guard<std::mutex> lock(coreMutex);
    applySettingsFromJson(
        settings, core.includePaths, core.shaderExtensions, preprocessorDefines,
        core.inlayHintsEnabled, core.inlayHintsParameterNamesEnabled,
        core.semanticTokensEnabled, core.diagnosticsExpensiveRulesEnabled,
        core.diagnosticsTimeBudgetMs, core.diagnosticsMaxItems,
        core.diagnosticsFastEnabled, core.diagnosticsFastDelayMs,
        core.diagnosticsFastTimeBudgetMs, core.diagnosticsFastMaxItems,
        core.diagnosticsFullEnabled, core.diagnosticsFullDelayMs,
        core.diagnosticsFullExpensiveRulesEnabled,
        core.diagnosticsFullTimeBudgetMs, core.diagnosticsFullMaxItems,
        core.diagnosticsWorkerCount, core.diagnosticsAutoWorkerCount,
        core.semanticCacheEnabled,
        core.diagnosticsIndeterminateEnabled,
        core.diagnosticsIndeterminateSeverity,
        core.diagnosticsIndeterminateMaxItems,
        core.diagnosticsIndeterminateSuppressWhenErrors,
        core.indexingWorkerCount, core.indexingQueueCapacity);
    core.preprocessorDefines = preprocessorDefines;
    const int maxPrefetchWorkers =
        std::min(std::max(1, core.indexingWorkerCount), 8);
    setDocumentPrefetchMaxConcurrency(maxPrefetchWorkers);
    gSemanticCacheEnabled.store(core.semanticCacheEnabled,
                                std::memory_order_relaxed);
  };
  auto makeDocumentRuntimeOptions = [&]() {
    DocumentRuntimeUpdateOptions options;
    const std::string activeUnitUri = getActiveUnitUri();
    std::lock_guard<std::mutex> lock(coreMutex);
    options.workspaceFolders = core.workspaceFolders;
    options.includePaths = core.includePaths;
    options.shaderExtensions = core.shaderExtensions;
    options.defines = preprocessorDefines;
    options.workspaceSummaryVersion = workspaceSummaryRuntimeGetVersion();
    options.resourceModelHash = getDocumentRuntimeResourceModelHash();
    if (!activeUnitUri.empty()) {
      auto it = core.documents.find(activeUnitUri);
      if (it != core.documents.end()) {
        options.activeUnitText = it->second.text;
        options.activeUnitDocumentVersion = it->second.version;
        options.activeUnitDocumentEpoch = it->second.epoch;
      }
    }
    return options;
  };
  auto makeDeferredDocBuildContext = [&]() {
    DeferredDocBuildContext context;
    std::lock_guard<std::mutex> lock(coreMutex);
    context.documents = core.documents;
    context.workspaceFolders = core.workspaceFolders;
    context.includePaths = core.includePaths;
    context.shaderExtensions = core.shaderExtensions;
    context.defines = preprocessorDefines;
    context.semanticLegend = core.semanticLegend;
    context.prewarmFullDiagnostics = core.diagnosticsFullEnabled;
    context.prewarmInlayHints =
        core.inlayHintsEnabled && core.inlayHintsParameterNamesEnabled;
    context.inlayHintsEnabled = core.inlayHintsEnabled;
    context.inlayHintsParameterNamesEnabled =
        core.inlayHintsParameterNamesEnabled;
    context.diagnosticsOptions.enableExpensiveRules =
        core.diagnosticsFullExpensiveRulesEnabled;
    context.diagnosticsOptions.timeBudgetMs = core.diagnosticsFullTimeBudgetMs;
    context.diagnosticsOptions.maxItems = core.diagnosticsFullMaxItems;
    context.diagnosticsOptions.semanticCacheEnabled = core.semanticCacheEnabled;
    context.diagnosticsOptions.indeterminateEnabled =
        core.diagnosticsIndeterminateEnabled;
    context.diagnosticsOptions.indeterminateSeverity =
        core.diagnosticsIndeterminateSeverity;
    context.diagnosticsOptions.indeterminateMaxItems =
        core.diagnosticsIndeterminateMaxItems;
    context.diagnosticsOptions.indeterminateSuppressWhenErrors =
        core.diagnosticsIndeterminateSuppressWhenErrors;
    context.diagnosticsOptions.activeUnitUri = getActiveUnitUri();
    if (context.diagnosticsOptions.activeUnitUri.empty()) {
      const std::string activeUnitPath = getActiveUnitPath();
      if (!activeUnitPath.empty())
        context.diagnosticsOptions.activeUnitUri = pathToUri(activeUnitPath);
    }
    if (!context.diagnosticsOptions.activeUnitUri.empty()) {
      auto it = core.documents.find(context.diagnosticsOptions.activeUnitUri);
      if (it != core.documents.end())
        context.diagnosticsOptions.activeUnitText = it->second.text;
    }
    return context;
  };
  auto makeRuntimeRequestContext = [&]() {
    std::lock_guard<std::mutex> lock(coreMutex);
    ServerRequestContext ctx = core;
    ctx.preprocessorDefines = preprocessorDefines;
    return ctx;
  };
  setDocumentPrefetchMaxConcurrency(
      std::min(std::max(1, core.indexingWorkerCount), 8));

  static const std::vector<std::string> noLanguageItems;

  struct MethodMetric {
    uint64_t count = 0;
    uint64_t cancelled = 0;
    uint64_t failed = 0;
    double totalMs = 0.0;
    double maxMs = 0.0;
    std::vector<double> samplesMs;
  };
  struct DiagnosticsMetric {
    uint64_t count = 0;
    uint64_t truncated = 0;
    uint64_t timedOut = 0;
    uint64_t heavyRulesSkipped = 0;
    uint64_t indeterminateTotal = 0;
    uint64_t indeterminateReasonRhsTypeEmpty = 0;
    uint64_t indeterminateReasonBudgetTimeout = 0;
    uint64_t indeterminateReasonHeavyRulesSkipped = 0;
    uint64_t semanticCacheSnapshotHit = 0;
    uint64_t semanticCacheSnapshotMiss = 0;
    uint64_t staleDroppedBeforeBuild = 0;
    uint64_t staleDroppedBeforePublish = 0;
    uint64_t canceledInPending = 0;
    uint64_t queueMaxPendingTotal = 0;
    uint64_t queueMaxReadyTotal = 0;
    uint64_t queueWaitSamples = 0;
    double queueWaitTotalMs = 0.0;
    double queueWaitMaxMs = 0.0;
    double totalMs = 0.0;
    double maxMs = 0.0;
  };
  std::mutex metricsMutex;
  std::unordered_map<std::string, MethodMetric> methodMetrics;
  DiagnosticsMetric diagnosticsMetrics;

  uint64_t documentEpochCounter = 0;

  auto recordMethodMetric = [&](const std::string &method, double durationMs,
                                bool canceled, bool failed) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    MethodMetric &metric = methodMetrics[method];
    metric.count++;
    if (canceled)
      metric.cancelled++;
    if (failed)
      metric.failed++;
    metric.totalMs += durationMs;
    metric.maxMs = std::max(metric.maxMs, durationMs);
    metric.samplesMs.push_back(durationMs);
  };
  auto recordDiagnosticsMetric = [&](const DiagnosticsBuildResult &result) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.count++;
    diagnosticsMetrics.totalMs += result.elapsedMs;
    diagnosticsMetrics.maxMs =
        std::max(diagnosticsMetrics.maxMs, result.elapsedMs);
    if (result.truncated)
      diagnosticsMetrics.truncated++;
    if (result.timedOut)
      diagnosticsMetrics.timedOut++;
    if (result.heavyRulesSkipped)
      diagnosticsMetrics.heavyRulesSkipped++;
    diagnosticsMetrics.indeterminateTotal += result.indeterminateTotal;
    diagnosticsMetrics.indeterminateReasonRhsTypeEmpty +=
        result.indeterminateReasonRhsTypeEmpty;
    diagnosticsMetrics.indeterminateReasonBudgetTimeout +=
        result.indeterminateReasonBudgetTimeout;
    diagnosticsMetrics.indeterminateReasonHeavyRulesSkipped +=
        result.indeterminateReasonHeavyRulesSkipped;
  };
  auto recordDiagnosticsStaleDropBeforeBuild = [&]() {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.staleDroppedBeforeBuild++;
  };
  auto recordDiagnosticsStaleDropBeforePublish = [&]() {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.staleDroppedBeforePublish++;
  };
  auto recordDiagnosticsQueueWait = [&](double waitMs) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.queueWaitSamples++;
    diagnosticsMetrics.queueWaitTotalMs += waitMs;
    diagnosticsMetrics.queueWaitMaxMs =
        std::max(diagnosticsMetrics.queueWaitMaxMs, waitMs);
  };
  auto recordDiagnosticsQueueMax = [&](size_t pendingTotal, size_t readyTotal) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.queueMaxPendingTotal =
        std::max(diagnosticsMetrics.queueMaxPendingTotal,
                 static_cast<uint64_t>(pendingTotal));
    diagnosticsMetrics.queueMaxReadyTotal =
        std::max(diagnosticsMetrics.queueMaxReadyTotal,
                 static_cast<uint64_t>(readyTotal));
  };

  auto isDocumentEpochCurrent = [&](const std::string &uri, uint64_t epoch) {
    std::lock_guard<std::mutex> lock(coreMutex);
    auto it = core.documents.find(uri);
    return it != core.documents.end() && it->second.epoch == epoch;
  };

  auto isDiagnosticsJobAnalysisCurrent = [&](const PendingDiagnosticsJob &job) {
    if (job.analysisFingerprint.empty())
      return true;
    DocumentRuntime runtime;
    if (!documentOwnerGetRuntime(job.uri, runtime))
      return false;
    return runtime.analysisSnapshotKey.fullFingerprint ==
           job.analysisFingerprint;
  };

  DiagnosticsBackgroundRuntime *diagnosticsRuntime = nullptr;

  auto publishDiagnosticsNow = [&](const PendingDiagnosticsJob &job) {
    if (!isDocumentEpochCurrent(job.uri, job.documentEpoch)) {
      recordDiagnosticsStaleDropBeforeBuild();
      return;
    }
    if (diagnosticsRuntime && !diagnosticsRuntime->isLatest(job)) {
      recordDiagnosticsStaleDropBeforeBuild();
      return;
    }
    if (!isDiagnosticsJobAnalysisCurrent(job)) {
      recordDiagnosticsStaleDropBeforeBuild();
      return;
    }
    if (job.kind == DiagnosticsJobKind::Full) {
      updateFullAstForDocument(job.uri, job.text, job.documentEpoch);
    }
    Json params = makeObject();
    params.o["uri"] = makeString(job.uri);
    DiagnosticsBuildResult diagnosticsResult;
      if (job.kind == DiagnosticsJobKind::Fast) {
        ImmediateSyntaxDiagnosticsOptions syntaxOptions;
        syntaxOptions.workspaceFolders = job.workspaceFolders;
        syntaxOptions.includePaths = job.includePaths;
        syntaxOptions.shaderExtensions = job.shaderExtensions;
        syntaxOptions.defines = job.defines;
        syntaxOptions.activeUnitUri = job.activeUnitUri;
        syntaxOptions.activeUnitText = job.activeUnitText;
        syntaxOptions.maxItems = job.diagnosticsOptions.maxItems;
        const ImmediateSyntaxDiagnosticsResult immediateResult =
            buildImmediateSyntaxDiagnostics(job.uri, job.text, job.changedRanges,
                                         syntaxOptions);
      diagnosticsResult.diagnostics = immediateResult.diagnostics;
      diagnosticsResult.truncated = immediateResult.truncated;
      diagnosticsResult.elapsedMs = immediateResult.elapsedMs;
      params.o["diagnostics"] = immediateResult.diagnostics;

      ImmediateSyntaxSnapshot syntaxSnapshot;
      syntaxSnapshot.documentEpoch = job.documentEpoch;
      syntaxSnapshot.documentVersion = job.documentVersion;
      syntaxSnapshot.diagnostics = immediateResult.diagnostics;
      syntaxSnapshot.changedWindowOnly = immediateResult.changedWindowOnly;
      syntaxSnapshot.changedWindowStartLine =
          immediateResult.changedWindowStartLine;
      syntaxSnapshot.changedWindowEndLine = immediateResult.changedWindowEndLine;
      documentOwnerUpdateImmediateSyntaxSnapshot(job.uri, syntaxSnapshot);
    } else {
      Document currentDoc;
      bool hasCurrentDoc = false;
      {
        std::lock_guard<std::mutex> lock(coreMutex);
        auto it = core.documents.find(job.uri);
        if (it != core.documents.end()) {
          currentDoc = it->second;
          hasCurrentDoc = true;
        }
      }
      if (hasCurrentDoc && currentDoc.epoch == job.documentEpoch &&
          currentDoc.version == job.documentVersion) {
        diagnosticsResult = buildDeferredFullDiagnostics(job.uri, currentDoc,
                                                         core, job.diagnosticsOptions);
      } else {
        diagnosticsResult = buildDiagnosticsWithOptions(
            job.uri, job.text, job.workspaceFolders, job.includePaths,
            job.shaderExtensions, job.defines, job.diagnosticsOptions);
      }
      params.o["diagnostics"] = diagnosticsResult.diagnostics;

      auto semanticSnapshot = getSemanticSnapshotView(
          job.uri, job.text, job.documentEpoch, job.workspaceFolders,
          job.includePaths, job.shaderExtensions, job.defines);
      if (semanticSnapshot) {
        DocumentRuntime runtime;
        if (documentOwnerGetRuntime(job.uri, runtime)) {
          auto deferred = std::make_shared<DeferredDocSnapshot>();
          deferred->key = runtime.analysisSnapshotKey;
          deferred->documentEpoch = job.documentEpoch;
          deferred->documentVersion = job.documentVersion;
          deferred->semanticSnapshot = std::move(semanticSnapshot);
          deferred->builtAtMs = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count());
          documentOwnerStoreDeferredSnapshot(job.uri, deferred);
        }
      }
    }
    recordDiagnosticsMetric(diagnosticsResult);
    if (!isDocumentEpochCurrent(job.uri, job.documentEpoch)) {
      recordDiagnosticsStaleDropBeforePublish();
      return;
    }
    if (diagnosticsRuntime && !diagnosticsRuntime->isLatest(job)) {
      recordDiagnosticsStaleDropBeforePublish();
      return;
    }
    if (!isDiagnosticsJobAnalysisCurrent(job)) {
      recordDiagnosticsStaleDropBeforePublish();
      return;
    }
    writeNotification("textDocument/publishDiagnostics", params);
  };

  auto scheduleDiagnosticsForText =
      [&](const Document &document, bool scheduleFast = true,
          bool scheduleFull = true, int fastDelayOverrideMs = -1,
          int fullDelayOverrideMs = -1,
          const std::vector<ChangedRange> *changedRangesOverride = nullptr) {
        PendingDiagnosticsJob fastJob;
        bool queueFast = false;
        PendingDiagnosticsJob fullJob;
        bool queueFull = false;
        std::string analysisFingerprint;
        std::string activeUnitUri;
        std::string activeUnitText;
        {
          DocumentRuntime runtime;
          if (documentOwnerGetRuntime(document.uri, runtime))
            analysisFingerprint = runtime.analysisSnapshotKey.fullFingerprint;
        }
        {
          std::lock_guard<std::mutex> lock(coreMutex);
          const auto now = std::chrono::steady_clock::now();
          activeUnitUri = getActiveUnitUri();
          if (activeUnitUri.empty()) {
            const std::string activeUnitPath = getActiveUnitPath();
            if (!activeUnitPath.empty())
              activeUnitUri = pathToUri(activeUnitPath);
          }
          if (!activeUnitUri.empty()) {
            auto itActive = core.documents.find(activeUnitUri);
            if (itActive != core.documents.end())
              activeUnitText = itActive->second.text;
          }
          if (scheduleFast && core.diagnosticsFastEnabled) {
            fastJob.kind = DiagnosticsJobKind::Fast;
            fastJob.hasPairedFull = scheduleFull && core.diagnosticsFullEnabled;
            fastJob.analysisFingerprint.clear();
            fastJob.uri = document.uri;
            fastJob.text = document.text;
            fastJob.documentVersion = document.version;
            fastJob.documentEpoch = document.epoch;
            fastJob.workspaceFolders = core.workspaceFolders;
            fastJob.includePaths = core.includePaths;
            fastJob.shaderExtensions = core.shaderExtensions;
            if (changedRangesOverride)
              fastJob.changedRanges = *changedRangesOverride;
            fastJob.defines = preprocessorDefines;
            fastJob.activeUnitUri = activeUnitUri;
            fastJob.activeUnitText = activeUnitText;
            fastJob.diagnosticsOptions.enableExpensiveRules = true;
            fastJob.diagnosticsOptions.timeBudgetMs =
                core.diagnosticsFastTimeBudgetMs;
            fastJob.diagnosticsOptions.maxItems = core.diagnosticsFastMaxItems;
            fastJob.diagnosticsOptions.semanticCacheEnabled =
                core.semanticCacheEnabled;
            fastJob.diagnosticsOptions.documentEpoch = document.epoch;
            fastJob.diagnosticsOptions.indeterminateEnabled =
                core.diagnosticsIndeterminateEnabled;
            fastJob.diagnosticsOptions.indeterminateSeverity =
                core.diagnosticsIndeterminateSeverity;
            fastJob.diagnosticsOptions.indeterminateMaxItems =
                core.diagnosticsIndeterminateMaxItems;
            fastJob.diagnosticsOptions.indeterminateSuppressWhenErrors =
                core.diagnosticsIndeterminateSuppressWhenErrors;
            const int delayMs = fastDelayOverrideMs >= 0
                                    ? fastDelayOverrideMs
                                    : core.diagnosticsFastDelayMs;
            fastJob.enqueuedAt = now;
            fastJob.due = now + std::chrono::milliseconds(std::max(0, delayMs));
            queueFast = true;
          }
          if (scheduleFull && core.diagnosticsFullEnabled) {
            fullJob.kind = DiagnosticsJobKind::Full;
            fullJob.analysisFingerprint = analysisFingerprint;
            fullJob.uri = document.uri;
            fullJob.text = document.text;
            fullJob.documentVersion = document.version;
            fullJob.documentEpoch = document.epoch;
            fullJob.workspaceFolders = core.workspaceFolders;
            fullJob.includePaths = core.includePaths;
            fullJob.shaderExtensions = core.shaderExtensions;
            fullJob.defines = preprocessorDefines;
            fullJob.activeUnitUri = activeUnitUri;
            fullJob.activeUnitText = activeUnitText;
            fullJob.diagnosticsOptions.enableExpensiveRules =
                core.diagnosticsFullExpensiveRulesEnabled;
            fullJob.diagnosticsOptions.timeBudgetMs =
                core.diagnosticsFullTimeBudgetMs;
            fullJob.diagnosticsOptions.maxItems = core.diagnosticsFullMaxItems;
            fullJob.diagnosticsOptions.semanticCacheEnabled =
                core.semanticCacheEnabled;
            fullJob.diagnosticsOptions.documentEpoch = document.epoch;
            fullJob.diagnosticsOptions.activeUnitUri = activeUnitUri;
            fullJob.diagnosticsOptions.activeUnitText = activeUnitText;
            fullJob.diagnosticsOptions.indeterminateEnabled =
                core.diagnosticsIndeterminateEnabled;
            fullJob.diagnosticsOptions.indeterminateSeverity =
                core.diagnosticsIndeterminateSeverity;
            fullJob.diagnosticsOptions.indeterminateMaxItems =
                core.diagnosticsIndeterminateMaxItems;
            fullJob.diagnosticsOptions.indeterminateSuppressWhenErrors =
                core.diagnosticsIndeterminateSuppressWhenErrors;
            const int delayMs = fullDelayOverrideMs >= 0
                                    ? fullDelayOverrideMs
                                    : core.diagnosticsFullDelayMs;
            fullJob.enqueuedAt = now;
            fullJob.due = now + std::chrono::milliseconds(std::max(0, delayMs));
            queueFull = true;
          }
        }
        if (!queueFast && !queueFull)
          return;
        PendingDiagnosticsJob *fastPtr = queueFast ? &fastJob : nullptr;
        PendingDiagnosticsJob *fullPtr = queueFull ? &fullJob : nullptr;
        if (diagnosticsRuntime)
          diagnosticsRuntime->enqueueJobs(fastPtr, fullPtr);
      };

  auto cancelDiagnosticsAndPublishEmpty = [&](const std::string &uri) {
    if (diagnosticsRuntime)
      diagnosticsRuntime->cancelAndPublishEmpty(uri);
  };

  size_t diagnosticsWorkerCountSnapshot = 2;
  {
    std::lock_guard<std::mutex> lock(coreMutex);
    if (core.diagnosticsAutoWorkerCount) {
      const size_t hardwareWorkers =
          std::max<size_t>(1, std::thread::hardware_concurrency());
      size_t suggested = std::max<size_t>(1, hardwareWorkers / 2);
      if (hardwareWorkers >= 4)
        suggested = std::max<size_t>(2, suggested);
      diagnosticsWorkerCountSnapshot = std::min<size_t>(suggested, 8);
    } else {
      diagnosticsWorkerCountSnapshot =
          core.diagnosticsWorkerCount < 1
              ? static_cast<size_t>(1)
              : static_cast<size_t>(core.diagnosticsWorkerCount);
    }
  }
  auto publishEmptyDiagnostics = [&](const std::string &uri) {
    Json params = makeObject();
    params.o["uri"] = makeString(uri);
    params.o["diagnostics"] = makeArray();
    writeNotification("textDocument/publishDiagnostics", params);
  };
  auto recordDiagnosticsCanceledPending = [&](size_t canceledJobs) {
    std::lock_guard<std::mutex> lock(metricsMutex);
    diagnosticsMetrics.canceledInPending += canceledJobs;
  };
  auto diagnosticsRuntimeOwner =
      std::make_unique<DiagnosticsBackgroundRuntime>(
          diagnosticsWorkerCountSnapshot, DiagnosticsBackgroundCallbacks{
                                              publishDiagnosticsNow,
                                              publishEmptyDiagnostics,
                                              recordDiagnosticsQueueWait,
                                              recordDiagnosticsQueueMax,
                                              recordDiagnosticsCanceledPending,
                                              recordDiagnosticsStaleDropBeforeBuild});
  diagnosticsRuntime = diagnosticsRuntimeOwner.get();

  auto stopDiagnosticsThread = [&]() {
    diagnosticsRuntimeOwner->stop();
  };

  bool metricsThreadStopping = false;
  std::mutex metricsThreadMutex;
  std::condition_variable metricsThreadCv;
  std::thread metricsThread([&]() {
    std::unique_lock<std::mutex> lock(metricsThreadMutex);
    while (!metricsThreadStopping) {
      metricsThreadCv.wait_for(lock, std::chrono::seconds(5),
                               [&]() { return metricsThreadStopping; });
      if (metricsThreadStopping)
        break;
      Json params = makeObject();
      Json methods = makeObject();
      DiagnosticsMetric diagnosticsSnapshot;
      size_t pendingFastSize = 0;
      size_t pendingFullSize = 0;
      size_t readyFastSize = 0;
      size_t readyFullSize = 0;
      FastAstMetricsSnapshot fastAstSnapshot;
      IncludeGraphCacheMetricsSnapshot includeGraphSnapshot;
      FullAstMetricsSnapshot fullAstSnapshot;
      SignatureHelpMetricsSnapshot signatureHelpSnapshot;
      InteractiveRuntimeMetricsSnapshot interactiveRuntimeSnapshot;
      DeferredDocRuntimeMetricsSnapshot deferredDocRuntimeSnapshot;
      auto percentileMs = [](std::vector<double> samples,
                             double percentile) -> double {
        if (samples.empty())
          return 0.0;
        std::sort(samples.begin(), samples.end());
        const double rank =
            std::max(0.0, std::min(1.0, percentile)) *
            static_cast<double>(samples.size() - 1);
        const size_t lower = static_cast<size_t>(std::floor(rank));
        const size_t upper = static_cast<size_t>(std::ceil(rank));
        if (lower >= samples.size())
          return samples.back();
        if (upper >= samples.size())
          return samples[lower];
        if (lower == upper)
          return samples[lower];
        const double frac = rank - static_cast<double>(lower);
        return samples[lower] * (1.0 - frac) + samples[upper] * frac;
      };
      {
        std::lock_guard<std::mutex> metricLock(metricsMutex);
        for (const auto &entry : methodMetrics) {
          Json item = makeObject();
          item.o["count"] = makeNumber(static_cast<double>(entry.second.count));
          item.o["cancelled"] =
              makeNumber(static_cast<double>(entry.second.cancelled));
          item.o["failed"] =
              makeNumber(static_cast<double>(entry.second.failed));
          item.o["avgMs"] = makeNumber(
              entry.second.count > 0 ? (entry.second.totalMs /
                                        static_cast<double>(entry.second.count))
                                     : 0.0);
          item.o["maxMs"] = makeNumber(entry.second.maxMs);
          item.o["p50Ms"] = makeNumber(percentileMs(entry.second.samplesMs, 0.50));
          item.o["p95Ms"] = makeNumber(percentileMs(entry.second.samplesMs, 0.95));
          item.o["p99Ms"] = makeNumber(percentileMs(entry.second.samplesMs, 0.99));
          methods.o[entry.first] = std::move(item);
        }
        diagnosticsSnapshot = diagnosticsMetrics;
        methodMetrics.clear();
        diagnosticsMetrics = DiagnosticsMetric{};
      }
      if (diagnosticsRuntime) {
        const DiagnosticsBackgroundQueueSnapshot queueSnapshot =
            diagnosticsRuntime->getQueueSnapshot();
        pendingFastSize = queueSnapshot.pendingFast;
        pendingFullSize = queueSnapshot.pendingFull;
        readyFastSize = queueSnapshot.readyFast;
        readyFullSize = queueSnapshot.readyFull;
      }
      fastAstSnapshot = takeFastAstMetricsSnapshot();
      includeGraphSnapshot = takeIncludeGraphCacheMetricsSnapshot();
      fullAstSnapshot = takeFullAstMetricsSnapshot();
      signatureHelpSnapshot = takeSignatureHelpMetricsSnapshot();
      interactiveRuntimeSnapshot = takeInteractiveRuntimeMetricsSnapshot();
      deferredDocRuntimeSnapshot = takeDeferredDocRuntimeMetricsSnapshot();
      SemanticCacheMetricsSnapshot semanticCacheSnapshot =
          takeSemanticCacheMetricsSnapshot();
      diagnosticsSnapshot.semanticCacheSnapshotHit +=
          semanticCacheSnapshot.snapshotHit;
      diagnosticsSnapshot.semanticCacheSnapshotMiss +=
          semanticCacheSnapshot.snapshotMiss;
      Json diagnostics = makeObject();
      diagnostics.o["count"] =
          makeNumber(static_cast<double>(diagnosticsSnapshot.count));
      diagnostics.o["truncated"] =
          makeNumber(static_cast<double>(diagnosticsSnapshot.truncated));
      diagnostics.o["timedOut"] =
          makeNumber(static_cast<double>(diagnosticsSnapshot.timedOut));
      diagnostics.o["heavyRulesSkipped"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.heavyRulesSkipped));
      diagnostics.o["indeterminateTotal"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.indeterminateTotal));
      Json diagnosticsIndeterminateReasons = makeObject();
      diagnosticsIndeterminateReasons
          .o[IndeterminateReason::DiagnosticsRhsTypeEmpty] =
          makeNumber(static_cast<double>(
              diagnosticsSnapshot.indeterminateReasonRhsTypeEmpty));
      diagnosticsIndeterminateReasons
          .o[IndeterminateReason::DiagnosticsBudgetTimeout] =
          makeNumber(static_cast<double>(
              diagnosticsSnapshot.indeterminateReasonBudgetTimeout));
      diagnosticsIndeterminateReasons
          .o[IndeterminateReason::DiagnosticsHeavyRulesSkipped] =
          makeNumber(static_cast<double>(
              diagnosticsSnapshot.indeterminateReasonHeavyRulesSkipped));
      diagnostics.o["indeterminateReasons"] = diagnosticsIndeterminateReasons;
      diagnostics.o["semanticCacheSnapshotHit"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.semanticCacheSnapshotHit));
      diagnostics.o["semanticCacheSnapshotMiss"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.semanticCacheSnapshotMiss));
      diagnostics.o["staleDroppedBeforeBuild"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.staleDroppedBeforeBuild));
      diagnostics.o["staleDroppedBeforePublish"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.staleDroppedBeforePublish));
      diagnostics.o["canceledInPending"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.canceledInPending));
      diagnostics.o["queueWaitAvgMs"] = makeNumber(
          diagnosticsSnapshot.queueWaitSamples > 0
              ? (diagnosticsSnapshot.queueWaitTotalMs /
                 static_cast<double>(diagnosticsSnapshot.queueWaitSamples))
              : 0.0);
      diagnostics.o["queueWaitMaxMs"] =
          makeNumber(diagnosticsSnapshot.queueWaitMaxMs);
      diagnostics.o["queueMaxPendingTotal"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.queueMaxPendingTotal));
      diagnostics.o["queueMaxReadyTotal"] = makeNumber(
          static_cast<double>(diagnosticsSnapshot.queueMaxReadyTotal));
      diagnostics.o["avgMs"] =
          makeNumber(diagnosticsSnapshot.count > 0
                         ? (diagnosticsSnapshot.totalMs /
                            static_cast<double>(diagnosticsSnapshot.count))
                         : 0.0);
      diagnostics.o["maxMs"] = makeNumber(diagnosticsSnapshot.maxMs);
      Json diagnosticsQueue = makeObject();
      diagnosticsQueue.o["pendingFast"] =
          makeNumber(static_cast<double>(pendingFastSize));
      diagnosticsQueue.o["pendingFull"] =
          makeNumber(static_cast<double>(pendingFullSize));
      diagnosticsQueue.o["pendingTotal"] =
          makeNumber(static_cast<double>(pendingFastSize + pendingFullSize));
      diagnosticsQueue.o["readyFast"] =
          makeNumber(static_cast<double>(readyFastSize));
      diagnosticsQueue.o["readyFull"] =
          makeNumber(static_cast<double>(readyFullSize));
      diagnosticsQueue.o["readyTotal"] =
          makeNumber(static_cast<double>(readyFastSize + readyFullSize));
      Json fastAst = makeObject();
      fastAst.o["lookups"] =
          makeNumber(static_cast<double>(fastAstSnapshot.lookups));
      fastAst.o["cacheHits"] =
          makeNumber(static_cast<double>(fastAstSnapshot.cacheHits));
      fastAst.o["cacheReused"] =
          makeNumber(static_cast<double>(fastAstSnapshot.cacheReused));
      fastAst.o["rebuilds"] =
          makeNumber(static_cast<double>(fastAstSnapshot.rebuilds));
      fastAst.o["functionsIndexed"] =
          makeNumber(static_cast<double>(fastAstSnapshot.functionsIndexed));
      Json includeGraphCache = makeObject();
      includeGraphCache.o["lookups"] =
          makeNumber(static_cast<double>(includeGraphSnapshot.lookups));
      includeGraphCache.o["cacheHits"] =
          makeNumber(static_cast<double>(includeGraphSnapshot.cacheHits));
      includeGraphCache.o["rebuilds"] =
          makeNumber(static_cast<double>(includeGraphSnapshot.rebuilds));
      includeGraphCache.o["invalidations"] =
          makeNumber(static_cast<double>(includeGraphSnapshot.invalidations));
      Json fullAst = makeObject();
      fullAst.o["lookups"] =
          makeNumber(static_cast<double>(fullAstSnapshot.lookups));
      fullAst.o["cacheHits"] =
          makeNumber(static_cast<double>(fullAstSnapshot.cacheHits));
      fullAst.o["rebuilds"] =
          makeNumber(static_cast<double>(fullAstSnapshot.rebuilds));
      fullAst.o["invalidations"] =
          makeNumber(static_cast<double>(fullAstSnapshot.invalidations));
      fullAst.o["functionsIndexed"] =
          makeNumber(static_cast<double>(fullAstSnapshot.functionsIndexed));
      fullAst.o["includesIndexed"] =
          makeNumber(static_cast<double>(fullAstSnapshot.includesIndexed));
      fullAst.o["documentsCached"] =
          makeNumber(static_cast<double>(fullAstSnapshot.documentsCached));
      Json signatureHelp = makeObject();
      signatureHelp.o["indeterminateTotal"] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.indeterminateTotal));
      Json signatureHelpIndeterminateReasons = makeObject();
      signatureHelpIndeterminateReasons
          .o[IndeterminateReason::SignatureHelpCallTargetUnknown] =
          makeNumber(static_cast<double>(
              signatureHelpSnapshot.indeterminateReasonCallTargetUnknown));
      signatureHelpIndeterminateReasons
          .o[IndeterminateReason::SignatureHelpDefinitionTextUnavailable] =
          makeNumber(static_cast<double>(
              signatureHelpSnapshot
                  .indeterminateReasonDefinitionTextUnavailable));
      signatureHelpIndeterminateReasons
          .o[IndeterminateReason::SignatureHelpSignatureExtractFailed] =
          makeNumber(static_cast<double>(
              signatureHelpSnapshot.indeterminateReasonSignatureExtractFailed));
      signatureHelpIndeterminateReasons
          .o[IndeterminateReason::SignatureHelpOther] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.indeterminateReasonOther));
      signatureHelp.o["indeterminateReasons"] =
          signatureHelpIndeterminateReasons;
      Json overloadResolverMetrics = makeObject();
      overloadResolverMetrics.o["attempts"] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.overloadResolverAttempts));
      overloadResolverMetrics.o["resolved"] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.overloadResolverResolved));
      overloadResolverMetrics.o["ambiguous"] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.overloadResolverAmbiguous));
      overloadResolverMetrics.o["noViable"] = makeNumber(
          static_cast<double>(signatureHelpSnapshot.overloadResolverNoViable));
      overloadResolverMetrics.o["shadowMismatch"] =
          makeNumber(static_cast<double>(
              signatureHelpSnapshot.overloadResolverShadowMismatch));
      signatureHelp.o["overloadResolver"] = overloadResolverMetrics;
      Json interactiveRuntime = makeObject();
      const uint64_t interactiveMergeTotal =
          interactiveRuntimeSnapshot.mergeCurrentDocHits +
          interactiveRuntimeSnapshot.mergeLastGoodHits +
          interactiveRuntimeSnapshot.mergeDeferredDocHits +
          interactiveRuntimeSnapshot.mergeWorkspaceSummaryHits +
          interactiveRuntimeSnapshot.mergeMisses;
      const uint64_t snapshotReuseCount =
          interactiveRuntimeSnapshot.analysisKeyHits +
          interactiveRuntimeSnapshot.lastGoodServed;
      interactiveRuntime.o["snapshotRequests"] =
          makeNumber(static_cast<double>(interactiveRuntimeSnapshot.snapshotRequests));
      interactiveRuntime.o["analysisKeyHits"] =
          makeNumber(static_cast<double>(interactiveRuntimeSnapshot.analysisKeyHits));
      interactiveRuntime.o["snapshotBuildAttempts"] =
          makeNumber(static_cast<double>(interactiveRuntimeSnapshot.snapshotBuildAttempts));
      interactiveRuntime.o["snapshotBuildSuccess"] =
          makeNumber(static_cast<double>(interactiveRuntimeSnapshot.snapshotBuildSuccess));
      interactiveRuntime.o["snapshotBuildFailed"] =
          makeNumber(static_cast<double>(interactiveRuntimeSnapshot.snapshotBuildFailed));
      interactiveRuntime.o["lastGoodServed"] =
          makeNumber(static_cast<double>(interactiveRuntimeSnapshot.lastGoodServed));
      interactiveRuntime.o["incrementalPromoted"] = makeNumber(
          static_cast<double>(interactiveRuntimeSnapshot.incrementalPromoted));
      interactiveRuntime.o["noSnapshotAvailable"] = makeNumber(
          static_cast<double>(interactiveRuntimeSnapshot.noSnapshotAvailable));
      interactiveRuntime.o["mergeCurrentDocHits"] =
          makeNumber(static_cast<double>(interactiveRuntimeSnapshot.mergeCurrentDocHits));
      interactiveRuntime.o["mergeLastGoodHits"] =
          makeNumber(static_cast<double>(interactiveRuntimeSnapshot.mergeLastGoodHits));
      interactiveRuntime.o["mergeDeferredDocHits"] =
          makeNumber(static_cast<double>(interactiveRuntimeSnapshot.mergeDeferredDocHits));
      interactiveRuntime.o["mergeWorkspaceSummaryHits"] = makeNumber(
          static_cast<double>(interactiveRuntimeSnapshot.mergeWorkspaceSummaryHits));
      interactiveRuntime.o["mergeMisses"] =
          makeNumber(static_cast<double>(interactiveRuntimeSnapshot.mergeMisses));
      interactiveRuntime.o["analysisKeyHitRate"] = makeNumber(
          interactiveRuntimeSnapshot.snapshotRequests > 0
              ? static_cast<double>(interactiveRuntimeSnapshot.analysisKeyHits) /
                    static_cast<double>(interactiveRuntimeSnapshot.snapshotRequests)
              : 0.0);
      interactiveRuntime.o["snapshotReuseRate"] = makeNumber(
          interactiveRuntimeSnapshot.snapshotRequests > 0
              ? static_cast<double>(snapshotReuseCount) /
                    static_cast<double>(interactiveRuntimeSnapshot.snapshotRequests)
              : 0.0);
      interactiveRuntime.o["workspaceMergeHitRate"] = makeNumber(
          interactiveMergeTotal > 0
              ? static_cast<double>(interactiveRuntimeSnapshot.mergeWorkspaceSummaryHits) /
                    static_cast<double>(interactiveMergeTotal)
              : 0.0);
      interactiveRuntime.o["snapshotWaitAvgMs"] = makeNumber(
          interactiveRuntimeSnapshot.snapshotWaitSamples > 0
              ? interactiveRuntimeSnapshot.snapshotWaitTotalMs /
                    static_cast<double>(interactiveRuntimeSnapshot.snapshotWaitSamples)
              : 0.0);
      interactiveRuntime.o["snapshotWaitMaxMs"] =
          makeNumber(interactiveRuntimeSnapshot.snapshotWaitMaxMs);

      Json deferredDocRuntime = makeObject();
      deferredDocRuntime.o["scheduled"] =
          makeNumber(static_cast<double>(deferredDocRuntimeSnapshot.scheduled));
      deferredDocRuntime.o["mergedLatestOnly"] = makeNumber(
          static_cast<double>(deferredDocRuntimeSnapshot.mergedLatestOnly));
      deferredDocRuntime.o["droppedStale"] =
          makeNumber(static_cast<double>(deferredDocRuntimeSnapshot.droppedStale));
      deferredDocRuntime.o["buildCount"] =
          makeNumber(static_cast<double>(deferredDocRuntimeSnapshot.buildCount));
      deferredDocRuntime.o["latestOnlyMergeRate"] = makeNumber(
          deferredDocRuntimeSnapshot.scheduled > 0
              ? static_cast<double>(deferredDocRuntimeSnapshot.mergedLatestOnly) /
                    static_cast<double>(deferredDocRuntimeSnapshot.scheduled)
              : 0.0);
      deferredDocRuntime.o["queueWaitAvgMs"] = makeNumber(
          deferredDocRuntimeSnapshot.queueWaitSamples > 0
              ? deferredDocRuntimeSnapshot.queueWaitTotalMs /
                    static_cast<double>(deferredDocRuntimeSnapshot.queueWaitSamples)
              : 0.0);
      deferredDocRuntime.o["queueWaitMaxMs"] =
          makeNumber(deferredDocRuntimeSnapshot.queueWaitMaxMs);
      deferredDocRuntime.o["buildAvgMs"] = makeNumber(
          deferredDocRuntimeSnapshot.buildCount > 0
              ? deferredDocRuntimeSnapshot.buildTotalMs /
                    static_cast<double>(deferredDocRuntimeSnapshot.buildCount)
              : 0.0);
      deferredDocRuntime.o["buildMaxMs"] =
          makeNumber(deferredDocRuntimeSnapshot.buildMaxMs);
      params.o["methods"] = methods;
      params.o["diagnostics"] = diagnostics;
      params.o["diagnosticsQueue"] = diagnosticsQueue;
      params.o["fastAst"] = fastAst;
      params.o["includeGraphCache"] = includeGraphCache;
      params.o["fullAst"] = fullAst;
      params.o["signatureHelp"] = signatureHelp;
      params.o["interactiveRuntime"] = interactiveRuntime;
      params.o["deferredDocRuntime"] = deferredDocRuntime;
      writeNotification("nsf/metrics", params);
    }
  });

  auto stopMetricsThread = [&]() {
    {
      std::lock_guard<std::mutex> lock(metricsThreadMutex);
      metricsThreadStopping = true;
    }
    metricsThreadCv.notify_one();
    if (metricsThread.joinable())
      metricsThread.join();
  };

  enum class RequestPriority : int { P0 = 0, P1 = 1, P2 = 2, P3 = 3 };

  struct QueuedRequest {
    std::string method;
    Json id;
    Json params;
    bool hasId = false;
    RequestPriority priority = RequestPriority::P2;
    std::string documentUri;
    int documentVersion = 0;
    std::string latestOnlyKey;
    uint64_t latestOnlyEpoch = 0;
    std::string inlayUri;
    uint64_t inlayEpoch = 0;
  };

  std::mutex queueMutex;
  std::condition_variable queueCv;
  bool requestWorkersStopping = false;
  std::deque<QueuedRequest> requestQueues[4];
  std::unordered_set<std::string> canceledStringIds;
  std::unordered_set<int64_t> canceledNumericIds;
  std::unordered_map<std::string, uint64_t> inlayLatestEpochByUri;
  std::unordered_map<std::string, uint64_t> latestOnlyEpochByKey;
  std::unordered_map<std::string, int> latestDocumentVersionByUri;

  auto markRequestCanceled = [&](const Json &requestId) {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (requestId.type == Json::Type::String && !requestId.s.empty()) {
      canceledStringIds.insert(requestId.s);
      return;
    }
    if (requestId.type == Json::Type::Number) {
      canceledNumericIds.insert(
          static_cast<int64_t>(std::llround(getNumberValue(requestId))));
    }
  };

  auto clearRequestCancelState = [&](const Json &requestId) {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (requestId.type == Json::Type::String && !requestId.s.empty()) {
      canceledStringIds.erase(requestId.s);
      return;
    }
    if (requestId.type == Json::Type::Number) {
      canceledNumericIds.erase(
          static_cast<int64_t>(std::llround(getNumberValue(requestId))));
    }
  };

  auto isRequestCanceled = [&](const QueuedRequest &request) -> bool {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (request.hasId) {
      if (request.id.type == Json::Type::String && !request.id.s.empty() &&
          canceledStringIds.find(request.id.s) != canceledStringIds.end()) {
        return true;
      }
      if (request.id.type == Json::Type::Number &&
          canceledNumericIds.find(static_cast<int64_t>(std::llround(
              getNumberValue(request.id)))) != canceledNumericIds.end()) {
        return true;
      }
    }
    if (!request.inlayUri.empty() && request.inlayEpoch > 0) {
      auto it = inlayLatestEpochByUri.find(request.inlayUri);
      if (it != inlayLatestEpochByUri.end() && it->second != request.inlayEpoch)
        return true;
    }
    if (!request.latestOnlyKey.empty() && request.latestOnlyEpoch > 0) {
      auto it = latestOnlyEpochByKey.find(request.latestOnlyKey);
      if (it != latestOnlyEpochByKey.end() &&
          it->second != request.latestOnlyEpoch) {
        return true;
      }
    }
    if ((!request.latestOnlyKey.empty() || !request.inlayUri.empty()) &&
        !request.documentUri.empty() && request.documentVersion > 0 &&
        (request.priority == RequestPriority::P2 ||
         request.priority == RequestPriority::P3)) {
      auto it = latestDocumentVersionByUri.find(request.documentUri);
      if (it != latestDocumentVersionByUri.end() &&
          it->second > request.documentVersion) {
        return true;
      }
    }
    return false;
  };

  auto priorityForMethod = [&](const std::string &method) {
    if (method == "textDocument/hover" || method == "textDocument/definition" ||
        method == "textDocument/signatureHelp" ||
        method == "textDocument/completion") {
      return RequestPriority::P1;
    }
    if (method == "textDocument/documentSymbol") {
      return RequestPriority::P2;
    }
    if (method == "textDocument/inlayHint" ||
        method == "textDocument/semanticTokens/full" ||
        method == "textDocument/semanticTokens/range" ||
        method == "textDocument/references" ||
        method == "textDocument/prepareRename" ||
        method == "textDocument/rename" ||
        method == "workspace/symbol") {
      return RequestPriority::P3;
    }
    return RequestPriority::P2;
  };

  auto enqueueRequest = [&](QueuedRequest request) {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (request.method == "textDocument/inlayHint" &&
        !request.inlayUri.empty()) {
      request.inlayEpoch = ++inlayLatestEpochByUri[request.inlayUri];
      auto &q = requestQueues[static_cast<int>(RequestPriority::P3)];
      for (auto it = q.begin(); it != q.end();) {
        if (it->method == "textDocument/inlayHint" &&
            it->inlayUri == request.inlayUri) {
          if (it->hasId)
            writeError(it->id, -32800, "Request cancelled");
          if (it->hasId) {
            if (it->id.type == Json::Type::String && !it->id.s.empty()) {
              canceledStringIds.erase(it->id.s);
            } else if (it->id.type == Json::Type::Number) {
              canceledNumericIds.erase(
                  static_cast<int64_t>(std::llround(getNumberValue(it->id))));
            }
          }
          it = q.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (!request.latestOnlyKey.empty()) {
      request.latestOnlyEpoch = ++latestOnlyEpochByKey[request.latestOnlyKey];
      for (auto &queue : requestQueues) {
        for (auto it = queue.begin(); it != queue.end();) {
          if (it->latestOnlyKey == request.latestOnlyKey) {
            if (it->hasId)
              writeError(it->id, -32800, "Request cancelled");
            it = queue.erase(it);
          } else {
            ++it;
          }
        }
      }
    }
    requestQueues[static_cast<int>(request.priority)].push_back(
        std::move(request));
    queueCv.notify_all();
  };

  auto takeNextRequestForLane = [&](bool interactiveLane) -> QueuedRequest {
    std::unique_lock<std::mutex> lock(queueMutex);
    queueCv.wait(lock, [&]() {
      if (requestWorkersStopping)
        return true;
      if (interactiveLane) {
        return !requestQueues[static_cast<int>(RequestPriority::P0)].empty() ||
               !requestQueues[static_cast<int>(RequestPriority::P1)].empty();
      }
      return !requestQueues[static_cast<int>(RequestPriority::P2)].empty() ||
             !requestQueues[static_cast<int>(RequestPriority::P3)].empty();
    });
    if (requestWorkersStopping) {
      const bool laneEmpty =
          interactiveLane
              ? (requestQueues[static_cast<int>(RequestPriority::P0)].empty() &&
                 requestQueues[static_cast<int>(RequestPriority::P1)].empty())
              : (requestQueues[static_cast<int>(RequestPriority::P2)].empty() &&
                 requestQueues[static_cast<int>(RequestPriority::P3)].empty());
      if (laneEmpty)
        return QueuedRequest{};
    }
    const int begin = interactiveLane ? 0 : 2;
    const int end = interactiveLane ? 2 : 4;
    for (int i = begin; i < end; i++) {
      if (!requestQueues[i].empty()) {
        QueuedRequest next = std::move(requestQueues[i].front());
        requestQueues[i].pop_front();
        return next;
      }
    }
    return QueuedRequest{};
  };

  auto runRequestWorker = [&](bool interactiveLane) {
    while (true) {
      QueuedRequest request = takeNextRequestForLane(interactiveLane);
      if (request.method.empty()) {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (requestWorkersStopping)
          break;
        continue;
      }

      const auto requestStart = std::chrono::steady_clock::now();
      if (isRequestCanceled(request)) {
        if (request.hasId)
          writeError(request.id, -32800, "Request cancelled");
        if (request.hasId)
          clearRequestCancelState(request.id);
        recordMethodMetric(
            request.method,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - requestStart)
                .count(),
            true, false);
        continue;
      }

      ServerRequestContext requestCtx = makeRuntimeRequestContext();
      requestCtx.isCancellationRequested = [&, request]() {
        return isRequestCanceled(request);
      };
      const Json *paramsPtr =
          request.params.type == Json::Type::Null ? nullptr : &request.params;
      bool handled =
          handleCoreRequestMethods(request.method, request.id, paramsPtr,
                                   requestCtx, noLanguageItems,
                                   noLanguageItems);
      bool failed = false;
      if (!handled && request.hasId) {
        writeError(request.id, -32601, "Method not implemented");
        failed = true;
      }
      const bool canceled = isRequestCanceled(request);
      recordMethodMetric(request.method,
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - requestStart)
                             .count(),
                         canceled, failed);
      if (request.hasId)
        clearRequestCancelState(request.id);
    }
  };

  std::thread requestWorkerInteractive([&]() { runRequestWorker(true); });
  std::thread requestWorkerBackground([&]() { runRequestWorker(false); });

  auto stopRequestWorkers = [&]() {
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      requestWorkersStopping = true;
    }
    queueCv.notify_all();
    if (requestWorkerInteractive.joinable())
      requestWorkerInteractive.join();
    if (requestWorkerBackground.joinable())
      requestWorkerBackground.join();
  };

  while (true) {
    std::string payload;
    if (!readMessage(payload)) {
      break;
    }
    Json message;
    if (!parseJson(payload, message))
      continue;
    const Json *methodValue = getObjectValue(message, "method");
    std::string method = methodValue ? getStringValue(*methodValue) : "";
    const Json *idValue = getObjectValue(message, "id");
    Json id = idValue ? *idValue : makeNull();
    const Json *params = getObjectValue(message, "params");
    if (method.empty())
      continue;

    if (method == "initialize") {
      if (params) {
        const Json *initOptions =
            getObjectValue(*params, "initializationOptions");
        if (initOptions)
          applySettings(*initOptions);
        const Json *folders = getObjectValue(*params, "workspaceFolders");
        if (folders && folders->type == Json::Type::Array) {
          std::lock_guard<std::mutex> lock(coreMutex);
          core.workspaceFolders.clear();
          for (const auto &item : folders->a) {
            const Json *uriValue = getObjectValue(item, "uri");
            if (uriValue && uriValue->type == Json::Type::String)
              core.workspaceFolders.push_back(uriToPath(uriValue->s));
          }
        }
      }
      std::vector<std::string> workspaceFoldersSnapshot;
      std::vector<std::string> includePathsSnapshot;
      std::vector<std::string> shaderExtensionsSnapshot;
      SemanticTokenLegend semanticLegendSnapshot;
      {
        std::lock_guard<std::mutex> lock(coreMutex);
        workspaceFoldersSnapshot = core.workspaceFolders;
        includePathsSnapshot = core.includePaths;
        shaderExtensionsSnapshot = core.shaderExtensions;
        semanticLegendSnapshot = core.semanticLegend;
        workspaceSummaryRuntimeSetConcurrencyLimits(
            core.indexingWorkerCount <= 0
                ? static_cast<size_t>(1)
                : static_cast<size_t>(core.indexingWorkerCount),
            core.indexingQueueCapacity <= 0
                ? static_cast<size_t>(4096)
                : static_cast<size_t>(core.indexingQueueCapacity));
      }
      workspaceSummaryRuntimeConfigure(workspaceFoldersSnapshot,
                                       includePathsSnapshot,
                                       shaderExtensionsSnapshot);
      Json completionProvider = makeObject();
      Json triggerChars = makeArray();
      for (const auto &ch :
           std::vector<std::string>{"#", "\"", "<", ".", "["}) {
        triggerChars.a.push_back(makeString(ch));
      }
      completionProvider.o["triggerCharacters"] = triggerChars;
      Json renameProvider = makeObject();
      renameProvider.o["prepareProvider"] = makeBool(true);
      Json semanticTokensProvider = makeObject();
      Json semanticLegendJson = makeObject();
      Json semanticTokenTypes = makeArray();
      for (const auto &t : semanticLegendSnapshot.tokenTypes) {
        semanticTokenTypes.a.push_back(makeString(t));
      }
      Json semanticTokenModifiers = makeArray();
      for (const auto &m : semanticLegendSnapshot.tokenModifiers) {
        semanticTokenModifiers.a.push_back(makeString(m));
      }
      semanticLegendJson.o["tokenTypes"] = semanticTokenTypes;
      semanticLegendJson.o["tokenModifiers"] = semanticTokenModifiers;
      semanticTokensProvider.o["legend"] = semanticLegendJson;
      semanticTokensProvider.o["full"] = makeBool(true);
      semanticTokensProvider.o["range"] = makeBool(true);
      Json signatureHelpProvider = makeObject();
      Json signatureTriggers = makeArray();
      for (const auto &ch : std::vector<std::string>{"(", ","}) {
        signatureTriggers.a.push_back(makeString(ch));
      }
      signatureHelpProvider.o["triggerCharacters"] = signatureTriggers;
      Json inlayHintProvider = makeBool(true);
      Json caps = makeObject();
      caps.o["textDocumentSync"] = makeNumber(2);
      caps.o["hoverProvider"] = makeBool(true);
      caps.o["definitionProvider"] = makeBool(true);
      caps.o["referencesProvider"] = makeBool(true);
      caps.o["documentSymbolProvider"] = makeBool(true);
      caps.o["workspaceSymbolProvider"] = makeBool(true);
      caps.o["renameProvider"] = renameProvider;
      caps.o["completionProvider"] = completionProvider;
      caps.o["semanticTokensProvider"] = semanticTokensProvider;
      caps.o["signatureHelpProvider"] = signatureHelpProvider;
      caps.o["inlayHintProvider"] = inlayHintProvider;
      Json result = makeObject();
      result.o["capabilities"] = caps;
      writeResponse(id, result);
      continue;
    }

    if (method == "workspace/didChangeConfiguration" && params) {
      const Json *settings = getObjectValue(*params, "settings");
      if (settings)
        applySettings(*settings);
      std::vector<std::string> workspaceFoldersSnapshot;
      std::vector<std::string> includePathsSnapshot;
      std::vector<std::string> shaderExtensionsSnapshot;
      std::vector<Document> documentSnapshot;
      {
        std::lock_guard<std::mutex> lock(coreMutex);
        workspaceFoldersSnapshot = core.workspaceFolders;
        includePathsSnapshot = core.includePaths;
        shaderExtensionsSnapshot = core.shaderExtensions;
        workspaceSummaryRuntimeSetConcurrencyLimits(
            core.indexingWorkerCount <= 0
                ? static_cast<size_t>(1)
                : static_cast<size_t>(core.indexingWorkerCount),
            core.indexingQueueCapacity <= 0
                ? static_cast<size_t>(4096)
                : static_cast<size_t>(core.indexingQueueCapacity));
        for (const auto &entry : core.documents)
          documentSnapshot.push_back(entry.second);
      }
      invalidateAllIncludeGraphCaches();
      workspaceSummaryRuntimeConfigure(workspaceFoldersSnapshot,
                                       includePathsSnapshot,
                                       shaderExtensionsSnapshot);
      documentOwnerRefreshAnalysisContext(makeDocumentRuntimeOptions(),
                                          makeRuntimeRequestContext());
      namespace fs = std::filesystem;
      auto normalize = [](const std::string &value) -> std::string {
        std::string out;
        out.reserve(value.size());
        for (unsigned char ch : value) {
          char c = static_cast<char>(std::tolower(ch));
          if (c == '\\')
            c = '/';
          out.push_back(c);
        }
        while (out.size() > 1 && out.back() == '/')
          out.pop_back();
        return out;
      };
      auto isUnderOrEqual = [&](const std::string &dir,
                                const std::string &path) -> bool {
        if (dir.empty())
          return false;
        if (path.size() < dir.size())
          return false;
        if (path.rfind(dir, 0) != 0)
          return false;
        if (path.size() == dir.size())
          return true;
        return path[dir.size()] == '/';
      };

      const std::string unitPath = getActiveUnitPath();
      const std::string unitDir =
          unitPath.empty() ? std::string()
                           : fs::path(unitPath).parent_path().string();
      const std::string unitDirN = normalize(unitDir);
      for (const auto &entry : documentSnapshot) {
        std::string docPath = uriToPath(entry.uri);
        if (docPath.empty())
          docPath = entry.uri;
        const std::string docPathN = normalize(docPath);
        const bool unitRelated =
            !unitDirN.empty() && isUnderOrEqual(unitDirN, docPathN);
        const int delay = unitPath.empty() ? 250 : (unitRelated ? 120 : 650);
        scheduleDiagnosticsForText(entry, true, true, std::min(delay, 120),
                                   delay);
      }
      continue;
    }

    if (method == "nsf/getIndexingState") {
      writeResponse(id, workspaceSummaryRuntimeGetIndexingState());
      continue;
    }

    if (method == "nsf/kickIndexing") {
      std::string reason = "manual";
      if (params && params->type == Json::Type::Object) {
        const Json *reasonValue = getObjectValue(*params, "reason");
        if (reasonValue && reasonValue->type == Json::Type::String &&
            !reasonValue->s.empty()) {
          reason = reasonValue->s;
        }
      }
      workspaceSummaryRuntimeKickIndexing(reason);
      if (id.type != Json::Type::Null)
        writeResponse(id, makeNull());
      continue;
    }

    if (method == "nsf/rebuildIndex") {
      std::string reason = "manual";
      bool clearDiskCache = false;
      if (params && params->type == Json::Type::Object) {
        const Json *reasonValue = getObjectValue(*params, "reason");
        if (reasonValue && reasonValue->type == Json::Type::String &&
            !reasonValue->s.empty()) {
          reason = reasonValue->s;
        }
        const Json *clearValue = getObjectValue(*params, "clearDiskCache");
        if (clearValue)
          clearDiskCache = getBoolValue(*clearValue, false);
      }
      invalidateAllIncludeGraphCaches();
      invalidateAllFastAstCaches();
      invalidateAllFullAstCaches();
      semanticCacheInvalidateAll();
      workspaceSummaryRuntimeRebuild(reason, clearDiskCache);
      documentRuntimeBumpWorkspaceSummaryVersion();
      documentOwnerRefreshAnalysisContext(makeDocumentRuntimeOptions(),
                                          makeRuntimeRequestContext());
      if (id.type != Json::Type::Null)
        writeResponse(id, makeNull());
      continue;
    }

    if (method == "workspace/didChangeWatchedFiles" && params) {
      {
        namespace fs = std::filesystem;
        std::vector<std::string> changedUris;
        std::unordered_set<std::string> changedDocUris;
        std::unordered_set<std::string> changedPaths;
        std::unordered_set<std::string> includeImpactedPaths;
        std::vector<std::string> refreshUris;
        std::vector<Document> documentSnapshot;
        auto normalize = [](const std::string &value) -> std::string {
          std::string out;
          out.reserve(value.size());
          for (unsigned char ch : value) {
            char c = static_cast<char>(std::tolower(ch));
            if (c == '\\')
              c = '/';
            out.push_back(c);
          }
          while (out.size() > 1 && out.back() == '/')
            out.pop_back();
          return out;
        };
        auto isUnderOrEqual = [&](const std::string &dir,
                                  const std::string &path) -> bool {
          if (dir.empty())
            return false;
          if (path.size() < dir.size())
            return false;
          if (path.rfind(dir, 0) != 0)
            return false;
          if (path.size() == dir.size())
            return true;
          return path[dir.size()] == '/';
        };
        const Json *changes = getObjectValue(*params, "changes");
        if (changes && changes->type == Json::Type::Array) {
          for (const auto &entry : changes->a) {
            if (entry.type != Json::Type::Object)
              continue;
            const Json *uriValue = getObjectValue(entry, "uri");
            if (uriValue && uriValue->type == Json::Type::String) {
              changedUris.push_back(uriValue->s);
              changedDocUris.insert(uriValue->s);
              std::string p = uriToPath(uriValue->s);
              if (p.empty())
                p = uriValue->s;
              changedPaths.insert(normalize(p));
            }
          }
        }
        if (!changedUris.empty())
          invalidateDocumentTextCacheByUris(changedUris);
        if (!changedUris.empty())
          invalidateFastAstByUris(changedUris);
        if (!changedUris.empty())
          invalidateFullAstByUris(changedUris);
        if (!changedUris.empty())
          invalidateIncludeGraphCacheByUris(changedUris);
        if (!changedUris.empty()) {
          workspaceSummaryRuntimeHandleFileChanges(changedUris);
        }
        if (!changedUris.empty()) {
          std::vector<std::string> impacted;
          workspaceSummaryRuntimeCollectReverseIncludeClosure(changedUris,
                                                              impacted, 4096);
          for (const auto &p : impacted) {
            if (!p.empty())
              includeImpactedPaths.insert(p);
          }
        }

        const std::string unitPath = getActiveUnitPath();
        const std::string unitDir =
            unitPath.empty() ? std::string()
                             : fs::path(unitPath).parent_path().string();
        const std::string unitDirN = normalize(unitDir);
        {
          std::lock_guard<std::mutex> lock(coreMutex);
          for (const auto &entry : core.documents)
            documentSnapshot.push_back(entry.second);
        }

        const int delay = 180;
        const int followupDelay = 1200;
        const bool fallbackRefreshAll = !changedUris.empty();
        for (const auto &doc : documentSnapshot) {
          std::string docPath = uriToPath(doc.uri);
          if (docPath.empty())
            docPath = doc.uri;
          const std::string docPathN = normalize(docPath);
          const bool underUnit =
              !unitDirN.empty() && isUnderOrEqual(unitDirN, docPathN);
          const bool directlyChanged =
              changedDocUris.find(doc.uri) != changedDocUris.end() ||
              changedPaths.find(docPathN) != changedPaths.end();
          const bool includeImpacted =
              includeImpactedPaths.find(docPathN) != includeImpactedPaths.end();
          if (directlyChanged || includeImpacted) {
            refreshUris.push_back(doc.uri);
          }
          if (underUnit || directlyChanged || includeImpacted ||
              fallbackRefreshAll) {
            scheduleDiagnosticsForText(doc, true, false, delay);
            if (!changedUris.empty()) {
              scheduleDiagnosticsForText(doc, false, true, -1, followupDelay);
            }
          }
        }
        if (!refreshUris.empty()) {
          documentRuntimeBumpWorkspaceSummaryVersion();
          documentOwnerRefreshAnalysisContextForUris(
              refreshUris, makeDocumentRuntimeOptions(),
              makeRuntimeRequestContext());
          std::unordered_set<std::string> refreshUriSet(refreshUris.begin(),
                                                        refreshUris.end());
          for (const auto &doc : documentSnapshot) {
            if (refreshUriSet.find(doc.uri) == refreshUriSet.end())
              continue;
            scheduleDiagnosticsForText(doc, true, true, 80, 220);
          }
        }
      }
      continue;
    }

    if (method == "nsf/setActiveUnit" && params) {
      const Json *uriValue = getObjectValue(*params, "uri");
      const Json *pathValue = getObjectValue(*params, "path");
      std::string uri;
      std::string unitPath;
      if (uriValue && uriValue->type == Json::Type::String) {
        uri = uriValue->s;
        unitPath = uriToPath(uri);
      } else if (pathValue && pathValue->type == Json::Type::String) {
        unitPath = pathValue->s;
        uri = pathToUri(unitPath);
      }
      const std::string previousUri = getActiveUnitUri();
      const std::string previousPath = getActiveUnitPath();
      setActiveUnit(uri, unitPath);
      if (previousUri != uri || previousPath != unitPath) {
        workspaceSummaryRuntimeKickIndexing("activeUnitChange");
      }
      documentOwnerRefreshAnalysisContext(makeDocumentRuntimeOptions(),
                                          makeRuntimeRequestContext());
      if (!uri.empty()) {
        Document activeDoc;
        bool hasActiveDoc = false;
        {
          std::lock_guard<std::mutex> lock(coreMutex);
          auto it = core.documents.find(uri);
          if (it != core.documents.end()) {
            activeDoc = it->second;
            hasActiveDoc = true;
          }
        }
        if (hasActiveDoc) {
          scheduleDiagnosticsForText(activeDoc, true, true, 80, 220);
        }
      }
      continue;
    }

    if (method == "nsf/_debugIncludeContextUnits" && params) {
      const Json *uriValue = getObjectValue(*params, "uri");
      Json result = makeArray();
      if (uriValue && uriValue->type == Json::Type::String) {
        std::vector<std::string> units;
        workspaceSummaryRuntimeCollectIncludingUnits({uriValue->s}, units, 256);
        for (const auto &unit : units)
          result.a.push_back(makeString(unit));
      }
      if (id.type != Json::Type::Null)
        writeResponse(id, result);
      continue;
    }

    if (method == "nsf/_debugDocumentRuntime") {
      Json result = makeObject();
      Json documents = makeArray();
      std::vector<std::string> targetUris;
      if (params) {
        const Json *urisValue = getObjectValue(*params, "uris");
        if (urisValue && urisValue->type == Json::Type::Array) {
          for (const auto &uriValue : urisValue->a) {
            if (uriValue.type == Json::Type::String && !uriValue.s.empty())
              targetUris.push_back(uriValue.s);
          }
        }
      }
      if (targetUris.empty()) {
        std::lock_guard<std::mutex> lock(coreMutex);
        targetUris.reserve(core.documents.size());
        for (const auto &entry : core.documents)
          targetUris.push_back(entry.first);
      }
      std::sort(targetUris.begin(), targetUris.end());
      targetUris.erase(std::unique(targetUris.begin(), targetUris.end()),
                       targetUris.end());

      for (const auto &uri : targetUris) {
        Json item = makeObject();
        item.o["uri"] = makeString(uri);
        DocumentRuntime runtime;
        if (!documentOwnerGetRuntime(uri, runtime)) {
          item.o["exists"] = makeBool(false);
          documents.a.push_back(std::move(item));
          continue;
        }
        item.o["exists"] = makeBool(true);
        item.o["version"] = makeNumber(static_cast<double>(runtime.version));
        item.o["epoch"] = makeNumber(static_cast<double>(runtime.epoch));
        item.o["analysisFullFingerprint"] =
            makeString(runtime.analysisSnapshotKey.fullFingerprint);
        item.o["analysisStableFingerprint"] =
            makeString(runtime.analysisSnapshotKey.stableContextFingerprint);
        item.o["workspaceSummaryVersion"] = makeNumber(
            static_cast<double>(
                runtime.analysisSnapshotKey.workspaceSummaryVersion));
        item.o["activeUnitPath"] =
            makeString(runtime.analysisSnapshotKey.activeUnitPath);
        item.o["activeUnitIncludeClosureFingerprint"] =
            makeString(runtime.activeUnitSnapshot.includeClosureFingerprint);
        item.o["activeUnitBranchFingerprint"] =
            makeString(runtime.activeUnitSnapshot.activeBranchFingerprint);
        item.o["activeUnitWorkspaceSummaryVersion"] = makeNumber(
            static_cast<double>(runtime.activeUnitSnapshot.workspaceSummaryVersion));
        item.o["hasInteractiveSnapshot"] =
            makeBool(static_cast<bool>(runtime.interactiveSnapshot));
        item.o["hasLastGoodInteractiveSnapshot"] =
            makeBool(static_cast<bool>(runtime.lastGoodInteractiveSnapshot));
        item.o["hasDeferredDocSnapshot"] =
            makeBool(static_cast<bool>(runtime.deferredDocSnapshot));
        if (runtime.interactiveSnapshot) {
          item.o["interactiveAnalysisFullFingerprint"] =
              makeString(runtime.interactiveSnapshot->key.fullFingerprint);
          item.o["interactiveAnalysisStableFingerprint"] =
              makeString(runtime.interactiveSnapshot->key.stableContextFingerprint);
        }
        if (runtime.lastGoodInteractiveSnapshot) {
          item.o["lastGoodAnalysisFullFingerprint"] =
              makeString(runtime.lastGoodInteractiveSnapshot->key.fullFingerprint);
        }
        if (runtime.deferredDocSnapshot) {
          item.o["deferredAnalysisFullFingerprint"] =
              makeString(runtime.deferredDocSnapshot->key.fullFingerprint);
          item.o["deferredAnalysisStableFingerprint"] =
              makeString(runtime.deferredDocSnapshot->key.stableContextFingerprint);
        }
        item.o["changedRangesCount"] =
            makeNumber(static_cast<double>(runtime.changedRanges.size()));
        documents.a.push_back(std::move(item));
      }
      result.o["documents"] = std::move(documents);
      if (id.type != Json::Type::Null)
        writeResponse(id, result);
      continue;
    }

    if (method == "shutdown") {
      writeResponse(id, makeNull());
      continue;
    }

    if (method == "exit") {
      stopRequestWorkers();
      stopMetricsThread();
      stopDiagnosticsThread();
      deferredDocRuntimeShutdown();
      workspaceSummaryRuntimeShutdown();
      return 0;
    }

    if (method == "textDocument/didOpen" && params) {
      const Json *textDocument = getObjectValue(*params, "textDocument");
      if (textDocument) {
        const Json *uriValue = getObjectValue(*textDocument, "uri");
        const Json *textValue = getObjectValue(*textDocument, "text");
        const Json *versionValue = getObjectValue(*textDocument, "version");
        if (uriValue && textValue && uriValue->type == Json::Type::String &&
            textValue->type == Json::Type::String) {
          std::string normalized = normalizeDocumentText(textValue->s);
          int version = 0;
          if (versionValue && versionValue->type == Json::Type::Number)
            version =
                static_cast<int>(std::llround(getNumberValue(*versionValue)));
          Document document;
          document.uri = uriValue->s;
          document.text = std::move(normalized);
          document.version = version;
          {
            std::lock_guard<std::mutex> lock(coreMutex);
            document.epoch = ++documentEpochCounter;
            core.documents[uriValue->s] = document;
          }
          {
            std::lock_guard<std::mutex> lock(queueMutex);
            latestDocumentVersionByUri[document.uri] = document.version;
          }
          documentOwnerDidOpen(document, makeDocumentRuntimeOptions(),
                               makeRuntimeRequestContext());
          deferredDocRuntimeSchedule(document, makeDeferredDocBuildContext());
          primeDocumentTextCache(document.uri, document.text);
          invalidateFullAstByUri(document.uri);
          invalidateIncludeGraphCacheByUri(document.uri);
          scheduleDiagnosticsForText(document);
        }
      }
      continue;
    }

    if (method == "textDocument/didChange" && params) {
      const Json *textDocument = getObjectValue(*params, "textDocument");
      const Json *changes = getObjectValue(*params, "contentChanges");
      if (textDocument && changes && changes->type == Json::Type::Array &&
          !changes->a.empty()) {
        const Json *uriValue = getObjectValue(*textDocument, "uri");
        const Json *versionValue = getObjectValue(*textDocument, "version");
        if (uriValue && uriValue->type == Json::Type::String) {
          std::string text;
          int oldVersion = 0;
          std::vector<ChangedRange> changedRanges;
          {
            std::lock_guard<std::mutex> lock(coreMutex);
            auto itDoc = core.documents.find(uriValue->s);
            if (itDoc == core.documents.end()) {
              Document created;
              created.uri = uriValue->s;
              created.epoch = ++documentEpochCounter;
              core.documents[uriValue->s] = created;
              itDoc = core.documents.find(uriValue->s);
            }
            text = itDoc->second.text;
            oldVersion = itDoc->second.version;
          }

          auto parsePosition = [&](const Json &pos, int &lineOut,
                                   int &chOut) -> bool {
            if (pos.type != Json::Type::Object)
              return false;
            const Json *lineV = getObjectValue(pos, "line");
            const Json *chV = getObjectValue(pos, "character");
            if (!lineV || !chV || lineV->type != Json::Type::Number ||
                chV->type != Json::Type::Number)
              return false;
            lineOut = static_cast<int>(getNumberValue(*lineV));
            chOut = static_cast<int>(getNumberValue(*chV));
            return true;
          };

          for (const auto &change : changes->a) {
            const Json *textValue = getObjectValue(change, "text");
            if (!textValue || textValue->type != Json::Type::String)
              continue;
            const std::string newText = normalizeDocumentText(textValue->s);

            const Json *rangeValue = getObjectValue(change, "range");
            if (!rangeValue || rangeValue->type == Json::Type::Null) {
              text = newText;
              continue;
            }
            if (rangeValue->type != Json::Type::Object) {
              text = newText;
              continue;
            }
            const Json *startValue = getObjectValue(*rangeValue, "start");
            const Json *endValue = getObjectValue(*rangeValue, "end");
            int startLine = 0;
            int startCh = 0;
            int endLine = 0;
            int endCh = 0;
            if (!startValue || !endValue ||
                !parsePosition(*startValue, startLine, startCh) ||
                !parsePosition(*endValue, endLine, endCh)) {
              text = newText;
              continue;
            }
            int newEndLine = 0;
            int newEndCharacter = 0;
            {
              size_t newLineStart = 0;
              for (size_t idx = 0; idx < newText.size(); idx++) {
                if (newText[idx] == '\n') {
                  newEndLine++;
                  newLineStart = idx + 1;
                }
              }
              const std::string lastLine = newText.substr(newLineStart);
              newEndCharacter =
                  byteOffsetInLineToUtf16(lastLine,
                                          static_cast<int>(lastLine.size()));
            }
            changedRanges.push_back(
                ChangedRange{startLine, startCh, endLine, endCh, newEndLine,
                             newEndCharacter});
            const size_t startOffset =
                positionToOffsetUtf16(text, startLine, startCh);
            const size_t endOffset =
                positionToOffsetUtf16(text, endLine, endCh);
            const size_t lo = std::min(startOffset, endOffset);
            const size_t hi = std::max(startOffset, endOffset);
            std::string updated;
            updated.reserve(text.size() - (hi - lo) + newText.size());
            updated.append(text, 0, lo);
            updated.append(newText);
            if (hi < text.size())
              updated.append(text, hi, std::string::npos);
            text = std::move(updated);
          }

          int nextVersion = oldVersion + 1;
          if (versionValue && versionValue->type == Json::Type::Number) {
            nextVersion =
                static_cast<int>(std::llround(getNumberValue(*versionValue)));
          }
          Document updatedDocument;
          {
            std::lock_guard<std::mutex> lock(coreMutex);
            Document &doc = core.documents[uriValue->s];
            doc.uri = uriValue->s;
            doc.text = text;
            doc.version = nextVersion;
            doc.epoch = ++documentEpochCounter;
            updatedDocument = doc;
          }
          {
            std::lock_guard<std::mutex> lock(queueMutex);
            latestDocumentVersionByUri[updatedDocument.uri] =
                updatedDocument.version;
          }
          scheduleDiagnosticsForText(updatedDocument, true, false, 0, -1,
                                     &changedRanges);
          documentOwnerDidChange(updatedDocument, changedRanges,
                                 makeDocumentRuntimeOptions(),
                                 makeRuntimeRequestContext());
          bool skipExpensivePostChangeWork = false;
          {
            DocumentRuntime runtime;
            if (documentOwnerGetRuntime(updatedDocument.uri, runtime)) {
              const bool commentOnlyEdit =
                  isCommentOnlyEditForDidChange(updatedDocument.text,
                                                changedRanges);
              const bool hasReusableInteractive =
                  runtime.lastGoodInteractiveSnapshot != nullptr;
              const bool hasDeferredSnapshot =
                  runtime.deferredDocSnapshot != nullptr;
              skipExpensivePostChangeWork =
                  hasDeferredSnapshot &&
                  runtime.syntaxOnlyEditHint ||
                  (hasDeferredSnapshot && hasReusableInteractive &&
                   (runtime.semanticNeutralEditHint || commentOnlyEdit));
            }
          }
          if (!skipExpensivePostChangeWork) {
            deferredDocRuntimeSchedule(updatedDocument,
                                       makeDeferredDocBuildContext());
          }
          primeDocumentTextCache(updatedDocument.uri, updatedDocument.text);
          if (!skipExpensivePostChangeWork) {
            invalidateFullAstByUri(updatedDocument.uri);
            invalidateIncludeGraphCacheByUri(updatedDocument.uri);
            scheduleDiagnosticsForText(updatedDocument, false, true, -1, 520);
          }
        }
      }
      continue;
    }

    if (method == "textDocument/didClose" && params) {
      const Json *textDocument = getObjectValue(*params, "textDocument");
      if (textDocument) {
        const Json *uriValue = getObjectValue(*textDocument, "uri");
        if (uriValue && uriValue->type == Json::Type::String) {
          {
            std::lock_guard<std::mutex> lock(coreMutex);
            core.documents.erase(uriValue->s);
          }
          {
            std::lock_guard<std::mutex> lock(queueMutex);
            latestDocumentVersionByUri.erase(uriValue->s);
          }
          invalidateDocumentTextCacheByUri(uriValue->s);
          invalidateFastAstByUri(uriValue->s);
          invalidateFullAstByUri(uriValue->s);
          invalidateIncludeGraphCacheByUri(uriValue->s);
          documentOwnerDidClose(uriValue->s);
          cancelDiagnosticsAndPublishEmpty(uriValue->s);
        }
      }
      continue;
    }
    if (method == "$/cancelRequest" && params) {
      const Json *requestId = getObjectValue(*params, "id");
      if (requestId)
        markRequestCanceled(*requestId);
      continue;
    }

    if (idValue) {
      QueuedRequest request;
      request.method = method;
      request.id = id;
      request.hasId = true;
      request.priority = priorityForMethod(method);
      request.params = params ? *params : makeNull();
      if (params) {
        const Json *textDocument = getObjectValue(*params, "textDocument");
        if (textDocument) {
          const Json *uriValue = getObjectValue(*textDocument, "uri");
          if (uriValue && uriValue->type == Json::Type::String) {
            request.documentUri = uriValue->s;
            std::lock_guard<std::mutex> lock(coreMutex);
            auto docIt = core.documents.find(request.documentUri);
            if (docIt != core.documents.end())
              request.documentVersion = docIt->second.version;
          }
        }
      }
      if (!request.documentUri.empty()) {
        if (method == "textDocument/inlayHint") {
          request.inlayUri = request.documentUri;
          request.latestOnlyKey = "inlay|" + request.documentUri;
        } else if (method == "textDocument/semanticTokens/full") {
          request.latestOnlyKey =
              "semanticTokens/full|" + request.documentUri;
        } else if (method == "textDocument/semanticTokens/range") {
          request.latestOnlyKey =
              "semanticTokens/range|" + request.documentUri;
        } else if (method == "textDocument/documentSymbol") {
          request.latestOnlyKey =
              "documentSymbol|" + request.documentUri;
        } else if (method == "textDocument/references") {
          request.latestOnlyKey =
              "references|" + request.documentUri;
        } else if (method == "textDocument/prepareRename") {
          request.latestOnlyKey =
              "prepareRename|" + request.documentUri;
        } else if (method == "textDocument/rename") {
          request.latestOnlyKey = "rename|" + request.documentUri;
        }
      } else if (method == "workspace/symbol") {
        request.latestOnlyKey = "workspace/symbol";
      }
      enqueueRequest(std::move(request));
    }
  }
  stopRequestWorkers();
  stopMetricsThread();
  stopDiagnosticsThread();
  deferredDocRuntimeShutdown();
  workspaceSummaryRuntimeShutdown();
  return 0;
}
