# Completion Provider Layer Latency Plan

Status: Phase K implemented and validated; Phase L handfeel-preserving
didChange/input-thread optimization implemented and validated. Identifier
trigger characters remain in scope and must not be removed without a separate
public UX decision.

Date: 2026-05-15.

## Background

The completion auto-trigger coordinator has already reduced identifier-prefix
request fan-out before LSP starts and neutral-resolved stale visible
auto-trigger requests after LSP starts.

The latest strict attribution pass added per-request client/server debug ids:

- client provider timing emits `completionDebugRequestId`
- completion params carry debug-only `nsfDebugRequestId`
- server completion debug history records the id
- replay attribution only uses server timing when the ids match

Latest `pbr-flow-water-full-input` replay result:

- 0 anomalies.
- Completion capture: avg 763.3ms, P50 715ms, P95/max 1467ms.
- Provider verification: avg 31.1ms, P50 33ms, P95/max 55ms.
- Direct server completion: avg 6.7ms, P50 3ms, P95/max 31ms.
- UI queue quiet: avg 585.5ms, P50 544ms, P95/max 1283ms.
- UI latest executed attribution: 10 latest executed requests.
- Server debug id matched: 6.
- Server debug id unmatched: 4.
- Server debug fallback: 0.
- For matched latest requests, server handler P95/max is 1.8ms while
  client/transport residual P95/max is 1288.4ms.

Conclusion from current data:

- The server completion handler is not the bottleneck.
- The remaining long tail is above the server handler: VS Code completion
  provider scheduling, LanguageClient request lifecycle, overlap with explicit
  suggest invocations, cancellation propagation, or replay UI triggering.
- The next change should not add a second completion transport path. The safer
  architecture is to reduce unnecessary provider work at the trigger policy
  level while preserving the single LanguageClient completion path.

## Problem Statement

Current completion capabilities advertise identifier characters as LSP trigger
characters. Together with language-default quick suggestions and replay
`editor.action.triggerSuggest`, a single typed prefix can create overlapping
provider work:

- native trigger-character requests for identifier characters
- quick-suggest / invoke-style requests
- explicit suggest UI requests in replay capture
- stale provider promises whose server work may complete later

The coordinator can drop or neutral-resolve some stale visible requests, but it
cannot prevent VS Code / LanguageClient from creating and scheduling all
provider promises. This leaves a long tail even when the server responds in
low milliseconds.

## Goals

- Keep one completion provider path through LanguageClient and the C++ server.
- Keep server-side completion candidates, ordering, filtering and rendering as
  the single semantic truth.
- Reduce duplicate provider work created by identifier-prefix typing.
- Preserve fast member completion on `.` and syntax-specific triggers such as
  preprocessor/include/attribute contexts.
- Make replay reports distinguish true product latency from replay-trigger
  artifacts.
- Keep behavior changes explicit and measurable before they are accepted.

## Non-Goals

- Do not reintroduce a direct `client.sendRequest(textDocument/completion)` path
  for product auto-trigger completion.
- Do not add feature flags, dual transports, compatibility shims, or fallback
  completion providers.
- Do not coalesce or neutral-resolve explicit user-invoked completion without a
  separate public-behavior decision.
- Do not move language knowledge to the client.
- Do not optimize by changing server completion candidate semantics.

## Stop-And-Confirm Boundaries

Stop and ask before implementing any of the following:

- Changing advertised completion trigger characters.
- Changing language defaults such as `editor.quickSuggestionsDelay`.
- Treating `CompletionTriggerKind.Invoke` as auto-trigger in more cases.
- Coalescing explicit invoke or member completion.
- Removing identifier trigger characters from server capabilities.
- Changing completion candidate order, labels, filtering or commit behavior.

## Architecture Direction

The preferred architecture is a single completion transport with an explicit
separation between the latest user-visible completion request and stale cleanup:

1. Server continues to own semantic completion.
2. Identifier trigger characters stay enabled so variable, function, uniform and
   type-name completion keeps its immediate feel.
3. The client coordinator remains narrow: only safe identifier-prefix
   auto-trigger work is latest-only; explicit invoke and member completion keep
   bypassing it.
4. Reports must separate:
   - latest visible provider return
   - post-latest detached cleanup
   - provider verification
   - direct server timing
5. Optimization should improve stale request exit, cancellation propagation and
   request lifecycle accounting without adding a second completion transport or
   weakening immediate identifier completion.

This keeps the UX goal first: reduce the tail without making completion appear
later for normal variable / function typing.

## Phase K: Measurement Isolation

Purpose: prove how much of the current tail is product latency versus replay
explicit-suggest overlap.

Tasks:

- Add a replay variant or payload mode for completion probes that uses native
  typing plus quick suggestions without calling `editor.action.triggerSuggest`.
- Keep the existing explicit suggest replay path as a separate measurement,
  not as the only UI metric.
- Add report fields:
  - `explicitInvokeOverlapRequests`
  - `latestExecutedHasServerDebug`
  - `serverDebugRequestIdMatchedCount`
  - `serverDebugRequestIdUnmatchedCount`
  - `serverDebugRequestIdFallbackCount`
  - slowest probes grouped by trigger source
- In report summaries, separate:
  - native identifier typing
  - explicit suggest UI command
  - provider verification
  - direct server request

Expected outcome:

- If native quick suggestion is already fast, the main issue is replay capture
  mixing explicit suggest command work into the UI path.
- If native quick suggestion is still slow, continue to Phase L trigger policy
  evaluation.

Validation:

- `npm run compile`
- `NSF_TEST_FILE_FILTER=real-workspace-replay npm run test:client:repo`
- `NSF_REAL_REPLAY_INCLUDE_LONG=1`
  `NSF_TEST_REAL_REPLAY_SCRIPT_FILTER=pbr-flow-water-full-input`
  `npm run test:client:real:replay`

Acceptance:

- 0 anomalies.
- Existing explicit-suggest replay still reports.
- New native-only report has enough data to compare UI queue quiet and latest
  executed LSP timing.

Implementation:

- Added `completionUiMode` to replay typing probe payloads and
  `typeDocumentFromDisk` defaults.
- Added `NSF_REAL_REPLAY_COMPLETION_UI_MODE` for measurement-only override:
  - `nativeOnly`: native typing / quick suggestion coverage only, without
    `editor.action.triggerSuggest`.
  - `explicitSuggest`: keeps the explicit suggest UI command path.
- Added trigger-source attribution on capture reports:
  - `completionCapture.uiCoverageTriggerSource`
  - `completionCapture.uiCoverage.triggerSource`
  - `latencySummary.completion.uiCoverageByTriggerSource`
- Added report fields for explicit replay overlap and strict server debug
  coverage:
  - `explicitInvokeOverlapRequests`
  - `latestExecutedHasServerDebug`
  - `latestExecutedWithServerDebugCount`
  - strict matched / unmatched / fallback server debug counts
- Kept all product completion paths unchanged. The implementation only affects
  replay measurement and report summarization.

Validation results on 2026-05-15:

- `npm run compile`: passed.
- `NSF_TEST_FILE_FILTER=real-workspace-replay-runner npm run test:client:repo`:
  4 passing.
- `NSF_TEST_FILE_FILTER=real-workspace-replay npm run test:client:repo`:
  12 passing.
