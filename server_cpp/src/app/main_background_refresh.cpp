#include "main_background_refresh.hpp"

#include <algorithm>
#include <utility>

DiagnosticsBackgroundRuntime::DiagnosticsBackgroundRuntime(
    size_t workerCount, DiagnosticsBackgroundCallbacks callbacksIn)
    : callbacks(std::move(callbacksIn)) {
  schedulerThread = std::thread([this]() { schedulerLoop(); });
  if (workerCount <= 1) {
    workers.emplace_back([this]() { mixedWorkerLoop(); });
  } else {
    workers.emplace_back([this]() { fastWorkerLoop(); });
    for (size_t worker = 1; worker < workerCount; worker++) {
      workers.emplace_back([this]() { fullWorkerLoop(); });
    }
  }
}

DiagnosticsBackgroundRuntime::~DiagnosticsBackgroundRuntime() { stop(); }

void DiagnosticsBackgroundRuntime::enqueueJobs(PendingDiagnosticsJob *fastJob,
                                               PendingDiagnosticsJob *fullJob) {
  if (!fastJob && !fullJob)
    return;
  std::lock_guard<std::mutex> lock(mutex);
  if (fastJob) {
    fastJob->latestOnlySerial = ++serialCounter;
    latestSerialByKey[latestOnlyKey(*fastJob)] = fastJob->latestOnlySerial;
    pendingFast[fastJob->uri] = std::move(*fastJob);
  }
  if (fullJob) {
    fullJob->latestOnlySerial = ++serialCounter;
    latestSerialByKey[latestOnlyKey(*fullJob)] = fullJob->latestOnlySerial;
    pendingFull[fullJob->uri] = std::move(*fullJob);
  }
  const size_t pendingTotal = pendingFast.size() + pendingFull.size();
  const size_t readyTotal = ready[0].size() + ready[1].size();
  callbacks.recordQueueMax(pendingTotal, readyTotal);
  cv.notify_all();
}

void DiagnosticsBackgroundRuntime::cancelAndPublishEmpty(const std::string &uri) {
  size_t canceledJobs = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    canceledJobs += pendingFast.erase(uri);
    canceledJobs += pendingFull.erase(uri);
    const size_t pendingTotal = pendingFast.size() + pendingFull.size();
    const size_t readyTotal = ready[0].size() + ready[1].size();
    callbacks.recordQueueMax(pendingTotal, readyTotal);
  }
  if (canceledJobs > 0) {
    callbacks.recordCanceledPending(canceledJobs);
  }
  callbacks.publishEmptyForUri(uri);
}

bool DiagnosticsBackgroundRuntime::isLatest(const PendingDiagnosticsJob &job) const {
  if (job.latestOnlySerial == 0)
    return true;
  std::lock_guard<std::mutex> lock(mutex);
  auto it = latestSerialByKey.find(latestOnlyKey(job));
  if (it == latestSerialByKey.end())
    return true;
  return it->second == job.latestOnlySerial;
}

DiagnosticsBackgroundQueueSnapshot
DiagnosticsBackgroundRuntime::getQueueSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex);
  DiagnosticsBackgroundQueueSnapshot snapshot;
  snapshot.pendingFast = pendingFast.size();
  snapshot.pendingFull = pendingFull.size();
  snapshot.readyFast = ready[0].size();
  snapshot.readyFull = ready[1].size();
  return snapshot;
}

void DiagnosticsBackgroundRuntime::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (stopping)
      return;
    stopping = true;
    pendingFast.clear();
    pendingFull.clear();
    ready[0].clear();
    ready[1].clear();
  }
  cv.notify_all();
  if (schedulerThread.joinable())
    schedulerThread.join();
  for (auto &worker : workers) {
    if (worker.joinable())
      worker.join();
  }
  workers.clear();
}

std::string DiagnosticsBackgroundRuntime::latestOnlyKey(
    const PendingDiagnosticsJob &job) const {
  return job.uri + "|" + (job.kind == DiagnosticsJobKind::Fast ? "fast" : "full");
}

