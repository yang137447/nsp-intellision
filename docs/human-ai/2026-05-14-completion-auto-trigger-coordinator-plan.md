# Completion Auto-Trigger Burst Coordinator Plan

## Purpose

This is an executable handoff for the next thread. It explains why the current
completion performance work should move from server optimization to
auto-trigger burst control, and it defines a staged implementation plan.

This document is a human/AI handoff note under `docs/human-ai/`; it is not a
promoted current-fact document.

## Required Reading For Next Thread

Start with the repository-required documents:

1. `README.md`
2. `docs/architecture.md`
3. `docs/resources.md`
4. `docs/testing.md`

Then read:

1. `docs/human-ai/2026-05-14-pbr-flow-water-real-replay-performance-plan.md`
2. This document

## Background

The real replay target is:

- Replay script: `src/test/replay/scripts/rw-pbr-flow-water-full-input.json`
- Report path: `out/test/perf-reports/real-replay/rw-pbr-flow-water-full-input.json`
- Real workspace file:
  `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\sfx\nodes\pbr_flow_water_nodes.hlsl`
- Active unit:
  `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\sfx\pbr_flow_water.nsf`

The original performance symptom was high completion capture time in the full
input replay, often around 0.8s to 1.8s. Direct server completion was already
fast, but the old replay number mixed native/editor trigger, suggest UI,
`vscode.executeCompletionItemProvider`, client provider work, LSP roundtrip,
and direct server comparison.

Recent work separated the report into:

- `uiCoverage`: native/editor-triggered request coverage.
- `providerVerification`: explicit `vscode.execute*Provider` correctness and
  latency verification after UI requests go quiet.
- `directServerCompletion`: direct server comparison.
- `latencySummary`: aggregate P50/P95/max, slowest probes, duplicated request
  paths, UI queue quiet and request burst counts.

## Current Evidence

Latest `pbr-flow-water-full-input` replay after request-sequence capture:

- Completion provider verification: avg 19.5ms, P50 13ms, P95/max 58ms.
- Completion client LSP request during provider verification: avg 8.1ms,
  P50 4ms, P95/max 39ms.
- Direct server completion: avg 9.1ms, P50 4ms, P95/max 45ms.
- Completion duplicated request paths: 0 / 17.
- Completion UI queue quiet timeouts: 0 / 17.
- Completion UI request burst count: avg 6.3 requests, P50 6, P95/max 15.
- Completion UI queue quiet wait: avg 796.9ms, P50 794ms, P95/max 1837ms.

Slowest completion probe:

- Label: `local variable completion after foam_structure prefix`
- UI coverage burst: 15 provider requests.
- Prefix lengths advance from 6 through 14.
- The final prefix length 14 appears multiple times.
- Several repeated final-prefix requests complete around 1967-1968ms.
- Provider verification for the same probe remains fast after the UI queue is
  quiet.

Conclusion:

- Do not optimize the server semantic completion query based on this replay.
- The slow product-facing area is native/editor auto-trigger request burst and
  stale / duplicate request accumulation.
- Any product-side change here touches completion trigger / cancellation
  behavior and must be confirmed before implementation.

## Desired End State

The completion path should:

- Preserve correctness for the latest cursor position and prefix.
- Preserve explicit user-invoked completion behavior.
- Preserve member completion after `.`.
- Preserve server-side completion candidates and ordering.
- Avoid sending stale identifier-prefix auto-trigger requests into the LSP
  queue when newer requests supersede them.
- Report separate counts for VS Code/provider requests received, requests sent
  to LSP, requests coalesced before LSP, and stale responses dropped after LSP.

## Phase A: Report-Only Coalescing Simulation

Status: implemented in the current working tree.

Behavior change: none.

Goal:

Use the existing `uiCoverage.providerRequestSequence` to simulate a future
completion coordinator without changing runtime behavior.

Implementation area:

- `src/test/replay/real_workspace_replay_report_writer.ts`
- `src/test/suite/client.real-workspace-replay-runner.test.ts`
- `docs/testing.md`
- This handoff or the performance plan document

Simulation should output per completion probe:

- `simulatedReceivedRequests`
- `simulatedExecutedRequests`
- `simulatedCoalescedRequests`
- `simulatedRetainedSequences`
- `simulatedDroppedSequences`
- `latestPrefixLength`
- `wouldReduceBurstBy`
- Optional comparison across debounce windows: 25ms, 40ms, 60ms

Suggested simulation rules:

- Only simulate identifier-prefix quick-suggest / auto-trigger completion.
- Do not simulate coalescing for explicit user invoke.
- Do not simulate coalescing for `.` member completion.
- Do not simulate coalescing for signature help.
- Group requests by same line and same identifier start column.
- Treat monotonically increasing prefix length as one burst.
- For duplicate requests at the same final prefix length, retain the last one.
- For a short debounce window, retain the latest request in each window.

Important note:

`triggerKind` alone is not enough. Quick suggest can appear as `Invoke`, so the
simulation should use prefix length, line / character, trigger character, and
request timing together.

Acceptance for Phase A:

- `npm run compile` passes.
- `$env:NSF_TEST_FILE_FILTER='real-workspace-replay'; npm run test:client:repo`
  passes.
- Real replay passes:

```powershell
$env:NSF_REAL_REPLAY_INCLUDE_LONG = "1"
$env:NSF_TEST_REAL_REPLAY_SCRIPT_FILTER = "pbr-flow-water-full-input"
npm run test:client:real:replay
```

- The report shows how many requests would be retained / dropped for the
  slowest probes.
- The slowest `foam_structure` probe should simulate a clear reduction from
  its current 15-request burst.
- No product behavior changes in this phase.

Execution result on 2026-05-14:

- `latencySummary.completion.coalescingSimulation` now reports aggregate and
  per-probe report-only simulation results for 25ms / 40ms / 60ms windows.
- The simulation keeps explicit invoke, `.` member completion and unknown
  request classes as bypassed / retained requests.
- The `pbr-flow-water-full-input` replay passed with 0 anomalies. With the
  default 40ms simulation, completion UI/provider requests were 98 received,
  31 retained/executed and 67 coalesced; 15 requests were bypassed.