- Existing explicit-suggest long replay:
  - command: `NSF_REAL_REPLAY_INCLUDE_LONG=1`
    `NSF_TEST_REAL_REPLAY_SCRIPT_FILTER=pbr-flow-water-full-input`
    `npm run test:client:real:replay`
  - report snapshot:
    `out/test/perf-reports/real-replay/rw-pbr-flow-water-full-input-explicitSuggest.json`
  - 0 anomalies, 17 completion probes.
  - UI queue quiet P50 661ms, P95/max 1460ms.
  - UI latest executed LSP P50 687ms, P95/max 1428ms.
  - Provider verification P50 32ms, P95/max 51ms.
  - Direct server completion P50 3ms, P95/max 26ms.
  - Server debug id matched 6, unmatched 4, fallback 0.
  - Matched server handler P95/max 2.4ms; client residual LSP P95/max
    1426.7ms.
  - `explicitInvokeOverlapRequests`: 6.
- Native-only long replay:
  - command: `NSF_REAL_REPLAY_INCLUDE_LONG=1`
    `NSF_TEST_REAL_REPLAY_SCRIPT_FILTER=pbr-flow-water-full-input`
    `NSF_REAL_REPLAY_COMPLETION_UI_MODE=nativeOnly`
    `npm run test:client:real:replay`
  - report snapshot:
    `out/test/perf-reports/real-replay/rw-pbr-flow-water-full-input-nativeOnly.json`
  - 0 anomalies, 17 completion probes.
  - UI queue quiet P50 783ms, P95/max 2246ms.
  - UI latest executed LSP P50 626ms, P95/max 2111ms.
  - Provider verification P50 34ms, P95/max 59ms.
  - Direct server completion P50 3ms, P95/max 32ms.
  - Server debug id matched 10, unmatched 0, fallback 0.
  - Matched server handler P95/max 3.2ms; client residual LSP P95/max 2108ms.
  - `explicitInvokeOverlapRequests`: 0.

Phase K conclusion:

- Native-only did not materially improve the long tail; in this run UI queue
  quiet P95 increased from 1460ms to 2246ms.
- The explicit suggest UI command is not the sole cause of the remaining tail.
  It does create measurable overlap in the explicit-suggest run, but removing
  it does not remove the client-side wait.
- The server remains ruled out as the bottleneck: direct server and provider
  verification stay below 60ms P95, while strict matched server handler timing
  remains below 4ms P95.
- The remaining bottleneck is still above the server handler, most likely in
  VS Code / LanguageClient provider scheduling, request lifecycle, cancellation
  propagation, or overlapping quick-suggest Invoke provider work.
- Continue to Phase L only after explicit confirmation, because changing
  trigger characters is a public completion behavior change.

## Phase L: Handfeel-Preserving Provider Lifecycle Optimization

Purpose: keep identifier completion immediate while reducing or explaining the
client-side tail above the server handler.

Accepted constraints:

- Keep identifier trigger characters:
  - `a-z`
  - `A-Z`
  - `0-9`
  - `_`
- Keep member completion after `.` as a strong trigger.
- Do not change `editor.quickSuggestionsDelay` or language defaults.
- Do not add a direct completion request path.
- Do not coalesce explicit user invoke or member completion.
- Do not change completion candidate semantics, sorting, labels or filtering.

Tasks:

- Add report metrics that separate user-visible return from cleanup:
  - `uiLatestVisibleProviderReturnMs`
  - `uiLatestVisibleProviderExecutionMs`
  - `uiLatestVisibleLspRequestMs`
  - `postLatestVisibleCleanupMs`
  - summary stats under `latencySummary.completion`
  - trigger-source grouped stats under
    `latencySummary.completion.uiCoverageByTriggerSource`
- Continue attributing latest executed requests with strict
  `completionDebugRequestId` / `nsfDebugRequestId` matching.
- Use the new metrics to distinguish:
  - latest request is slow before server response
  - latest request is fast but detached stale cleanup is slow
  - explicit suggest overlap versus native quick-suggest Invoke shape
- Only after the metric split proves the bottleneck, consider a targeted
  coordinator lifecycle change such as earlier stale resource disposal or
  tighter cancellation accounting. Such changes must preserve the current
  visible trigger behavior.

Files likely touched:

- `src/test/replay/real_workspace_replay_report_writer.ts`
- `src/test/suite/client.real-workspace-replay-runner.test.ts`
- `docs/testing.md`
- This plan document

Validation:

- `npm run compile`
- `NSF_TEST_FILE_FILTER=real-workspace-replay npm run test:client:repo`
- `NSF_REAL_REPLAY_INCLUDE_LONG=1`
  `NSF_TEST_REAL_REPLAY_SCRIPT_FILTER=pbr-flow-water-full-input`
  `NSF_REAL_REPLAY_COMPLETION_UI_MODE=nativeOnly`
  `npm run test:client:real:replay`

Acceptance:

- 0 replay anomalies.
- Identifier variable / function completion remains immediate.
- Member completion after `.` remains reliable.
- Provider verification P95 remains under 100ms.
- Direct server completion P95 remains under 60ms.
- Report can tell whether the user-visible latest completion return is slow or
  whether only detached cleanup is slow.
- No increase in completion expected-missing anomalies.

Initial implementation on 2026-05-15:

- Added report-level split for completion UI coverage:
  - `uiLatestVisibleProviderReturnMs`
  - `uiLatestVisibleProviderExecutionMs`
  - `uiLatestVisibleLspRequestMs`
  - `uiLatestVisibleNextWaitMs`
  - `uiLatestVisibleNextExecutionMs`
  - `uiLatestVisibleLspStartDelayMs`
  - `uiLatestVisibleLspCompletionToProviderReturnMs`
  - `uiLatestVisibleActiveProviderOverlapAtStart`
  - `uiLatestVisibleActiveNextOverlapAtStart`
  - `uiLatestVisibleServerHandlerMs`
  - `uiLatestVisibleClientResidualLspMs`
  - `uiLatestVisibleClientToServerReceivedMs`
  - `uiLatestVisibleServerResponseToClientResolveMs`
  - `uiLatestVisibleHasServerDebug`
  - `postLatestVisibleCleanupMs`
- Added aggregate stats:
  - `latencySummary.completion.uiLatestVisibleProviderReturn`
  - `latencySummary.completion.uiLatestVisibleNextWait`
  - `latencySummary.completion.uiLatestVisibleNextExecution`
  - `latencySummary.completion.uiLatestVisibleLspStartDelay`
  - `latencySummary.completion.uiLatestVisibleLspRequest`
  - `latencySummary.completion.uiLatestVisibleLspCompletionToProviderReturn`
  - `latencySummary.completion.uiLatestVisibleActiveProviderOverlapAtStart`
  - `latencySummary.completion.uiLatestVisibleActiveNextOverlapAtStart`
  - `latencySummary.completion.uiLatestVisibleServerHandler`
  - `latencySummary.completion.uiLatestVisibleClientResidualLsp`
  - `latencySummary.completion.uiLatestVisibleClientToServerReceived`
  - `latencySummary.completion.uiLatestVisibleServerResponseToClientResolve`
  - `latencySummary.completion.uiLatestVisibleWithServerDebugCount`
  - `latencySummary.completion.postLatestVisibleCleanup`
  - the same stats inside
    `latencySummary.completion.uiCoverageByTriggerSource`
- Kept runtime completion behavior unchanged:
  - identifier trigger characters remain enabled
  - member completion remains bypassed / immediate
  - explicit invoke remains bypassed
  - no direct completion request path was added
- Added debug-only client/server boundary timestamps:
  - client `sendRequest` start timestamp on completion params
  - server message-received timestamp
  - server request-worker start timestamp
  - server response-write-completed timestamp
  - none of these fields participates in completion behavior
- Added debug-only didChange input-thread attribution:
  - `serverDidChangeCompletedBeforeRequestCount`
  - `serverDidChangeOverlapClientSendCount`
  - `serverDidChangeOverlapClientSendMs`
  - `serverLastDidChangeDurationMs`
  - `serverLastDidChangeEndToRequestReceivedMs`
  - these fields only explain whether a completion request was waiting behind
    server-side `textDocument/didChange` processing before the request was read