PendingDiagnosticsJob DiagnosticsBackgroundRuntime::takeNextReadyJob() {
  std::unique_lock<std::mutex> lock(mutex);
  cv.wait(lock, [&]() {
    if (stopping)
      return true;
    return !ready[static_cast<int>(DiagnosticsQueuePriority::Fast)].empty() ||
           !ready[static_cast<int>(DiagnosticsQueuePriority::Full)].empty();
  });
  if (stopping)
    return PendingDiagnosticsJob{};
  for (int i = 0; i < 2; i++) {
    if (!ready[i].empty()) {
      PendingDiagnosticsJob job = std::move(ready[i].front());
      ready[i].pop_front();
      const auto now = std::chrono::steady_clock::now();
      if (job.readyAt.time_since_epoch().count() > 0) {
        const double waitMs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - job.readyAt)
                .count());
        callbacks.recordQueueWait(waitMs);
      }
      const size_t pendingTotal = pendingFast.size() + pendingFull.size();
      const size_t readyTotal = ready[0].size() + ready[1].size();
      callbacks.recordQueueMax(pendingTotal, readyTotal);
      return job;
    }
  }
  return PendingDiagnosticsJob{};
}

PendingDiagnosticsJob DiagnosticsBackgroundRuntime::takeNextReadyJobForPriority(
    DiagnosticsQueuePriority priority) {
  const int idx = static_cast<int>(priority);
  std::unique_lock<std::mutex> lock(mutex);
  cv.wait(lock, [&]() {
    if (stopping)
      return true;
    return !ready[idx].empty();
  });
  if (stopping)
    return PendingDiagnosticsJob{};
  PendingDiagnosticsJob job = std::move(ready[idx].front());
  ready[idx].pop_front();
  const auto now = std::chrono::steady_clock::now();
  if (job.readyAt.time_since_epoch().count() > 0) {
    const double waitMs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - job.readyAt)
            .count());
    callbacks.recordQueueWait(waitMs);
  }
  const size_t pendingTotal = pendingFast.size() + pendingFull.size();
  const size_t readyTotal = ready[0].size() + ready[1].size();
  callbacks.recordQueueMax(pendingTotal, readyTotal);
  return job;
}

void DiagnosticsBackgroundRuntime::schedulerLoop() {
  std::unique_lock<std::mutex> lock(mutex);
  while (!stopping) {
    if (pendingFast.empty() && pendingFull.empty()) {
      cv.wait(lock, [&]() {
        return stopping || !pendingFast.empty() || !pendingFull.empty();
      });
      continue;
    }

    auto nextDue = std::chrono::steady_clock::time_point::max();
    for (const auto &entry : pendingFast) {
      nextDue = std::min(nextDue, entry.second.due);
    }
    for (const auto &entry : pendingFull) {
      nextDue = std::min(nextDue, entry.second.due);
    }

    cv.wait_until(lock, nextDue, [&]() { return stopping; });
    if (stopping)
      break;

    const auto now = std::chrono::steady_clock::now();
    auto collectReady = [&](std::unordered_map<std::string, PendingDiagnosticsJob> &pending,
                            DiagnosticsQueuePriority priority) {
      for (auto it = pending.begin(); it != pending.end();) {
        if (it->second.due <= now) {
          it->second.readyAt = now;
          ready[static_cast<int>(priority)].push_back(std::move(it->second));
          it = pending.erase(it);
        } else {
          ++it;
        }
      }
    };
    collectReady(pendingFast, DiagnosticsQueuePriority::Fast);
    collectReady(pendingFull, DiagnosticsQueuePriority::Full);
    const size_t pendingTotal = pendingFast.size() + pendingFull.size();
    const size_t readyTotal = ready[0].size() + ready[1].size();
    callbacks.recordQueueMax(pendingTotal, readyTotal);
    cv.notify_all();
  }
}

void DiagnosticsBackgroundRuntime::mixedWorkerLoop() {
  while (true) {
    PendingDiagnosticsJob job = takeNextReadyJob();
    if (job.uri.empty())
      break;
    if (!isLatest(job)) {
      callbacks.recordLatestOnlyDrop();
      continue;
    }
    callbacks.publishJob(job);
  }
}

void DiagnosticsBackgroundRuntime::fastWorkerLoop() {
  while (true) {
    PendingDiagnosticsJob job =
        takeNextReadyJobForPriority(DiagnosticsQueuePriority::Fast);
    if (job.uri.empty())
      break;
    if (!isLatest(job)) {
      callbacks.recordLatestOnlyDrop();
      continue;
    }
    callbacks.publishJob(job);
  }
}

void DiagnosticsBackgroundRuntime::fullWorkerLoop() {
  while (true) {
    PendingDiagnosticsJob job =
        takeNextReadyJobForPriority(DiagnosticsQueuePriority::Full);
    if (job.uri.empty())
      break;
    if (!isLatest(job)) {
      callbacks.recordLatestOnlyDrop();
      continue;
    }
    callbacks.publishJob(job);
  }
}