- The slowest `foam_structure` completion probe simulated 15 received requests,
  2 retained/executed requests and 13 coalesced requests.

## Phase B: Client Completion Request Coordinator

Status: implemented in the current working tree after user confirmation.

Behavior change: yes. Stop and confirm before implementation.

Proposed new module:

```text
client/src/client_completion_request_coordinator.ts
```

Responsibilities:

- Classify completion request source:
  - explicit invoke
  - identifier-prefix auto-trigger / quick suggest
  - member trigger
  - retrigger
  - unknown
- Build a coalescing key for safe identifier-prefix bursts.
- Schedule short-window latest-only execution.
- Execute only the latest safe-to-coalesce request via `next(...)`.
- Return a neutral result for superseded stale requests.
- Record metrics.

First product-side version should be conservative:

- Only completion, not signature help.
- Only identifier-prefix auto-trigger.
- Explicit invoke bypasses coordinator.
- Member completion after `.` bypasses coordinator.
- Unknown / ambiguous trigger source bypasses coordinator.
- No changes to server completion assembly.
- No changes to completion item filtering, ordering or documentation.

Potential coalescing key:

- document URI
- line
- identifier start column
- context class: identifier-prefix auto-trigger

Do not include document version as a hard grouping key, because continuous
typing creates a new version per character and would prevent coalescing. Use
document version only for ordering / staleness.

Suggested debounce windows:

- Start with 25-40ms.
- Keep the window short to reduce UI flicker and stale work without delaying
  visible suggestions excessively.

Metrics to add:

- `completionCoordinatorReceivedCount`
- `completionCoordinatorExecutedCount`
- `completionCoordinatorCoalescedBeforeLspCount`
- `completionCoordinatorStaleDroppedAfterLspCount`
- `completionCoordinatorBypassedExplicitCount`
- `completionCoordinatorBypassedMemberCount`
- `completionCoordinatorBypassedUnknownCount`

Replay summary should report per probe:

- received request burst count
- executed LSP count
- coalesced-before-LSP count
- stale-dropped-after-LSP count
- retained request sequences
- dropped request sequences
- latest prefix length

Acceptance for Phase B:

- `npm run compile`
- `$env:NSF_TEST_FILE_FILTER='real-workspace-replay'; npm run test:client:repo`
- Completion-focused repo tests, at minimum completion auto-trigger, completion
  client metrics, member completion and interactive visibility tests.
- Real replay command above.
- `pbr-flow-water-full-input` keeps replay anomalies at 0.
- Expected completion labels remain present.
- Provider verification P95 stays below 100ms.
- Direct server completion remains near the current range.
- Executed LSP request count per completion UI burst drops substantially,
  ideally toward 1-2 for identifier-prefix probes.
- Explicit completion and `.` member completion do not regress.

Execution result on 2026-05-14:

- Added `client/src/client_completion_request_coordinator.ts`.
- Runtime behavior is limited to identifier-prefix auto-trigger / quick suggest
  before-LSP latest-only coalescing. Explicit invoke, `.` member completion,
  member-prefix completion, retrigger and unknown requests bypass the
  coordinator.
- Provider timing entries now record `completionCoordinatorAction`, source, key
  and coordinator prefix length. Real replay reports
  `latencySummary.completion.coordinatorActual` for actual retained / dropped
  request sequences.
- `pbr-flow-water-full-input` replay passed with 0 anomalies. Actual
  coordinator counts were 108 received, 39 executed LSP, 69 coalesced before
  LSP, 0 stale-dropped-after-LSP and 0 cancelled-before-LSP.
- The slowest `foam_structure` probe had 15 actual coordinator requests, 1
  executed LSP request and 14 coalesced-before-LSP requests.

## Phase C: Stale Response Guard

Status: optional follow-up after Phase B.

Behavior change: yes. Stop and confirm before implementation.

Purpose:

Even after before-LSP coalescing, some requests may already be in flight. When
an older request returns after a newer request has superseded it, guard against
publishing stale results.

Rules:

- Latest request returns normally.
- Stale auto-trigger request returns neutral result and records
  `staleDroppedAfterLsp`.
- Explicit invoke and member completion should remain conservative / bypassed
  unless evidence proves they need guarding.

This is a safety guard, not the main optimization. The main benefit should come
from Phase B before-LSP coalescing.

## Phase D: UI Executed Request Attribution

Status: implemented in the current working tree.

Behavior change: none.

Purpose:

After Phase B, determine whether remaining `uiQueueQuiet` time comes from the
latest UI/native request that still executes LSP work, or from post-completion
quiet / cleanup waiting in the replay harness.

Implementation result:

- `ProviderQueueQuietReport` now records absolute and capture-relative start /
  completion times.
- `latencySummary.completion.uiExecutedLspRequest` summarizes LSP request time
  for `uiCoverage.providerRequestSequence` entries whose
  `completionCoordinatorAction` is `executed`.
- `latencySummary.completion.uiExecutedAttribution` reports per-probe latest
  executed request sequence, prefix length, LSP time, provider total, wait until
  latest executed request completion and post-completion quiet time.

Latest `pbr-flow-water-full-input` result:

- Replay passed with 0 anomalies.
- Completion coordinator actual counts: 99 received, 36 executed LSP, 63
  coalesced before LSP, 0 stale dropped and 0 cancelled.
- UI executed LSP request time: avg 752.7ms, P50 759ms, P95/max 1571ms.
- Wait until latest executed request completion: avg 710.5ms, P50 711ms,
  P95/max 1523ms.
- Post latest executed quiet time: avg 28.4ms, P50 27ms, P95/max 51ms.
- Slowest `foam_structure` probe: latest executed sequence 118, LSP 1571ms,
  wait-until-complete 1523ms, post-quiet 29ms.

Conclusion:

- Remaining long UI/native completion wait is not mostly replay cleanup quiet.
- It is concentrated in the live UI-triggered request that still reaches LSP.
- Direct server and provider verification remain fast after queue quiet, so the
  next investigation should attribute why live dirty-document UI requests spend
  1s+ in the LSP request path while the same final position is fast after the
  queue settles.

## Phase E: Live UI LSP Path Attribution

Status: implemented in the current working tree.

Behavior change: none.

Purpose:

Determine whether the remaining live UI/native completion LSP time is spent in
server queue wait, server request-context snapshot build, completion handler
work, or client/transport time outside those server-side buckets.

Implementation result:

- `ServerRequestContext` now carries attribution-only request queue wait,
  request context build time and request document version from the request
  worker. These fields are debug/report data only and must not drive behavior.
- `CompletionDebugSnapshot` now reports document/version state, prefix, item
  count, request queue wait, context build, handler total, interactive collect,
  member base/query, item assembly and response write timings.
- The replay runner captures `uiCoverage.latestCompletionDebug` after the UI
  completion queue is quiet and before provider verification overwrites the
  last completion debug snapshot.
- `latencySummary.completion.uiExecutedAttribution` now includes
  `latestExecutedServerAttribution` per probe and aggregate stats for server
  queue wait, context build, completion handler and residual client/transport
  LSP time.
- Client provider timing now binds converter / LSP request measurements to the
  provider draft that is actively executing `next(...)`, rather than using the
  lifecycle stack. This keeps coalescing delays and overlapping provider
  requests from attributing an older LSP request to a newer request sequence.

Validation so far:

- `npm run compile` passed.
- `cmake --build .\server_cpp\build` passed after stopping a running
  `nsf_lsp.exe` held by the editor extension host.
- `$env:NSF_TEST_FILE_FILTER='real-workspace-replay'; npm run test:client:repo`
  passed.
- `$env:NSF_TEST_FILE_FILTER='completion-client-metrics'; npm run
  test:client:repo` passed.
- `$env:NSF_TEST_FILE_FILTER='member-completion'; npm run test:client:repo`
  passed.
- `$env:NSF_TEST_FILE_FILTER='completion-auto-trigger'; npm run
  test:client:repo` passed.
- `$env:NSF_REAL_REPLAY_INCLUDE_LONG='1';
  $env:NSF_TEST_REAL_REPLAY_SCRIPT_FILTER='pbr-flow-water-full-input'; npm run
  test:client:real:replay` passed with 0 anomalies after the timing binding
  fix.

Latest `pbr-flow-water-full-input` result after Phase E:

- Completion coordinator actual counts: 102 received, 39 executed LSP, 63
  coalesced before LSP, 0 stale dropped and 0 cancelled.
- Completion provider verification: avg 37.2ms, P50 35ms, P95/max 65ms.
- Client LSP verification: avg 7.8ms, P50 4ms, P95/max 28ms.
- Direct server completion: avg 7.5ms, P50 3ms, P95/max 30ms.
- UI executed LSP request time: avg 833.5ms, P50 664ms, P95/max 1745ms.
- Server attribution for latest executed UI requests: queue wait avg 0.4ms /
  max 1.9ms, context build avg/max 0ms after rounding, completion handler avg
  2.1ms / max 4.1ms.
- Residual client/transport LSP time remains high: avg 831ms, P50 662.1ms,
  P95/max 1741.8ms.
- Slowest `foam_structure` probe matched server attribution for the latest
  executed request: LSP 1745ms, server-known time 3.18ms, residual 1741.8ms.

Conclusion:

- The remaining long UI/native wait is not server completion query, server
  queueing, or request-context build time.
- The next optimization target should be the client / VS Code LanguageClient
  path before or around `sendRequest`, especially pending text-document sync
  while typing dirty buffers and the interaction between bypassed explicit /
  member requests and latest identifier-prefix requests.
- If more precision is needed for mixed windows, add a recent completion debug
  ring or request correlation before changing behavior; a single
  `lastCompletionDebug` can point at an earlier bypass request when multiple
  completion requests finish in the same quiet window.

## Phase F: Client SendRequest Attribution

Status: implemented in the current working tree.

Behavior change: none.

Purpose:

Determine whether the high residual LSP time comes from coordinator delay before
`next(...)`, client text-document version changes before the request is sent,
overlapping provider execution, or the `LanguageClient.sendRequest(...)`
promise after the request is handed to the VS Code language client.

Implementation result:

- Provider timing now records:
  - same-kind provider lifecycle overlap at provider start
  - same-kind active `next(...)` overlap at `next(...)` start
  - `nextWaitMs` and `nextExecutionMs`
  - LSP request start / completion timestamps, `lspStartDelayMs`,
    `lspRequestCount` and completion-to-provider-return time
  - document version / dirty state at provider start, `next(...)` start, LSP
    start and provider return
- Replay `providerRequestSequence` carries these fields.
- `latencySummary.completion.uiExecutedAttribution` now reports
  `latestExecutedClientAttribution` per probe and aggregate stats for client
  next wait, next execution, LSP start delay, active overlap and document
  version advancement counts.

Validation:

- `npm run compile` passed.
- `$env:NSF_TEST_FILE_FILTER='real-workspace-replay'; npm run test:client:repo`
  passed.
- `$env:NSF_TEST_FILE_FILTER='completion-client-metrics'; npm run
  test:client:repo` passed.
- `$env:NSF_TEST_FILE_FILTER='completion-auto-trigger'; npm run
  test:client:repo` passed.
- `$env:NSF_REAL_REPLAY_INCLUDE_LONG='1';
  $env:NSF_TEST_REAL_REPLAY_SCRIPT_FILTER='pbr-flow-water-full-input'; npm run
  test:client:real:replay` passed with 0 anomalies.

Latest `pbr-flow-water-full-input` result after Phase F:

- Completion coordinator actual counts: 89 received, 37 executed LSP, 52
  coalesced before LSP, 0 stale dropped and 0 cancelled.
- Completion provider verification: avg 34.8ms, P50 39ms, P95/max 60ms.
- Client LSP verification: avg 10.7ms, P50 4ms, P95/max 58ms.
- Direct server completion: avg 8.5ms, P50 5ms, P95/max 34ms.
- UI executed LSP request time: avg 871.6ms, P50 887ms, P95/max 1756ms.
- Client next wait / LSP start delay: avg 28.3ms, P50 29ms, P95/max 30ms.
  This is essentially the coordinator debounce window, not the long tail.
- Active same-kind overlap at provider start: avg 2.7, P95/max 4. Active
  `next(...)` overlap at start: avg 1.8, P95/max 4.
- Document version advanced before next/LSP/during LSP in only 1 of 10 latest
  executed attribution probes.