Initial metric result:

- Explicit-suggest report:
  - UI queue quiet P50 661ms, P95/max 1460ms.
  - Latest visible provider return P50 776ms, P95/max 1533ms.
  - Post-latest cleanup P50 45ms, P95/max 203ms.
- Native-only live replay with current report writer:
  - report snapshot:
    `out/test/perf-reports/real-replay/rw-pbr-flow-water-full-input-nativeOnly-phaseL.json`
  - 0 anomalies, 17 completion probes.
  - UI queue quiet P50 665ms, P95/max 1564ms.
  - Latest visible provider return P50 653ms, P95/max 1512ms.
  - Post-latest cleanup P50 47ms, P95/max 90ms.
  - Provider verification P50 30ms, P95/max 48ms.
  - Direct server completion P50 3ms, P95/max 33ms.
  - Server debug id matched 10, unmatched 0, fallback 0.
  - Matched server handler P95/max 2.7ms; client residual LSP P95/max
    1418.2ms.
- Native-only live replay with client/server boundary timestamps:
  - report snapshot:
    `out/test/perf-reports/real-replay/rw-pbr-flow-water-full-input-nativeOnly-client-server-boundary.json`
  - 0 anomalies, 17 completion probes.
  - UI queue quiet P50 598ms, P95/max 1298ms.
  - Latest visible provider return P50 570ms, P95/max 1256ms.
  - Latest visible `nextWait` / LSP start delay P50 30ms, P95/max 39ms.
  - Latest visible LSP request P50 577ms, P95/max 1202ms.
  - Client send-start to server received P50 571.6ms, P95/max 1202.3ms.
  - Server handler P50 1.6ms, P95/max 27.7ms.
  - Server response write to client promise resolve P50 0ms, P95/max 0.2ms.
  - Post-latest cleanup P50 42ms, P95/max 103ms.
  - Provider verification P50 29ms, P95/max 46ms.
  - Direct server completion P50 3ms, P95/max 24ms.

Initial Phase L conclusion:

- The remaining native-only tail is not primarily detached cleanup after the
  latest visible request. Cleanup after latest visible return is comparatively
  small.
- The slow part is before the latest visible provider returns, and it aligns
  with the large client / LanguageClient residual already seen in strict server
  attribution.
- The client/server boundary timestamp split shows the dominant delay is before
  the C++ server receives the completion request: client send-start to server
  received has essentially the same P95 as the whole latest visible LSP
  request. Once the server receives the request, handler time is small, and
  server response write to client promise resolve is near zero.
- Next optimization should focus on why LanguageClient delays sending the latest
  visible completion request to the server after middleware calls `sendRequest`,
  especially document sync progression, JSON-RPC writer queueing, cancellation
  interaction, extension-host scheduling and overlapping provider requests.

Follow-up implementation on 2026-05-15:

- Added server didChange input-thread overlap attribution to completion debug
  history and replay summary. This is designed to separate two cases that both
  previously looked like `clientSendStarted -> serverReceived` delay:
  - the request is still queued/writing on the client/JSON-RPC side
  - the request has been written behind earlier `didChange` notifications, but
    the server input thread is still applying those document changes and has
    not read the completion message yet
- Added summary stats:
  - `latencySummary.completion.uiLatestVisibleServerDidChangeOverlap`
  - `latencySummary.completion.uiLatestVisibleServerLastDidChangeDuration`
  - `latencySummary.completion.uiLatestVisibleServerLastDidChangeGap`
  - same fields under `uiCoverageByTriggerSource`
  - same server attribution under
    `latencySummary.completion.uiExecutedAttribution`
- Runtime completion behavior remains unchanged. Identifier trigger characters,
  `.` member completion, explicit invoke bypass, candidate generation, sorting
  and filtering are untouched.

