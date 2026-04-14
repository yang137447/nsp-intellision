# Real Workspace Replay UX Capture (Completion + Signature Help) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the real-workspace replay harness to capture completion/signature-help content + timing into replay reports, and add scripts for `pbr_flow_water_nodes.hlsl` (report-only, no test failures).

**Architecture:** Add two new replay step kinds (`captureCompletion`, `captureSignatureHelp`) executed by the replay runner. Each step records provider results (bounded) + durationMs into the step report, while optional `samplingWindow` continues to record internal status / metrics / runtime + interactive debug snapshots.

**Tech Stack:** TypeScript, VS Code integration tests (`vscode.execute*Provider` commands), existing replay harness under `src/test/replay/*`.

---

### Task 1: Extend Replay Types And Report Shape

**Files:**
- Modify: `src/test/replay/real_workspace_replay_types.ts`
- Modify: `src/test/replay/real_workspace_replay_runner.ts`

- [ ] **Step 1: Extend `ReplayStep` union**

Add two step kinds:

```ts
export type ReplayStep =
  | /* existing */
  | {
      kind: 'captureCompletion';
      label: string;
      payload?: { expectedLabels?: string[]; maxLabels?: number };
      afterActionPauseMs?: number;
      samplingWindow?: ReplaySamplingWindow;
    }
  | {
      kind: 'captureSignatureHelp';
      label: string;
      payload?: { expectedSubstrings?: string[]; maxSignatures?: number };
      afterActionPauseMs?: number;
      samplingWindow?: ReplaySamplingWindow;
    };
```

- [ ] **Step 2: Extend runner step report type to carry capture fields**

Add optional fields on the per-step report:

```ts
type ReplayStepReport = {
  /* existing */
  completionCapture?: {
    durationMs: number;
    itemCount: number;
    topLabels: string[];
    expectedLabels?: string[];
    expectedPresent?: Record<string, boolean>;
    line?: number;
    character?: number;
    lineText?: string;
    lastCompletionDebug?: unknown;
  };
  signatureHelpCapture?: {
    durationMs: number;
    signatureCount: number;
    signatureLabels: string[];
    activeSignature?: number;
    activeParameter?: number;
    expectedSubstrings?: string[];
    expectedMatched?: Record<string, boolean>;
    line?: number;
    character?: number;
    lineText?: string;
  };
};
```

- [ ] **Step 3: Run typecheck locally**

Run: `npm run compile`
Expected: exit 0

---

### Task 2: Implement Provider Captures In Replay Runner

**Files:**
- Modify: `src/test/replay/real_workspace_replay_runner.ts`

- [ ] **Step 1: Add helpers to normalize labels**

```ts
function completionLabelToString(label: unknown): string {
  if (typeof label === 'string') return label;
  const maybe = label as any;
  if (maybe && typeof maybe.label === 'string') return maybe.label;
  return String(label ?? '');
}
```

- [ ] **Step 2: Implement `captureCompletion` execution**

Logic:

1. Require `activeEditor` and read `(uri, position, lineText)`.
2. `const started = Date.now();`
3. Execute completion provider:
   `await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', uri, position)`
4. Compute `durationMs`.
5. Extract `items` and `topLabels` (cap by `maxLabels`, default 50).
6. Optional expected presence check:
   `expectedPresent[label] = topLabels.includes(label)`.
7. Best-effort capture `nsf._getLastCompletionDebug` (try/catch).

- [ ] **Step 3: Implement `captureSignatureHelp` execution**

Logic:

1. Require `activeEditor` and read `(uri, position, lineText)`.
2. `await vscode.commands.executeCommand('vscode.executeSignatureHelpProvider', uri, position)`
3. Extract signature labels and active indices; cap by `maxSignatures` default 10.
4. Optional substring-match map:
   `expectedMatched[s] = signatureLabels.some(label => label.includes(s))`.

- [ ] **Step 4: Ensure report-only behavior**

Do not add new anomalies. Any provider failure should be caught and recorded as empty capture fields, not thrown.

- [ ] **Step 5: Run replay suite locally**

Run: `npm run test:client:real:replay`
Expected: exit 0

---

### Task 3: Add Real Workspace Replay Scripts For `pbr_flow_water_nodes.hlsl`

**Files:**
- Create: `src/test/replay/scripts/rw-ux-bumpoffset-completion.json`
- Create: `src/test/replay/scripts/rw-ux-bumpoffset-signature.json`

- [ ] **Step 1: Confirm target file anchors**

Target path: `shader-source/sfx/nodes/pbr_flow_water_nodes.hlsl`

Anchor text candidates (from the real workspace file):

```text
BumpOffsetUV(MaterialParameters, diffuse_sample_uv, u_maintex_height)
```

- [ ] **Step 2: Script A (completion)**

Flow:

1. `openDocument` at `BumpOffsetUV(...)` anchor.
2. `selectRange` covering `BumpOffsetUV` (offset 0..11).
3. `typeText` payload `Bu` (no samplingWindow, avoid anomalies).
4. `captureCompletion` with samplingWindow delays `[0,30,80,160,320,640]` and debug capture flags.

- [ ] **Step 3: Script B (signature)**

Flow:

1. `openDocument` at `BumpOffsetUV(...)` anchor.
2. `selectRange` covering full argument list including parens:
   `"(MaterialParameters, diffuse_sample_uv, u_maintex_height)"`
3. `typeText` payload `"("` (no samplingWindow).
4. `captureSignatureHelp` with samplingWindow delays `[0,30,80,160,320,640]` and expected substrings:
   - `BumpOffsetUV`
   - `MaterialParameters`
   - `coordinate_uv` (or `diffuse_sample_uv` if signature uses callsite names; capture first then decide)
   - `height`

Note: This phase is report-only, so mismatches are captured but do not fail the run.

- [ ] **Step 4: Run filtered replay**

Run:

```powershell
$env:NSF_TEST_REAL_REPLAY_SCRIPT_FILTER="bumpoffset"; npm run test:client:real:replay
```

Expected: exit 0, reports written under `out/test/perf-reports/real-replay/`.

---

### Task 4: Commit Incrementally

**Files:**
- Modify/Create: as above

- [ ] **Step 1: Commit replay harness changes**

```bash
git add src/test/replay/real_workspace_replay_types.ts src/test/replay/real_workspace_replay_runner.ts
git commit -m "test: capture completion/signature in real workspace replay reports"
```

- [ ] **Step 2: Commit new scripts**

```bash
git add src/test/replay/scripts/rw-ux-bumpoffset-completion.json src/test/replay/scripts/rw-ux-bumpoffset-signature.json
git commit -m "test: add real workspace replay scripts for bumpoffset UX"
```