- Server queue/context/handler remained small: queue avg/max 0ms after
  rounding, context avg 0.3ms / max 3.2ms, handler avg 5ms / max 31.6ms.
- Residual client/transport LSP time remained high: avg 866.2ms, P50 885.2ms,
  P95/max 1754ms.

Conclusion:

- The long wait is not caused by the 25ms coordinator window and is not mainly
  caused by document versions continuing to advance during the measured latest
  request.
- The dominant remaining signal is overlapping same-kind provider / `next(...)`
  execution while `LanguageClient.sendRequest(...)` promises stay pending even
  though the server handles matching requests in a few milliseconds.
- The next behavior change should target in-flight auto-trigger overlap: for
  identifier-prefix coordinator keys, keep at most one in-flight LSP request,
  retain only the latest superseding request while one is in flight, and return
  a neutral result for superseded stale auto-trigger responses. Explicit invoke,
  `.` member completion, member-prefix completion, retrigger and unknown
  requests should remain bypassed.

## Phase G Review: In-Flight Auto-Trigger Optimization

Status: reviewed; not implemented.

Behavior change: yes. Stop and confirm before implementation.

### Review verdict

The optimization direction is valid: remaining completion latency is strongly
correlated with overlapping same-kind auto-trigger provider / `next(...)`
execution, while server-side completion work stays in the low-millisecond range.

However, the naive version of the plan should not be implemented as written:

- "Strictly keep at most one underlying LSP request in flight per coordinator
  key" is only safe if the older in-flight request can be cancelled or detached
  without blocking the latest request.
- The current client middleware does not own a cancellation source for a
  `next(...)` call after it has been handed to VS Code / LanguageClient.
- If the implementation simply waits for an old in-flight request to finish
  before starting the latest request, the latest cursor position can be blocked
  behind the exact 0.9s-1.7s stale `sendRequest(...)` promise we are trying to
  avoid.

Recommended conclusion:

- Keep the target: reduce visible stale auto-trigger overlap for the same
  identifier-prefix key.
- Do not start with strict underlying-LSP serialization.
- First implement a conservative visible-provider stale guard that resolves
  superseded identifier-prefix auto-trigger provider promises neutrally while
  observing and ignoring their already-started `next(...)` promises.
- Only consider strict single-underlying-LSP serialization later if detached
  stale LSP cleanup still proves to be the dominant bottleneck.

### Evidence supporting the direction

Latest Phase F `pbr-flow-water-full-input` replay:

- Completion coordinator actual counts: 89 received, 37 executed LSP, 52
  coalesced before LSP, 0 stale dropped and 0 cancelled.
- UI executed LSP request time: avg 871.6ms, P50 887ms, P95/max 1756ms.
- Client next wait / LSP start delay: avg 28.3ms, P95/max 30ms, matching the
  short coordinator window rather than the long tail.
- Active same-kind provider overlap at provider start: avg 2.7, P95/max 4.
- Active same-kind `next(...)` overlap at start: avg 1.8, P95/max 4.
- Document version advanced before next/LSP/during LSP in only 1 of 10 latest
  executed attribution probes.
- Server queue/context/handler remained small; residual client/transport LSP
  time remained high.

Interpretation:

- The coordinator debounce window is not the problem.
- Server semantic completion is not the problem.
- Ongoing dirty-document typing is not the primary measured cause for most
  latest executed requests.
- The risky area is overlapped provider lifecycle and pending
  `LanguageClient.sendRequest(...)` promises.

### Recommended behavior contract

Scope:

- Apply only to `identifierPrefixAutoTrigger` requests with a coordinator key.
- Keep explicit invoke, `.` member completion, member-prefix completion,
  retrigger and unknown requests bypassed.
- Do not change server completion candidates, item ordering, filtering,
  documentation or trigger characters.
- Do not apply to signature help.

Per-key state:

- `pendingBeforeLsp`: the existing short-window latest-only request before
  `next(...)` starts.
- `visibleInFlight`: the provider promise currently representing an executed
  auto-trigger request for that key.
- `latestSupersedingRequest`: the latest same-key request observed after
  `visibleInFlight` started.
- A monotonically increasing generation / epoch for stale checks.

Rules:

- Before LSP starts, retain the existing Phase B behavior: replace pending
  same-key identifier-prefix requests and resolve superseded pending requests
  as neutral.
- Once a same-key auto-trigger request is visibly in flight, a newer same-key
  auto-trigger request marks the older visible request stale.
- Superseded visible requests should resolve neutral for VS Code as soon as the
  coordinator can safely do so, while the already-started `next(...)` promise is
  observed in the background and its result is ignored.
- The latest same-key request remains the only request eligible to publish
  results.
- Any detached `next(...)` promise must have an error handler; cancellation,
  rejection or late completion must not surface as an unhandled rejection.
- If the latest request's own token is already cancelled before execution,
  return neutral and record `cancelledBeforeLsp`.
- If a stale request's underlying `next(...)` later completes, record it as
  stale / detached cleanup, not as a publishable completion result.

### Metrics to add or clarify

Keep existing metrics:

- `completionCoordinatorReceivedCount`
- `completionCoordinatorExecutedCount`
- `completionCoordinatorCoalescedBeforeLspCount`
- `completionCoordinatorStaleDroppedAfterLspCount`
- `completionCoordinatorCancelledBeforeLspCount`
- bypass counts

Add or split metrics if implementing early neutralization:

- `completionCoordinatorSupersededInFlightCount`
- `completionCoordinatorDetachedLspCount`
- `completionCoordinatorDetachedLspErrorCount`
- `completionCoordinatorNeutralResolvedWhileInFlightCount`
- `completionCoordinatorLatestRetainedWhileInFlightCount`

Replay attribution should preserve request sequence clarity:

- original provider sequence
- action: `executed`, `coalescedBeforeLsp`, `staleResolvedWhileInFlight`,
  `cancelledBeforeLsp`, `cancelledWhileInFlight` or bypass action
- generation / key / prefix length
- detached LSP cleanup and detached error counts from coordinator snapshot

### Risk review

Risk: latest request head-of-line blocking.

- Trigger: strict single-underlying-LSP serialization waits for old in-flight
  requests before executing the latest request.