Root-cause confirmation from the first native-only replay after that
attribution:

- `uiLatestVisibleClientToServerReceived` P95/max: 1636.7ms.
- `uiLatestVisibleServerDidChangeOverlap` P95/max: 1635.9ms.
- `uiLatestVisibleServerHandler` P95/max: 28.7ms.
- `uiLatestVisibleServerResponseToClientResolve` P95/max: 0.5ms.
- `postLatestVisibleCleanup` P95/max: 50ms.

Conclusion:

- The remaining long tail is almost entirely server input-thread time spent
  applying earlier `textDocument/didChange` notifications before the completion
  request is read.
- The issue is not server completion handler time, response delivery, detached
  cleanup, or replay explicit suggest overlap.

Accepted optimization:

- Keep didChange responsible for applying the latest document text and updating
  the `DocumentRuntime` key.
- Stop rebuilding current-doc semantic snapshots synchronously from didChange.
- Keep `didOpen` and analysis-context refresh prewarm behavior.
- Let completion / hover / signature help build or promote the latest
  current-doc semantic snapshot on demand through the existing
  `getOrBuildInteractiveSnapshot` path.
- Move local structural snapshot publication for didChange to the existing fast
  diagnostics worker. Fast diagnostics is already per-URI latest-only, so rapid
  typing keeps only the newest structural job instead of synchronously scanning
  every intermediate version on the input thread.
- This preserves the single semantic source and diagnostics publish layer while
  avoiding rebuilds for every intermediate character version during rapid
  typing. The first interactive request after edits still resolves through the
  existing server semantic layer, but only for the latest requested document
  state.

Rollback:

- If any lifecycle change makes identifier completion visibly delayed or
  missing, revert that lifecycle change and keep the measurement split.
- Do not compensate by adding a direct request path or a second provider.

Implemented continuation on 2026-05-15:

- Removed didChange synchronous current-doc semantic prewarm. `didOpen` and
  analysis-context refresh still prewarm; didChange only updates document text,
  runtime keys and visibility state.
- Removed didChange synchronous local structural snapshot rebuild from the
  server input thread.
- Kept local structural runtime coverage by letting fast diagnostics jobs always
  build/store the latest local structural snapshot asynchronously. If visible
  fast diagnostics publishing is disabled, the worker stores the runtime
  snapshot without publishing `textDocument/publishDiagnostics`.
- Fixed diagnostics scheduler wakeup so a newly enqueued earlier fast job can
  wake a scheduler that was waiting for a later full job.
- Updated runtime tests to wait for the asynchronous local structural snapshot
  instead of assuming it is ready when `applyEdit(...)` returns.

Native-only long replay result after the continuation:

- Report:
  `out/test/perf-reports/real-replay/rw-pbr-flow-water-full-input.json`.
- 0 anomalies, 17 completion probes.
- Completion capture P50 274ms, P95/max 371ms.
- UI queue quiet P50 233ms, P95/max 318ms.
- Latest visible provider return P50 91ms, P95/max 109ms.
- Latest visible LSP request P50 3ms, P95/max 9ms.
- Client send-start to server received P50 2.1ms, P95/max 3.8ms.
- Server didChange overlap P50 0ms, P95/max 2.4ms.
- Server last didChange duration P50 2.5ms, P95/max 4.1ms.
- Server handler P50 2ms, P95/max 6.8ms.
- Server response write to client resolve P50/P95/max 0ms.
- Post-latest cleanup P50 193ms, P95/max 226ms.
- Provider verification P50 30ms, P95/max 55ms.
- Direct server completion P50 3ms, P95/max 8ms.
- Coordinator actual: 70 received requests, 31 executed LSP requests, 39
  coalesced before LSP, 3 stale resolved while in-flight.

Continuation conclusion:

- The previous root cause is removed: `clientSend -> serverReceived` P95/max
  fell from roughly 1636.7ms to 3.8ms, and server didChange overlap P95/max fell
  from roughly 1635.9ms to 2.4ms.
- Identifier trigger characters, member completion, explicit invoke bypass,
  completion candidate generation, ordering and filtering remain unchanged.
- Remaining user-visible quiet time is now mostly completion-provider lifecycle
  and post-latest cleanup accounting, not server input-thread didChange
  blocking.

Validation update on 2026-05-15:

- `npm run gate:d3` passed end to end.
- The gate covered json validation, TypeScript compile, clean C++ configure /
  build, hover smoke, client all tests, real workspace smoke and real replay.

Tail audit on 2026-05-15:

- The latest `rw-pbr-flow-water-full-input.json` report shows the remaining
  completion tail is no longer server input-thread blocking:
  - latest visible provider return P95/max: 109ms
  - latest visible LSP request P95/max: 9ms
  - server didChange overlap P95/max: 2.4ms
  - server handler P95/max: 6.8ms
- The apparent `postLatestVisibleCleanup` P95/max of 226ms is dominated by the
  replay queue-quiet guard. For every visible completion probe in that report,
  the last observed provider request completed at the same time as the latest
  visible provider return; the remaining delay is the measurement quiet window
  plus polling slack, not additional provider work.
- Report attribution was tightened to keep the old
  `postLatestVisibleCleanup` total while adding:
  - `postLatestVisibleProviderActivity`
  - `postLatestVisibleQuietGuard`
  - `uiQueueQuietGuard`
- Runtime completion behavior remains unchanged. Identifier trigger
  characters, member completion, explicit invoke bypass and server candidate
  semantics are untouched.

## Phase M: Coordinator Contract Review

Purpose: after Phase L metric split and any accepted lifecycle changes, remove
any coordinator complexity that is no longer justified, or tighten the contract
if identifier typing still creates bursts.

Tasks:

- Re-check whether `isAmbiguousInvokeContinuation` is still needed.
- Re-check cross-key stale cancellation with identifier trigger chars retained.
- Keep explicit invoke and member completion bypassed unless a new public
  behavior decision is approved.
- Update focused coordinator tests to match the accepted lifecycle contract.

Validation:

- `NSF_TEST_FILE_FILTER=completion-request-coordinator npm run test:client:repo`
- Completion replay validations from Phase L.

Acceptance:

- Coordinator behavior is still narrow, understandable and test-covered.
- No old/new dual behavior path remains.

Initial implementation on 2026-05-15:

- Kept `isAmbiguousInvokeContinuation` because native quick suggest still
  arrives as `Invoke` in identifier-prefix bursts; removing it would re-expand
  provider/LSP fan-out while identifier trigger characters remain enabled.
- Tightened the recent-burst source instead: only requests already classified
  as `identifierPrefixAutoTrigger` refresh the recent identifier state.
  Standalone explicit `Invoke` requests still bypass the coordinator and no
  longer seed later coalescing.
- Kept cross-key stale cancellation for older visible auto-trigger requests.
  A new completion key still neutral-resolves stale auto-trigger work so old
  results cannot outlive the user's current completion context.
- Runtime candidate semantics and advertised trigger characters remain
  unchanged.

## Phase N: Finalize Current Facts

If Phase L or M changes accepted runtime behavior, promote the resulting
behavior from human-ai plan to current fact docs:

- `docs/architecture.md`
- `docs/testing.md`
- `docs/client-editor-features.md` if editor-facing defaults changed
- relevant tests and replay script comments

Do not promote speculative measurements as current facts. Only document the
accepted lifecycle contract and validation matrix.

## Recommended Next Action

Continue Phase M coordinator contract review with identifier trigger
characters retained. The current provider-lifecycle tail should be judged from
`postLatestVisibleProviderActivity` and `postLatestVisibleQuietGuard` rather
than the legacy combined `postLatestVisibleCleanup` total.
