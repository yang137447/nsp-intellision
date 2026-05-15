# pbr_flow_water Real Replay Performance Baseline And Upgrade Plan

## Purpose

This note stores the first full-input real replay performance baseline for
`pbr_flow_water_nodes.hlsl` and turns it into the next upgrade plan. It is a
human/AI handoff document, not a promoted current-fact document.

Primary generated report:

- `out/test/perf-reports/real-replay/rw-pbr-flow-water-full-input.json`

Replay script:

- `src/test/replay/scripts/rw-pbr-flow-water-full-input.json`

Target real-workspace file:

- `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\sfx\nodes\pbr_flow_water_nodes.hlsl`

Expected active unit:

- `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\sfx\pbr_flow_water.nsf`

## Baseline Command

```powershell
$env:NSF_REAL_REPLAY_INCLUDE_LONG = "1"
$env:NSF_TEST_REAL_REPLAY_SCRIPT_FILTER = "pbr-flow-water-full-input"
npm run test:client:real:replay
```

The run completed successfully before this note was written.

## Baseline Summary

| Metric | Value |
| --- | ---: |
| Full replay duration | 145.51s |
| Source size | 545 lines / 27,411 chars |
| Checkpoints | 109 |
| Probes | 29 |
| Probe samples | 2,900 |
| Samples per probe | 100 |
| Final text matches source | yes |
| Anomalies | 0 |
| Completion UI-trigger probes | 17 |
| Signature UI-trigger probes | 11 |
| Final diagnostics | 11 diagnostics / 4 errors |
| Final diagnostics capture | 1,767ms |
| Runtime ready observed | yes |
| Touches needed for final diagnostics | 0 |

## Completion And Signature Timing

| Path | Avg | P50 | P95 | Max |
| --- | ---: | ---: | ---: | ---: |
| Completion capture through VS Code path | 857.2ms | 828ms | 1,548ms | 1,805ms |
| Direct server completion inside probe | 8.4ms | 4ms | 30ms | 30ms |
| Signature capture through VS Code path | 333.8ms | 246ms | 495ms | 1,028ms |

Important conclusion:

- The direct server completion path is fast in this baseline.
- The observed completion delay is concentrated in the VS Code/provider/UI
  capture chain, or in measurement sequencing around that chain.
- Do not start by optimizing server semantic collection unless a finer-grained
  trace later proves the server path is the bottleneck.

## Slowest Completion Probes

| Probe | Category | Capture | UI command | Direct server | Items |
| --- | --- | ---: | ---: | ---: | ---: |
| local variable completion after foam_structure prefix | `completion.symbol.local-variable` | 1,805ms | 138ms | 5ms | 416 |
| texture method completion after distort texture dot | `completion.member.texture-sample` | 1,548ms | 124ms | 3ms | 13 |
| preprocessor macro completion after USE_MAIN prefix | `completion.preprocessor.macro` | 1,112ms | 136ms | 4ms | 6 |
| uniform completion after u_foam prefix | `completion.symbol.uniform` | 1,070ms | 122ms | 5ms | 43 |
| workspace function completion after GetLight prefix | `completion.function.workspace-include` | 1,056ms | 137ms | 3ms | 5 |
| include function completion after Bump prefix | `completion.function.include` | 1,032ms | 124ms | 4ms | 11 |
| texture method completion after NormalMap dot | `completion.member.texture-samplebias` | 1,009ms | 121ms | 2ms | 13 |
| type completion after PixelMaterial prefix | `completion.symbol.type` | 987ms | 147ms | 4ms | 2 |

## Current Interpretation

The replay now covers a realistic full-file typing path instead of bulk-filling
the final text and then asking completions. Completion probes type to the exact
trigger point, use native editor typing for completion trigger text, run the
VS Code suggest command, and separately record direct server completion timing.

This makes the report useful for user-perceived latency investigation, but it
also means the current "completion capture" number is an aggregate. It may
include native typing, VS Code auto-trigger, explicit suggest command,
`vscode.executeCompletionItemProvider`, extension-provider sync wait,
LSP roundtrip, item conversion, and test harness waiting.

The biggest immediate risk is mis-attribution. The first upgrade should split
that aggregate into smaller timings before changing runtime behavior.