- Impact: visible latest completion can become slower than the current system.
- Mitigation: do not start with strict serialization; prefer early neutral
  resolution for stale visible provider promises.

Risk: detached LSP promise leaks or unhandled rejections.

- Trigger: old `next(...)` continues after the visible provider promise has
  returned neutral.
- Impact: noisy errors, retained closures, or process-level unhandled rejection.
- Mitigation: always attach `then/catch/finally`, record detached cleanup
  metrics, and clear per-key state by generation.

Risk: stale result publication.

- Trigger: old request completes after a newer same-key request was retained.
- Impact: UI can show candidates for an older prefix.
- Mitigation: generation check before resolving any visible provider promise
  with real completion results; stale generations resolve neutral only.

Risk: suppressing useful intermediate suggestions.

- Trigger: user pauses briefly on an intermediate prefix and then continues.
- Impact: fewer intermediate suggestion updates.
- Mitigation: scope only to same-key identifier-prefix auto-trigger; explicit
  invoke still bypasses and returns full results immediately.

Risk: member completion regression.

- Trigger: misclassifying member-prefix or dot completion as identifier-prefix.
- Impact: object fields / methods can disappear or arrive late.
- Mitigation: keep current member bypass checks, including `triggerCharacter`
  `.` and `previousCharacter === '.'`; add focused member completion regression.

Risk: ambiguous quick suggest vs explicit invoke.

- Trigger: VS Code can report quick suggest as `Invoke`.
- Impact: explicit user requests could be treated as stale auto-trigger.
- Mitigation: only treat ambiguous `Invoke` as auto-trigger inside the existing
  recent identifier burst window and same key; true standalone invoke remains
  bypassed.

Risk: extra complexity in `client_completion_request_coordinator.ts`.

- Trigger: mixing pending-before-LSP, visible-in-flight and detached cleanup in
  one method.
- Impact: future maintenance gets brittle.
- Mitigation: split per-key state transitions into small private helpers and
  keep the coordinator as the single owner of this scheduling behavior.

### Acceptance criteria

Correctness:

- Explicit completion still bypasses and returns results.
- `.` member completion and member-prefix completion still bypass.
- Retrigger and unknown classifications still bypass.
- Latest identifier-prefix auto-trigger returns expected labels.
- Superseded identifier-prefix auto-trigger requests return neutral and never
  publish stale results.
- Detached `next(...)` cleanup has no unhandled rejections.

Performance:

- `pbr-flow-water-full-input` keeps 0 anomalies.
- Provider verification P95 remains below 100ms.
- Direct server completion remains in the current low-millisecond range.
- UI queue quiet P50/P95 should improve versus Phase F, or at minimum not
  regress.
- Same-key visible provider overlap should drop materially.
- Detached LSP cleanup counts should be reported separately so improvement is
  not hidden by background cleanup.

Recommended validation:

```powershell
npm run compile
$env:NSF_TEST_FILE_FILTER='completion-auto-trigger'; npm run test:client:repo
$env:NSF_TEST_FILE_FILTER='completion-client-metrics'; npm run test:client:repo
$env:NSF_TEST_FILE_FILTER='member-completion'; npm run test:client:repo
$env:NSF_TEST_FILE_FILTER='interactive-visibility'; npm run test:client:repo
$env:NSF_TEST_FILE_FILTER='real-workspace-replay'; npm run test:client:repo
$env:NSF_REAL_REPLAY_INCLUDE_LONG='1'; $env:NSF_TEST_REAL_REPLAY_SCRIPT_FILTER='pbr-flow-water-full-input'; npm run test:client:real:replay
```

### Implementation recommendation

Recommended first behavior implementation:

1. Extend `CompletionCoordinatorAction` with explicit in-flight stale /
   detached cleanup actions.
2. Introduce per-key state for pending, visible in-flight, latest superseding
   request and generation.
3. Preserve Phase B before-LSP coalescing exactly.
4. When a newer same-key identifier-prefix request arrives during visible
   in-flight execution, mark the old visible generation stale and retain only
   the latest superseding request.
5. Resolve stale visible provider promises neutral without waiting for their
   detached `next(...)` cleanup to publish.
6. Observe detached `next(...)` promises and record cleanup / error metrics.
7. Keep all bypass classes out of this state machine.

Do not implement strict single-underlying-LSP serialization as the first
behavior change unless new evidence proves detached LSP cleanup, rather than
visible provider overlap, is still the dominant bottleneck after this step.

## Phase H: Comprehensive Solution Design

Status: implemented in the client coordinator on 2026-05-14; validation in progress.

Behavior change: yes. User confirmed implementation direction before code changes.

### Problem model

The remaining completion latency has two different surfaces:

- Visible provider latency: how long VS Code waits for a completion provider
  promise before it can update or settle the suggest UI.
- Underlying LSP cleanup latency: how long an already-started
  `LanguageClient.sendRequest(...)` promise continues before it resolves, even
  if its result is no longer useful.

Phase F shows the visible provider path is the product-facing problem. Server
completion handler time remains small, and the latest request usually starts
after only the short coordinator debounce window. The long tail lives inside
overlapping `sendRequest(...)` promises.

Therefore the comprehensive solution should optimize visible latest-result
correctness first, and only then reduce underlying cleanup if evidence still
requires it.

### Design goals

- Latest identifier-prefix auto-trigger request for a safe key should be the
  only request allowed to publish real completion results.
- Superseded identifier-prefix auto-trigger provider promises should settle
  neutral quickly instead of holding the suggest UI hostage.
- Already-started `next(...)` promises must be observed and cleaned up without
  blocking the latest visible request.
- Explicit invoke, `.` member completion, member-prefix completion, retrigger
  and unknown requests must keep current bypass behavior.
- Server completion candidates, sorting, filtering and documentation must not
  change.
- The coordinator should remain the single owner of client-side completion
  request scheduling.
- Replay metrics must distinguish visible promise settlement from detached LSP
  cleanup.

### Non-goals

- Do not optimize server semantic completion based on these replay numbers.
- Do not coalesce signature help.
- Do not coalesce explicit invoke.
- Do not coalesce `.` member completion or member-prefix completion.
- Do not bypass the VS Code / LanguageClient provider stack by constructing a
  separate direct LSP completion request path.
