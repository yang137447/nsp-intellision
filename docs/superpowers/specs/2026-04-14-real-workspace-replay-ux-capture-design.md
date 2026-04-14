# Real Workspace Replay UX Capture (Completion + Signature Help)

Date: 2026-04-14

## Context

We have a real-workspace replay suite (`npm run test:client:real:replay`) that replays short typing scripts against a configured external workspace (currently `C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace`).

Current replay reports focus on:

- Whether interactive request counters (completion/signature help) are observed during a sampling window.
- Soft anomalies (e.g. `active-rpc-backlog-never-settled`) recorded in reports but not treated as hard failures.

However, we currently do not capture the user-visible content correctness:

- Which completion popup items are actually returned (e.g. expected symbol missing).
- Whether signature help after typing `(` contains the expected signature/parameter order.

We also need better evidence to locate performance bottlenecks for these UX flows.

## Problem Statement

In real workspace editing (example from `shader-source/sfx/nodes/pbr_flow_water_nodes.hlsl`), users observe:

1. Completion popup missing expected items (e.g. function `BumpOffsetUV` not suggested when typing prefix).
2. Slow interactive experience (completion/signature help appears late).

We want to reproduce these interactions via replay and record enough structured evidence to:

- Debug "missing popup items" with concrete captured lists and context.
- Diagnose "slow" by correlating provider latency with internal status/metrics/debug snapshots.

## Goals

- Extend the real-workspace replay framework to capture:
  - Completion result content (labels, counts) at a cursor position.
  - Signature help result content (signature label(s), active param).
  - Provider-call duration (ms).
  - Nearby debug snapshots (`nsf._getInternalStatus`, `nsf._getLatestMetrics`, optional runtime/interactive debug).
- Add new real workspace replay scripts that cover realistic user flows in the target file(s).
- Default behavior: **write reports only, do not fail the test run** when content is missing or unexpected. (We will store "observations" in the report and keep the suite green.)

## Non-Goals

- Introducing perf gates / failing thresholds in this phase.
- Fixing the underlying completion/signature help correctness or performance issues yet.
- Replacing the existing repo-mode tests; this is additive real-workspace coverage.

## Proposed Approach (Recommended)

### 1) Add Replay Step Kinds for Capturing UX Outputs

Extend `ReplayStep` with capture steps that do not mutate documents:

- `captureCompletion`
  - Executes `vscode.executeCompletionItemProvider` for the active document/cursor (or a provided target anchor).
  - Records:
    - durationMs
    - itemCount
    - topLabels (bounded, e.g. first 50)
    - (optional) expectedLabels and whether they were present (report-only)
    - cursor location + current line text (for debugging)
- `captureSignatureHelp`
  - Executes `vscode.executeSignatureHelpProvider` for the active document/cursor (or provided target anchor).
  - Records:
    - durationMs
    - signatures (bounded, e.g. first 10 labels)
    - activeSignature / activeParameter
    - (optional) expectedSignatureSubstrings and whether matched (report-only)
    - cursor location + current line text

These steps should be report-only: do not add new hard-asserting anomalies, and do not cause `realWorkspace.replay.test.ts` to fail.

### 2) Extend Report Schema (Backward-Compatible)

Augment `runReplayScript` step reports with optional fields, e.g.:

- `completionCapture?: { durationMs, itemCount, topLabels, expectedLabels?, expectedPresent?, position?, lineText? }`
- `signatureHelpCapture?: { durationMs, signatureLabels, activeSignature?, activeParameter?, expected?, expectedMatched?, position?, lineText? }`

Existing fields (`samples`, `anomalies`) remain unchanged so existing scripts and report readers keep working.

### 3) Add New Real Workspace Scripts for the Report-Only Phase

Add scripts under `src/test/replay/scripts/` that target the real workspace file where users reported the issue:

- `shader-source/sfx/nodes/pbr_flow_water_nodes.hlsl`
  - Completion flow:
    - Navigate to the `BumpOffsetUV(...)` callsite.
    - Replace the function name with a short prefix (e.g. `Bu`) to mimic typing.
    - Capture completion items and timing at the cursor.
    - (optional) also sample internal status over a window to correlate request/queue behavior.
  - Signature flow:
    - Navigate to the same callsite.
    - Replace the existing argument list with `(` to mimic entering a call.
    - Capture signature help content and timing at the cursor.

All scripts should set `cleanup.restoreTouchedDocuments = true`.

### 4) Data Collection Workflow

- Run all scripts:
  - `npm run test:client:real:replay`
- Run just the new scripts:
  - Use `NSF_TEST_REAL_REPLAY_SCRIPT_FILTER` to select script ids (substring match).
- Inspect report JSON under:
  - `out/test/perf-reports/real-replay/`

## Risks / Tradeoffs

- Capturing completion/signature via explicit provider commands may not match the exact UI widget composition, but it provides actionable evidence about the provider outputs and latencies.
- Provider capture calls may themselves add interactive workload; we will keep capture bounded (e.g. capture once per script step, cap item counts).
- Real workspace environments vary; scripts should avoid fragile assertions (hence report-only phase).

## Validation

For this phase, success means:

- `npm run test:client:real:replay` stays green.
- The report JSON includes the new capture fields for the new scripts.
- Captures are readable and bounded (no megabytes of completion items).

## Next Steps (After Data Collection)

- Use captured evidence to identify:
  - Whether `BumpOffsetUV` is absent from completion results, and what the provider returned instead.
  - Whether signature help has wrong signature/param ordering, and at what layer it was resolved.
  - Where latency accumulates (client-side wait, server RPC, indexing backlog, etc.).
- Decide whether to:
  - Add soft anomaly classification for content mismatch (still not failing), or
  - Promote specific correctness checks to hard failures once stabilized.

