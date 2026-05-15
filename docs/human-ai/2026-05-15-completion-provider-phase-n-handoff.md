# Completion Provider Phase N Handoff

## Purpose

This is the handoff note for the completion-provider line after the Phase M
coordinator contract review. Completion behavior is now frozen; the next thread
should start a fresh signature-help tail audit instead of reopening completion
behavior work.

## Completed

- The replay attribution split is in place:
  - `uiLatestVisibleProviderReturnMs`
  - `uiLatestVisibleLspRequestMs`
  - `postLatestVisibleProviderActivityMs`
  - `postLatestVisibleQuietGuardMs`
  - `uiQueueQuietGuardMs`
- The coordinator contract was tightened:
  - identifier-prefix auto-trigger still uses latest-only coalescing
  - explicit `Invoke` no longer seeds the recent-burst state
  - member completion remains bypassed
  - identifier trigger characters remain unchanged
- Validation passed:
  - `npm run compile`
  - `NSF_TEST_FILE_FILTER=completion-request-coordinator npm run test:client:repo`
  - `NSF_TEST_FILE_FILTER=completion-trigger-characters npm run test:client:repo`
  - `NSF_TEST_FILE_FILTER=completion npm run test:client:repo`
  - `NSF_REAL_REPLAY_INCLUDE_LONG=1 NSF_TEST_REAL_REPLAY_SCRIPT_FILTER=pbr-flow-water-full-input npm run test:client:real:replay`

## Current Conclusion

- Completion is effectively closed for now.
- The remaining apparent tail in the older combined `postLatestVisibleCleanup`
  number was mostly the replay quiet guard, not active provider work.
- In the latest long replay, `postLatestVisibleProviderActivity` stayed at 0
  while `postLatestVisibleQuietGuard` carried the remaining delay.
- No further completion behavior change is justified without new replay
  evidence.

## Remaining Steps

1. Start a new thread on signature-help tail audit.
2. Establish a signature-help baseline using the same replay/report machinery.
3. Only if the new evidence shows a real regression, consider a
   signature-help-specific lifecycle or attribution change.

## Risks / Needs Confirmation

- Do not change completion trigger characters.
- Do not change explicit invoke or member completion behavior.
- Do not treat the legacy combined cleanup number as the bottleneck by itself.
- Do not reopen completion work unless a new replay proves a regression.

## Signature-Help Tail Audit Continuation

Continuation status on 2026-05-15:

- Added replay report summary support for top-level `captureSignatureHelp`
  steps, not only full-document typing probes.
- Added signature-help replay splits parallel to the completion tail audit:
  - `uiLatestVisibleProviderReturn`
  - `uiLatestVisibleLspRequest`
  - `postLatestVisibleProviderActivity`
  - `postLatestVisibleQuietGuard`
  - `uiQueueQuietGuard`
- No product signature-help behavior changed.

Baseline evidence:

- `rw-ux-bumpoffset-warm-signature`:
  - 0 anomalies.
  - 1 signature-help probe.
  - latest visible provider return: 89ms.
  - latest visible LSP request: 18ms.
  - post-latest provider activity: 0ms.
  - post-latest quiet guard: 211ms.
- `rw-pbr-flow-water-full-input`:
  - 0 anomalies.
  - 11 signature-help probes.
  - capture P50 323ms, P95/max 360ms.
  - latest visible provider return P50 89ms, P95/max 103ms.
  - latest visible LSP request P50 11ms, P95/max 19ms.
  - post-latest provider activity P50/P95/max 0ms.
  - post-latest quiet guard P50 220ms, P95/max 263ms.

Conclusion:

- The current signature-help tail does not show a server/provider regression.
- The apparent post-latest tail is carried by replay queue-quiet guard time,
  not additional provider activity.
- No signature-help lifecycle or behavior change is justified from this
  baseline.

Validation:

- `npm run compile`
- `node .\out\test\runCodeTests.js --mode repo --file-filter real-workspace-replay-runner`
- `node .\out\test\runCodeTests.js --mode repo --file-filter signature-help`
- `NSF_TEST_REAL_REPLAY_SCRIPT_FILTER=rw-ux-bumpoffset-warm-signature npm run test:client:real:replay`
- `NSF_REAL_REPLAY_INCLUDE_LONG=1 NSF_TEST_REAL_REPLAY_SCRIPT_FILTER=pbr-flow-water-full-input npm run test:client:real:replay`

## Inlay Continuity Check

Continuation status on 2026-05-15:

- Added `captureInlayContinuity` for full-document replay steps.
- The long `rw-pbr-flow-water-full-input` replay now samples inlay hints at
  every checkpoint over the current typed document range.
- If hints have already appeared, an empty/error sample followed by recovery
  reports `inlay-hints-transient-drop`; ending empty after visible hints reports
  `inlay-hints-ended-missing-after-visible`.

Baseline evidence:

- `rw-pbr-flow-water-full-input`:
  - 0 anomalies.
  - 109 inlay continuity checkpoint samples.
  - 109 non-empty samples.
  - first non-empty sample at checkpoint line 6.
  - minimum hint count after first non-empty: 4.
  - maximum hint count: 91.
  - transient drop count: 0.
  - trailing missing run length: 0.

Conclusion:

- The long real replay did not show an inlay-hints disappear-and-recover
  pattern.
- This check is now part of the long replay anomaly surface, so future
  regressions should fail the replay instead of remaining a visual suspicion.

Validation:

- `npm run compile`
- `node .\out\test\runCodeTests.js --mode repo --file-filter real-workspace-replay-runner`
- `NSF_REAL_REPLAY_INCLUDE_LONG=1 NSF_TEST_REAL_REPLAY_SCRIPT_FILTER=pbr-flow-water-full-input npm run test:client:real:replay`

## Resume Entry

Minimum files to read:

- `README.md`
- `docs/architecture.md`
- `docs/resources.md`
- `docs/testing.md`
- `docs/human-ai/2026-05-15-completion-provider-layer-latency-plan.md`
- this handoff

Minimum commands to rerun for the new thread:

- `npm run compile`
- the relevant `signatureHelp` repo tests once identified
- the relevant signature-help replay / perf script once identified

## Closeout

Root cause:

- The completion tail was being over-attributed to the old combined cleanup
  metric.

Actual changes:

- Added replay attribution splits and tightened the coordinator burst source.

Architecture fit:

- Kept completion semantics on the shared server path and preserved identifier
  trigger behavior.

Verification:

- The focused coordinator tests, completion tests, trigger-character test, and
  long real replay all passed.

Doc update status:

- This handoff is the new continuation note for the next thread.