- Do not add a long-lived feature flag or compatibility layer for old/new
  completion behavior.

### Key constraints

- Middleware receives `next(document, position, context, token)` from VS Code,
  but does not own the cancellation source for an already-started `next(...)`.
- The request token can tell us that VS Code cancelled a request, but the
  coordinator cannot reliably cancel an old `sendRequest(...)` once it is
  already in the LanguageClient path.
- Returning neutral for a stale provider promise is safe only for
  auto-trigger / quick-suggest classes where a newer same-key request exists.
- Returning neutral for explicit invoke or member completion would be a visible
  behavior regression and is outside scope.
- Same key alone is not sufficient for every edit pattern; prefix progression
  must remain monotonic or duplicate. Backspace / shrink / cursor relocation
  should not be folded into the same stale-drop path without evidence.

### State machine

Per coordinator key, maintain a small explicit state object:

- `idle`: no pending or visible eligible request.
- `pendingBeforeLsp`: a debounced latest-only request that has not called
  `next(...)` yet.
- `visibleInFlight`: one visible provider promise has started `next(...)` and
  may still publish if its generation remains current.
- `supersededVisible`: a newer same-key request has invalidated an older
  visible in-flight generation; the older visible provider promise should settle
  neutral.
- `detachedCleanup`: an already-started `next(...)` promise continues in the
  background after its visible provider promise was settled neutral.

Each eligible request gets a monotonically increasing generation:

- Only the current generation may resolve with real completion results.
- Older generations resolve neutral.
- Detached cleanup generations may update metrics only.
- Per-key state cleanup must be generation-aware so a late old promise cannot
  clear the current key state.

### Request classification

Continue using the current conservative classifier:

- Eligible: `identifierPrefixAutoTrigger` with a non-empty coordinator key.
- Bypass: explicit invoke, member, retrigger and unknown.

Additional guard for in-flight supersession:

- A newer request may supersede an older same-key eligible request only when
  the prefix length is the same or longer.
- If prefix length shrinks, the coordinator should not treat it as the same
  forward auto-trigger burst; prefer bypass or a fresh generation after direct
  evidence.
- If identifier start changes, it is a different key.

### Recommended first implementation

Use early visible stale neutralization, not strict underlying-LSP
serialization.

1. Keep Phase B `pendingBeforeLsp` behavior unchanged.
2. When the debounce timer fires, start `execute()` but do not simply
   `await execute()` inside the only promise path.
3. Store an in-flight entry with:
   - key
   - generation
   - prefix length
   - visible `resolve` / `reject`
   - settled flag
   - detached cleanup promise bookkeeping
4. If a newer same-key eligible request arrives while an older visible
   generation is in flight:
   - mark the older generation stale
   - settle the older visible promise neutral if it has not already settled
   - attach cleanup handlers to the already-started `execute()` promise
   - retain the newer request through normal short debounce
5. If a generation completes and is still current, resolve with real result.
6. If a generation completes after becoming stale, ignore the result and record
   detached cleanup / stale metrics.
7. If a generation rejects while still current, propagate the error as today.
8. If a generation rejects after becoming stale/detached, swallow it only after
   recording detached cleanup error metrics.

This keeps latest visible work from waiting behind old stale visible promises,
while avoiding the head-of-line risk of strict serialization.

### Alternative options reviewed

Option A: Strict single underlying LSP in-flight per key.

- Benefit: reduces underlying LSP overlap most aggressively.
- Problem: without reliable cancellation, the latest request can be blocked
  behind a stale `sendRequest(...)` promise for 0.9s-1.7s.
- Verdict: reject as first implementation.

Option B: Early visible neutralization with detached cleanup.

- Benefit: targets the product-facing provider promise latency while avoiding
  head-of-line blocking.
- Problem: old underlying `sendRequest(...)` promises can still run in the
  background and consume LanguageClient capacity.
- Verdict: recommended first implementation.

Option C: Direct client LSP request path with owned cancellation.

- Benefit: could give the coordinator stronger cancellation control.
- Problem: duplicates LanguageClient provider conversion / lifecycle behavior
  and creates a second completion transport path.
- Verdict: reject unless the current provider stack proves impossible to tame.

Option D: Server-side latest-only for completion.

- Benefit: server could drop stale P1 completion work.
- Problem: server work is already low-millisecond; the measured wait is mostly
  before or around LanguageClient promise settlement.
- Verdict: not the next optimization.

Option E: Wider debounce window.

- Benefit: fewer requests.
- Problem: directly delays visible latest completion and does not address
  in-flight overlap once `next(...)` starts.
- Verdict: not recommended.

### Data needed during implementation

Add metrics that separate visible and cleanup paths:

- visible stale neutralized count
- visible stale neutralized latency
- detached cleanup count
- detached cleanup duration
- detached cleanup error count
- latest real result count
- latest real result latency
- in-flight supersession count
- max active detached cleanup count

Replay should show:

- visible provider request sequence and action
- detached cleanup sequence / generation where possible
- whether UI quiet improved while detached cleanup still existed
- whether latest expected labels remained present

### Correctness tests to add

Coordinator-level focused tests:

- pending-before-LSP replacement still resolves older pending request neutral.
- in-flight same-key newer prefix resolves older visible promise neutral.
- newer different-key completion cancels older visible auto-trigger work without
  coalescing the new request.
- stale in-flight `execute()` result is ignored.
- stale in-flight `execute()` rejection is recorded and not unhandled.
- latest generation resolves real result.
- explicit invoke bypasses even when same key has in-flight auto-trigger.
- member completion bypasses even when same key-like prefix exists.
- prefix shrink is not treated as safe forward supersession.
- dispose/reset settles pending visible promises and clears detached state.

Integration tests:

- completion auto-trigger still issues and resolves latest prefix results.
- completion client metrics expose new counts.
- member completion matrix remains unchanged.
- real-workspace replay summary reports visible stale and detached cleanup
  separately.

### Performance acceptance

For `pbr-flow-water-full-input`:

- 0 anomalies.
- Provider verification P95 remains below 100ms.
- Direct server completion remains near current low-millisecond baseline.
- UI queue quiet P50/P95 improves materially compared with Phase F, or the
  report clearly shows detached cleanup rather than visible provider waiting.