## Upgrade Plan

## Execution Progress

Phase 1/2 instrumentation has been implemented in the current working tree:

- Client completion and signature providers now expose per-request provider
  timing through `nsf._getInternalStatus`, including code-to-protocol
  conversion, LSP request, protocol-to-code conversion, provider total, request
  sequence, document version, dirty state and result counts.
- Real replay completion and signature captures now persist separate timings for
  trigger typing, UI command, `vscode.execute*Provider`, client provider timing,
  direct server completion and request counter deltas.
- Real replay report writing now adds a top-level `latencySummary` with
  completion / signature P50, P95, max, slowest probes and duplicated request
  path counts, so the next pass can start from attribution instead of manually
  reading raw probe JSON.
- Phase 2 harness separation is implemented: completion / signature captures
  now record UI/native trigger coverage under `uiCoverage`, close the UI widget,
  wait for provider request counters to go quiet, and then run
  `providerVerification` as a separate `vscode.execute*Provider` measurement.
  `latencySummary.duplicatedRequestProbeCount` now treats separated probes as
  duplicated only when the UI queue quiet wait timed out.
- Auto-trigger burst observation is now part of the same report path. Client
  provider timing exposes recent completion / signature request sequences with
  trigger kind, trigger character, line / character, prefix length, request
  start / completion time and result counts. Replay `uiCoverage` slices that
  sequence per probe and `latencySummary` reports UI request burst counts.
- The old capture aggregate fields remain in the report for comparison.
- Repo replay runner coverage asserts that completion capture reports include
  provider timing and request counter fields.

Latest `pbr-flow-water-full-input` replay after adding `latencySummary`:

- Before Phase 2 separation, completion probes were 17 / 17 duplicated request
  paths and signature probes were 11 / 11 duplicated request paths.
- After Phase 2 separation, completion duplicated request paths are 0 / 17 and
  signature duplicated request paths are 0 / 11.
- Completion provider verification: avg 13.6ms, P50 10ms, P95/max 48ms.
- Completion client LSP request during provider verification: avg 6.9ms,
  P50 4ms, P95/max 26ms.
- Direct server completion: avg 8.9ms, P50 3ms, P95/max 49ms.
- Completion UI queue quiet wait: avg 641.8ms, P50 586ms, P95/max 1447ms.
- Signature provider verification: avg 20.2ms, P50 17ms, P95/max 72ms.
- Signature client LSP request during provider verification: avg 14.6ms,
  P50 16ms, P95/max 25ms.
- Signature UI queue quiet wait: avg 266.5ms, P50 185ms, P95/max 832ms.
- Slowest completion probe after separation:
  `local variable completion after foam_structure prefix`, capture 1592ms,
  provider verification 15ms, client LSP request 4ms, direct server 4ms,
  UI coverage requests 10, provider-verification requests 1.

Latest replay after adding auto-trigger burst sequence capture:

- Completion provider verification: avg 19.5ms, P50 13ms, P95/max 58ms.
- Completion client LSP request during provider verification: avg 8.1ms,
  P50 4ms, P95/max 39ms.
- Direct server completion: avg 9.1ms, P50 4ms, P95/max 45ms.
- Completion duplicated request paths remain 0 / 17; UI queue quiet timeouts
  remain 0 / 17.
- Completion UI request burst count: avg 6.3 requests, P50 6, P95/max 15.
- Completion UI queue quiet wait: avg 796.9ms, P50 794ms, P95/max 1837ms.
- Slowest completion probe remains
  `local variable completion after foam_structure prefix`; its UI coverage
  sequence contains 15 provider requests. The sequence shows prefix lengths
  advancing from 6 through 14, with several duplicate prefix-length 14 requests
  completing around 1967-1968ms. Provider verification for the same probe is
  still fast after the UI queue is quiet.

Current attribution:

- The server direct request path is still fast.
- Provider verification is now fast after separating it from UI/native trigger
  coverage.
- The remaining 1s-level capture aggregate is almost entirely the UI coverage
  queue quiet wait after native/editor-triggered requests.
- The burst sequence suggests the next product-facing optimization should
  inspect why native typing emits multiple stale completion requests for the
  same final prefix, not the server semantic completion query itself.
