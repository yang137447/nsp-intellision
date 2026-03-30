#pragma once

#include "diagnostics.hpp"
#include "document_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Background diagnostics queue/runtime owned by app/main.cpp.
//
// Responsibilities:
// - own latest-only diagnostics queue state and worker threads
// - schedule fast/full diagnostics jobs and publish empty clears on cancel
// - keep queue wait/max/cancel accounting outside the entry file
//
// Non-goals:
// - does not build diagnostics itself; app/main.cpp still provides the publish
//   callback so startup wiring can control current-doc/deferred interactions
// - does not own request-lane workers or LSP message dispatch

enum class DiagnosticsJobKind : int { Fast = 0, Full = 1 };
enum class DiagnosticsQueuePriority : int { Fast = 0, Full = 1 };

struct PendingDiagnosticsJob {
  DiagnosticsJobKind kind = DiagnosticsJobKind::Fast;
  bool hasPairedFull = false;
  int documentVersion = 0;
  uint64_t documentEpoch = 0;
  uint64_t latestOnlySerial = 0;
  std::string analysisFingerprint;
  std::string uri;
  std::string text;
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  std::vector<ChangedRange> changedRanges;
  std::unordered_map<std::string, int> defines;
  std::string activeUnitUri;
  std::string activeUnitText;
  DiagnosticsBuildOptions diagnosticsOptions;
  std::chrono::steady_clock::time_point enqueuedAt;
  std::chrono::steady_clock::time_point due;
  std::chrono::steady_clock::time_point readyAt;
};

struct DiagnosticsBackgroundCallbacks {
  std::function<void(const PendingDiagnosticsJob &)> publishJob;
  std::function<void(const std::string &)> publishEmptyForUri;
  std::function<void(double)> recordQueueWait;
  std::function<void(size_t, size_t)> recordQueueMax;
  std::function<void(size_t)> recordCanceledPending;
  std::function<void()> recordLatestOnlyDrop;
};

struct DiagnosticsBackgroundQueueSnapshot {
  size_t pendingFast = 0;
  size_t pendingFull = 0;
  size_t readyFast = 0;
  size_t readyFull = 0;
};

class DiagnosticsBackgroundRuntime {
public:
  DiagnosticsBackgroundRuntime(size_t workerCount,
                               DiagnosticsBackgroundCallbacks callbacks);

  ~DiagnosticsBackgroundRuntime();

  DiagnosticsBackgroundRuntime(const DiagnosticsBackgroundRuntime &) = delete;
  DiagnosticsBackgroundRuntime &
  operator=(const DiagnosticsBackgroundRuntime &) = delete;

  void enqueueJobs(PendingDiagnosticsJob *fastJob, PendingDiagnosticsJob *fullJob);

  void cancelAndPublishEmpty(const std::string &uri);

  bool isLatest(const PendingDiagnosticsJob &job) const;

  DiagnosticsBackgroundQueueSnapshot getQueueSnapshot() const;

  void stop();

private:
  std::string latestOnlyKey(const PendingDiagnosticsJob &job) const;

  PendingDiagnosticsJob takeNextReadyJob();

  PendingDiagnosticsJob
  takeNextReadyJobForPriority(DiagnosticsQueuePriority priority);

  void schedulerLoop();
  void mixedWorkerLoop();
  void fastWorkerLoop();
  void fullWorkerLoop();

  DiagnosticsBackgroundCallbacks callbacks;

  mutable std::mutex mutex;
  std::condition_variable cv;
  bool stopping = false;
  std::unordered_map<std::string, PendingDiagnosticsJob> pendingFast;
  std::unordered_map<std::string, PendingDiagnosticsJob> pendingFull;
  std::deque<PendingDiagnosticsJob> ready[2];
  uint64_t serialCounter = 0;
  std::unordered_map<std::string, uint64_t> latestSerialByKey;

  std::thread schedulerThread;
  std::vector<std::thread> workers;
};