- Latest executed visible provider overlap decreases.
- No increase in member completion latency.
- Detached cleanup errors are zero.

Suggested numeric target for the first behavior pass:

- Reduce completion `uiQueueQuiet` P50 from the current ~750-900ms range toward
  sub-300ms.
- Reduce slowest identifier-prefix visible provider wait by at least 40%.
- Keep provider verification P95 under 100ms and direct server P95 under 60ms.

These are not product promises; they are acceptance signals for the replay
optimization branch.

### Rollback and failure handling

- Do not add a feature flag.
- Keep the change localized to `client_completion_request_coordinator.ts` and
  existing metrics/report consumers.
- If replay shows latest results missing, member completion regression, or
  unhandled detached errors, revert the in-flight behavior while keeping
  attribution-only metrics if useful.
- If UI quiet does not improve but detached cleanup remains high, reassess
  whether LanguageClient-level cancellation or a deeper provider integration is
  justified.

### Implementation sequence

Recommended order:

1. Add coordinator unit-style tests around generation and detached cleanup
   behavior.
2. Refactor coordinator internals to explicit per-key state helpers without
   changing behavior.
3. Add new metrics/actions and report plumbing.
4. Implement early visible stale neutralization for eligible same-key
   identifier-prefix auto-trigger requests.
5. Run repo completion tests.
6. Run `pbr-flow-water-full-input` real replay and compare Phase F vs new
   report.
7. Only if detached cleanup remains the main measured bottleneck, review a
   second-stage cancellation/serialization design.

## Phase I: Implementation Notes

Status: implemented and validated on 2026-05-15.

Implemented behavior:

- `client_completion_request_coordinator.ts` now owns an explicit per-key
  generation state with pending-before-LSP and visible-in-flight maps.
- Safe forward prefix requests still coalesce before LSP starts.
- Safe forward prefix requests that arrive while an older same-key visible
  request is in flight resolve the older visible provider promise neutral with
  action `staleResolvedWhileInFlight`.
- New completion requests with a different coordinator key also stale-resolve
  older visible identifier auto-trigger requests. This does not coalesce or
  neutralize the new explicit/member request; it only cancels stale old
  auto-trigger work that would otherwise block the shared completion transport.
- Same-key prefix shrink remains protected and does not stale-resolve the older
  visible generation.
- Eligible visible requests run `next(...)` with a coordinator-owned
  cancellation token linked to the original VS Code token. Superseding a stale
  visible generation cancels that owned token, giving LanguageClient/server a
  real cancellation signal without adding a separate direct LSP path.
- The already-started `next(...)` promise is observed by attached completion
  and error handlers; stale success increments detached cleanup metrics and
  stale errors increment detached error metrics instead of surfacing as
  unhandled rejections.
- Prefix shrink is not considered safe supersession. The coordinator keeps the
  older visible generation publishable and lets the shorter-prefix request run
  as a separate generation.
- Explicit invoke, `.` member completion, retrigger and unknown requests remain
  bypassed.

Metrics now exposed by the coordinator:

- `completionCoordinatorSupersededInFlightCount`
- `completionCoordinatorNeutralResolvedWhileInFlightCount`
- `completionCoordinatorLatestRetainedWhileInFlightCount`
- `completionCoordinatorDetachedLspCount`
- `completionCoordinatorDetachedLspErrorCount`
- `completionCoordinatorCancelledWhileInFlightCount`

Implementation choices rejected after measurement:

- Direct auto-trigger `client.sendRequest(textDocument/completion)` using
  LanguageClient converters was tested and removed. It increased replay
  `uiQueueQuiet` and provider verification tail latency, so the final solution
  keeps the normal `next(...)` provider path.
- A 40ms debounce window was tested and removed. It reduced LSP starts, but the
  extra visible wait outweighed the benefit in the full-input replay; the final
  runtime remains at 25ms.

Report plumbing:

- `latencySummary.completion.coordinatorActual` now counts
  `staleResolvedWhileInFlightRequests` and
  `cancelledWhileInFlightRequests`.
- Dropped sequence accounting treats `staleResolvedWhileInFlight` as a dropped
  visible provider result while still counting it as LSP-started work.
- `uiExecutedAttribution` remains focused on request sequences whose final
  visible action is `executed`.

Focused tests added:

- pending-before-LSP safe forward coalescing.
- in-flight same-key forward-prefix neutralization and detached cleanup.
- stale detached rejection recording without propagating.
- prefix shrink not being treated as safe stale supersession.
- explicit invoke and member completion bypass.

Final validation:

- `npm run compile`: passed.
- `NSF_TEST_FILE_FILTER=completion-request-coordinator npm run test:client:repo`:
  passed, 6 tests.
- `NSF_TEST_FILE_FILTER=completion-auto-trigger npm run test:client:repo`:
  passed.
- `NSF_TEST_FILE_FILTER=completion-client-metrics npm run test:client:repo`:
  passed.
- `NSF_TEST_FILE_FILTER=member-completion npm run test:client:repo`: passed
  with the existing real-workspace member test pending.
- `NSF_TEST_FILE_FILTER=interactive-visibility npm run test:client:repo`:
  passed. One earlier run had a hover debug wait timeout and passed on rerun.
- `NSF_TEST_FILE_FILTER=real-workspace-replay npm run test:client:repo`:
  passed.
- `NSF_REAL_REPLAY_INCLUDE_LONG=1`
  `NSF_TEST_REAL_REPLAY_SCRIPT_FILTER=pbr-flow-water-full-input`
  `npm run test:client:real:replay`: passed with 0 anomalies.

Final `pbr-flow-water-full-input` completion data:

- Coordinator actual: 97 received, 38 LSP-started, 59 coalesced-before-LSP,
  10 stale-resolved-while-in-flight, 0 cancelled, 3 explicit bypass, 7 member
  bypass, 8 unknown bypass.
- Provider verification: avg 32.9ms, P95/max 49ms.
- Direct server completion: avg 9.9ms, P95/max 52ms.
- UI queue quiet: avg 709.9ms, P50 603ms, P95/max 1605ms.
- UI executed LSP attribution: avg 773.2ms, P50 676ms, P95/max 1592ms.
- Server queue/context/handler remain low: queue P95 1.4ms, context P95 0ms,
  handler P95 2.4ms.