- The next product-facing decision, if any, should focus on reducing or
  coalescing auto-trigger request bursts in real editing. Do not optimize the
  server semantic completion path based on this replay; the separated provider
  and direct server numbers no longer support that attribution.

### Phase 1: Instrument the completion and signature path

Goal: identify where the 0.8s-1.8s completion capture time is spent.

Tasks:

- Add per-probe timing for native trigger typing, explicit suggest command, and
  `vscode.executeCompletionItemProvider`.
- Add client-side provider timing around completion and signature provider
  entry, document sync wait, LSP request, response conversion, and provider
  return.
- Record request counters before/after each probe so duplicate auto-trigger,
  explicit suggest, and execute-provider requests can be counted.
- Persist these fields into the real replay report and keep the old aggregate
  fields for comparison.

Acceptance:

- The report can answer whether the slow segment is VS Code UI command,
  execute-provider verification, extension-host provider work, sync wait,
  LSP roundtrip, or harness waiting.
- No completion result-set behavior changes in this phase.

### Phase 2: Separate UI visibility from provider verification

Goal: preserve real user-trigger coverage while avoiding one mixed number that
looks like product latency.

Tasks:

- Keep the real suggest UI trigger for probes where user-perceived popup
  behavior matters.
- Record UI command latency separately from provider verification latency.
- Detect whether native typing already triggered a provider request before the
  explicit suggest command.
- Label any intentionally duplicated request path in the report.

Acceptance:

- The report has separate numbers for "suggest UI command completed",
  "provider returned items", and "direct server returned items".
- If duplicate triggering exists, it is visible in the report before any
  product behavior is changed.

### Phase 3: Optimize only the confirmed bottleneck

Goal: reduce real completion latency based on Phase 1/2 evidence.

Possible work, depending on evidence:

- If sync wait dominates, inspect document-version propagation and current-doc
  runtime readiness in the client provider path.
- If extension-host conversion dominates, inspect completion item construction
  and large-list handling in client code.
- If duplicate trigger sequencing dominates only in the harness, adjust replay
  measurement so UI trigger and provider verification are measured separately.
- If server roundtrip unexpectedly becomes slow after finer tracing, optimize
  the shared server query path rather than adding client-side semantic facts.

Acceptance target for the next performance pass:

- Direct server completion remains below 50ms max on this replay.
- Completion capture has a clearly attributed slow segment.
- After the confirmed fix, completion capture P95 should move toward sub-500ms
  without reducing correctness coverage.

Stop-and-confirm boundary:

- Any change that alters completion candidates, trigger semantics, signature
  help contents, diagnostics contents, or user-visible behavior must be
  confirmed before implementation.

### Phase 4: Keep diagnostics as a separate track

The final diagnostics baseline is stable enough for this replay:

- 11 diagnostics / 4 errors.
- Runtime ready observed.
- No document touches needed.
- No replay anomalies.

The remaining diagnostics match the previously deferred HLSL type-compatibility
category. Do not mix that work into completion latency optimization unless the
user explicitly reopens type compatibility behavior.

## Validation Matrix For The Next Pass

Minimum validation after instrumentation-only changes:

```powershell
npm run compile
$env:NSF_TEST_FILE_FILTER = "real-workspace-replay"
npm run test:client:repo
$env:NSF_REAL_REPLAY_INCLUDE_LONG = "1"
$env:NSF_TEST_REAL_REPLAY_SCRIPT_FILTER = "pbr-flow-water-full-input"
npm run test:client:real:replay
```

If C++ server behavior changes:

```powershell
cmake --build .\server_cpp\build
npm run test:client:repo
```

If resources or resource schemas change:

```powershell
npm run json:validate
cmake --build .\server_cpp\build
```

## Next Thread Start

1. Read `README.md`, `docs/architecture.md`, `docs/resources.md`, and
   `docs/testing.md`.
2. Read `docs/human-ai/2026-05-13-pbr-flow-water-diagnostics-handoff.md`.
3. Read this document.
4. Start with Phase 1 instrumentation; do not start with semantic completion
   optimization unless the new timings prove it is needed.

## Working Tree Notes

The repo is already very dirty with many unrelated uncommitted changes. Do not
revert unrelated work. Treat this document as a saved reference for the next
performance-improvement pass.
