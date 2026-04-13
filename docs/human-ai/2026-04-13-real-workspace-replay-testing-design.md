# Real Workspace Replay Testing Design

## Status

- This document is a design proposal for collaboration and planning.
- It is not a current-facts document.
- Current facts remain `README.md`, `docs/architecture.md`, `docs/resources.md`, and `docs/testing.md`.

## Context

The current repo already has:

- repo-mode correctness suites
- repo-mode perf suites
- real-workspace smoke and targeted perf suites
- internal test-only commands for client status, metrics, and runtime debug

Those layers are useful, but they still under-cover the main risk now observed in real editing:

- new interactive results are not ready yet
- partial or insufficient new results arrive
- old results are dropped too early or replaced at the wrong time
- completion, signature help, hover, and diagnostics become blocked or visibly delayed during continuous editing

The current test system is stronger at validating isolated requests and milestone baselines than it is at validating short, realistic, continuous edit sequences in a real workspace.

## Goal

Build a real-workspace recording and replay system that can:

1. capture realistic short editing scripts from VS Code interactions
2. replay those scripts through real editor input paths
3. collect per-step timing and runtime evidence
4. generate analysis-first reports that help expose latency, blocking, and sequencing problems

The first phase is explicitly for sampling and problem discovery, not for hard release gating.

## Non-goals

The first phase does not try to:

- build a generic desktop recorder for arbitrary UI interactions
- replace the existing repo-mode correctness suites
- become a strict pass/fail gate for CI
- solve all continuity classification in the first version
- support long multi-minute sessions before short high-value scripts are stable

## Approach Options

### Option A: Keep expanding ad-hoc real workspace perf tests

Add more one-off `realWorkspace.*.test.ts` files and keep hand-writing interaction flow, timing, and report logic in each file.

Pros:

- fastest short-term path
- minimal structural change

Cons:

- script logic, timing logic, and reporting logic keep duplicating
- hard to compare runs across scenarios
- hard to evolve into a reusable system

### Option B: Domain-specific replay harness

Create a focused recorder/replayer/analyzer stack for real VS Code editing flows, with scripts modeled as normalized editor actions rather than raw input logs.

Pros:

- directly targets the current problem
- keeps realistic input paths while remaining replayable
- gives a stable base for future scripts, reports, and regressions

Cons:

- requires a modest framework layer up front

### Option C: Full generic record/replay platform

Build a broad recording engine for arbitrary editor and desktop events, then replay them as a universal interaction trace.

Pros:

- most general long-term capability

Cons:

- too heavy for the immediate need
- high design cost before producing useful evidence

## Recommendation

Choose Option B.

The current need is not a generic automation framework. The current need is a stable, repo-owned way to replay realistic short editing flows in a real workspace and collect timing evidence around the interactive paths that users actually feel.

## High-level Design

The system should have three layers:

1. Recorder
2. Replayer
3. Analyzer

### Recorder

The recorder observes real editing activity and produces a normalized script draft.

It should listen to:

- active editor changes
- selection changes
- document edits
- typed text commands
- delete/backspace-like commands
- explicit command invocations used in a scenario

The recorder should not persist a raw event stream as the primary artifact. Instead, it should convert raw activity into a domain-specific script made of meaningful editor actions.

### Replayer

The replayer executes a normalized script inside a real workspace using real editor behavior.

It should prefer real interaction paths such as:

- `type`
- real selection movement
- real cursor placement
- real editor edits when no real command path exists

The replayer must attach a sampling window to each action so the system can observe not only whether a result eventually appears, but also when it starts moving and whether it is delayed by concurrent work.

### Analyzer

The analyzer converts replay output into structured reports.

The first version is analysis-first:

- emit timing evidence
- emit anomalies
- summarize suspected blocking points

It should avoid over-asserting correctness in phase one.

## Why the script model must be domain-specific

The replay artifact should not be a raw keyboard and editor event log.

Instead, it should be a stable script model built from domain actions such as:

- `openDocument`
- `placeCursor`
- `selectRange`
- `typeText`
- `deleteLeft`
- `deleteRight`
- `pause`
- `invokeCommand`
- `captureSnapshot`
- `sampleWindow`

Reasons:

- raw event logs are noisy and fragile
- line-based and device-specific behavior drifts too easily in real workspaces
- domain actions remain readable and reviewable
- replay remains realistic because execution still goes through real VS Code edit paths

## Script Format

Each script should be stored as a structured JSON document under a dedicated replay-script directory in `src/test` or a neighboring test asset directory.

Each script should include:

- `id`
- `title`
- `workspaceHint`
- `targetDocument`
- `intent`
- `tags`
- `setup`
- `steps`
- `cleanup`

Each step should include:

- `kind`
- `label`
- `target`
- `payload`
- `afterActionPauseMs`
- `samplingWindow`

`target` should prefer anchor-based location rather than hard-coded absolute line numbers:

- workspace folder suffix
- relative path
- anchor text
- occurrence index
- character offset

This reduces brittleness when real workspace files shift slightly.

## Sampling Contract

Every replay step that matters must include a sampling window.

The system should sample over a short ladder such as:

- `0ms`
- `30ms`
- `80ms`
- `160ms`
- `320ms`
- `640ms`

The exact defaults can stay configurable, but the contract is:

- one action
- one short observation window
- multiple probes inside that window

This is essential because the problem is not just “did the feature work”; it is “when did the system react after the user action”.

## Evidence to collect in phase one

Phase one prioritizes timing and delay evidence.

For each relevant step, collect:

- step start and end wall-clock
- sampling timestamps
- `nsf._getInternalStatus`
- `nsf._getLatestMetrics`
- optional `nsf._getMetricsHistory`
- optional `nsf._getDocumentRuntimeDebug`
- optional `nsf._getInteractiveRuntimeDebug`

Primary questions to answer:

- how long after real input does the target request begin
- how long until the target request count or metrics move
- how long until the target result becomes observable
- whether interactive work runs while active RPC backlog remains elevated
- whether background activity appears to delay the target step

## Replay Execution Contract

The replayer should execute each step as:

1. resolve target location
2. perform the editor action
3. run the configured sampling window
4. append timeline data
5. continue or stop depending on script policy

The replayer must also support best-effort restore:

- remember edited ranges or deleted text
- restore the original content after the script
- restore even on failure where possible

Because this is a real workspace, cleanup is part of the core contract, not an afterthought.

## Report Format

The analyzer should write reports beside the existing perf reports, under a dedicated subdirectory such as:

- `out/test/perf-reports/real-replay/`

Each run should emit:

### Step timeline report

For every step:

- action metadata
- target document
- anchor metadata
- start time
- end time
- sampling points
- observed internal status deltas
- observed metrics deltas
- anomalies

### Scenario summary report

Aggregate:

- completion start latency
- signature help start latency
- hover or definition observed latency
- diagnostics change latency
- max active RPC during scenario
- indexing disturbances during scenario
- soft-budget exceed counts

## Phase-one anomaly model

The first release should use soft anomaly markers instead of hard failures for most timing observations.

Examples:

- completion request did not start within window
- signature help request started late
- hover latency exceeded soft budget
- diagnostics change lagged behind edit window
- active RPC backlog stayed high through the whole sampling ladder
- metrics revision advanced but target method count did not move

This keeps the system useful for problem discovery even while real workspace noise remains higher than repo-mode tests.

## First-wave short scripts

Phase one should start with five short scripts:

### 1. Prefix completion script

Flow:

- open a real `.nsf` file
- delete a known suffix from a function or symbol use site
- type a short prefix character-by-character
- sample after each input step

Primary value:

- measure auto-trigger completion timing under real input

### 2. Signature help entry script

Flow:

- trim an existing call site down to the function name
- type `(`
- optionally type `,` and one parameter fragment
- sample after each input step

Primary value:

- measure signature help entry and parameter progression timing

### 3. Member completion chain script

Flow:

- trim an existing member access down to `base.`
- type or continue from that state
- sample after each step

Primary value:

- measure member completion responsiveness and cross-layer interactive timing

### 4. Delete-and-restore diagnostics script

Flow:

- delete `;`, `)`, or a small expression fragment
- sample diagnostics reaction
- restore the original text
- sample recovery timing

Primary value:

- measure edit-to-diagnostics update timing in real workspace context

### 5. Mixed interaction chain script

Flow:

- trigger prefix completion
- continue into a call site
- trigger signature help
- optionally capture hover or definition at the follow-up symbol

Primary value:

- detect multi-step interaction slowdown when earlier work has not fully settled

## Suggested module layout

The implementation should stay focused and avoid mixing responsibilities.

Recommended layout:

- `src/test/replay/real_workspace_replay_types.ts`
  - script schema and report schema
- `src/test/replay/real_workspace_replay_targets.ts`
  - anchor resolution and target lookup
- `src/test/replay/real_workspace_replay_recorder.ts`
  - event capture and script normalization
- `src/test/replay/real_workspace_replay_runner.ts`
  - step execution and cleanup orchestration
- `src/test/replay/real_workspace_replay_sampler.ts`
  - sampling windows and metrics capture
- `src/test/replay/real_workspace_replay_analyzer.ts`
  - anomaly marking and report generation
- `src/test/replay/scripts/*.json`
  - recorded scripts
- `src/test/suite/realWorkspace.replay.test.ts`
  - entry suite that replays selected scripts

This keeps recording, execution, sampling, and analysis decoupled.

## Integration with current test infrastructure

The design should reuse current test infrastructure rather than bypass it.

Key reuse points:

- `runCodeTests.ts` mode and file filtering
- existing real-workspace test launch path
- `nsf._getInternalStatus`
- `nsf._resetInternalStatus`
- `nsf._getLatestMetrics`
- `nsf._getMetricsHistory`
- `nsf._getDocumentRuntimeDebug`
- `nsf._getInteractiveRuntimeDebug`
- existing perf report output conventions

The replay suite should run in `real` mode first.

A later phase can add `perf` mode coverage for selected scripts once noise is better understood.

## Risks

### Workspace drift

Real workspace files may change and invalidate hard-coded locations.

Mitigation:

- anchor-based targeting
- small scripts
- explicit per-script restore logic

### Noise and instability

Real workspace timings will vary more than repo fixtures.

Mitigation:

- analysis-first reporting
- soft anomalies before hard gate thresholds
- short scripts before longer sessions

### Over-generalization

Trying to support every possible editor action in v1 would stall delivery.

Mitigation:

- only support actions required by the first five scripts
- add new step kinds only when a real script needs them

## Open questions for later phases

- whether to add continuity-focused anomaly classification after timing-first phase
- whether to support semi-random script generation on top of recorded scripts
- whether to run selected replay scripts in a dedicated perf mode with repeated sampling
- whether to add server-side instrumentation specifically for replay step correlation

## Recommended first implementation boundary

Phase one should stop after the repository can do all of the following:

1. record a short real editor flow into a normalized script draft
2. replay a normalized script in a real workspace
3. attach per-step sampling windows
4. emit structured real-replay reports
5. run at least the first wave of short scripts

That boundary is sufficient to start collecting evidence for architecture work without prematurely turning the system into a hard gate.