Conclusion:

- The coordinator now removes stale visible auto-trigger results and cancels
  stale underlying `next(...)` tokens without changing server semantics.
- The remaining long tail is still client/LanguageClient residual time, not
  server completion work. Direct request and wider debounce experiments were
  measured and rejected, so the final patch keeps the clean coordinator
  boundary instead of adding a second completion transport path.

## Phase J: Per-Request Server Attribution

Status: implemented and validated on 2026-05-14.

Reason:

- Phase I still used the server's global `lastCompletionDebug` when attributing
  the latest visible UI/native completion request.
- In real bursts, stale or bypassed provider work can finish after the latest
  visible request starts, so global last-debug attribution can point at an
  older position or request sequence.
- The next optimization decision needed exact client/server correlation before
  changing public completion trigger behavior.

Implemented attribution-only changes:

- Completion provider timing now creates a `completionDebugRequestId` for each
  client provider draft.
- Provider timing now uses async-local context when running `next(...)`, so
  converter and `sendRequest` hooks attribute metrics and debug ids to the
  owning provider draft instead of relying on a global active stack during
  overlapping completion promises.
- The existing LanguageClient completion request carries that id as
  `nsfDebugRequestId` in params. This is a debug-only field and does not affect
  completion candidates, ordering, filtering or trigger decisions.
- The C++ completion handler records `nsfDebugRequestId` in
  `CompletionDebugSnapshot` and keeps a bounded recent debug history returned
  by `nsf/_getLastCompletionDebug`.
- Replay UI coverage stores `completionDebugHistory`; report attribution first
  matches `completionDebugRequestId` to `nsfDebugRequestId`.
- If a client request id exists but server history does not contain it, the
  report records `serverDebugRequestIdUnmatchedCount` and does not fall back to
  global last debug. Fallback remains only for legacy captures without ids.

Validation:

- `npm run compile`: passed.
- `cmake --build .\server_cpp\build`: passed.
- `NSF_TEST_FILE_FILTER=real-workspace-replay-runner npm run test:client:repo`:
  passed, 4 tests.
- `NSF_TEST_FILE_FILTER=completion-client-metrics npm run test:client:repo`:
  passed.
- `NSF_TEST_FILE_FILTER=completion-auto-trigger npm run test:client:repo`:
  passed.
- `NSF_TEST_FILE_FILTER=completion-request-coordinator npm run test:client:repo`:
  passed, 6 tests.
- `NSF_TEST_FILE_FILTER=member-completion npm run test:client:repo`: passed
  with the existing real-workspace member test pending.
- `NSF_TEST_FILE_FILTER=real-workspace-replay npm run test:client:repo`:
  passed, 12 tests.
- `NSF_REAL_REPLAY_INCLUDE_LONG=1`
  `NSF_TEST_REAL_REPLAY_SCRIPT_FILTER=pbr-flow-water-full-input`
  `npm run test:client:real:replay`: passed with 0 anomalies.

Latest `pbr-flow-water-full-input` completion data after async-local provider
timing:

- Completion capture: avg 763.3ms, P50 715ms, P95/max 1467ms.
- Provider verification: avg 31.1ms, P50 33ms, P95/max 55ms.
- Client provider total: avg 25.8ms, P50 28ms, P95/max 53ms.
- Client provider LSP request: avg 8.2ms, P50 3ms, P95/max 28ms.
- Direct server completion: avg 6.7ms, P50 3ms, P95/max 31ms.
- UI queue quiet: avg 585.5ms, P50 544ms, P95/max 1283ms.
- Coordinator actual: 108 received, 41 LSP-started,
  67 coalesced-before-LSP, 12 stale-resolved-while-in-flight,
  0 cancelled-before-LSP, 0 cancelled-while-in-flight,
  5 explicit bypass, 7 member bypass, 7 unknown bypass.
- UI executed attribution: 10 latest executed requests,
  `serverDebugRequestIdMatchedCount` 6,
  `serverDebugRequestIdUnmatchedCount` 4,
  `serverDebugRequestIdFallbackCount` 0.
- For the 6 server-matched latest executed requests, server queue/context
  P95 are 1.5ms / 0ms and server handler P95/max is 1.8ms;
  client/transport residual remains high with P50 642.9ms and P95/max
  1288.4ms.

Conclusion:

- The older global last-debug attribution was indeed unsafe; several slow
  probes previously appeared to match a server request only because an older
  debug snapshot was reused.
- After strict id matching, the server is still not the bottleneck. Some slow
  latest provider promises are server-matched but delayed after the server
  response; others are not observed in server debug history at all and must be
  treated as LanguageClient / VS Code provider-layer wait, cancellation or
  transport scheduling.
- The remaining product optimization likely sits above the server request
  handler: trigger strategy, provider overlap, or LanguageClient request
  serialization. Changing advertised completion trigger characters or treating
  explicit invokes differently is public completion behavior and remains a
  stop-and-confirm boundary.

## Stop-And-Confirm Boundaries

Stop and confirm before:

- Implementing Phase B or C product behavior changes.
- Returning neutral results for any completion request class.
- Coalescing `.` member completion.
- Coalescing explicit user-invoked completion.
- Changing completion candidates, item ordering, trigger semantics or signature
  help behavior.

Phase A report-only simulation can proceed without confirmation because it does
not change runtime behavior.

## Historical Phase A Next Thread Prompt

```text
Read README.md, docs/architecture.md, docs/resources.md, docs/testing.md,
docs/human-ai/2026-05-14-pbr-flow-water-real-replay-performance-plan.md, and
docs/human-ai/2026-05-14-completion-auto-trigger-coordinator-plan.md.

Execute Phase A only: add report-only coalescing simulation for completion
auto-trigger bursts. Do not change runtime completion behavior. Validate with
compile, real-workspace-replay repo tests, and the pbr-flow-water full-input
real replay. Summarize simulated retained/dropped request counts for the
slowest probes.
```

## Working Tree Notes

The repository is dirty with many unrelated uncommitted changes. Do not revert
unrelated work. Current Phase I changes are scoped to the client completion
coordinator, replay report accounting, focused tests and docs.
