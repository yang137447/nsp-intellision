# Real Workspace Replay Testing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a real-workspace recording and replay testing system that captures short editor interaction scripts, replays them through real VS Code input paths, and emits timing-first analysis reports for interactive latency and blocking issues.

**Architecture:** Keep the recorder in the client test-only internal-command layer so it can observe real editor/document events inside the extension host, but keep replay execution, sampling, script loading, target resolution, and anomaly analysis in focused `src/test/replay/*` modules owned by the integration test harness. Reuse the existing internal status and metrics commands instead of inventing a parallel observability stack, and keep phase one analysis-first by emitting soft anomalies and JSON reports instead of hard performance gates.

**Tech Stack:** TypeScript, VS Code extension/test APIs, Mocha integration tests, existing `runCodeTests` launcher, existing `nsf._getInternalStatus` / `nsf._getLatestMetrics` / runtime debug commands

---

## File Structure

- Create: `client/src/client_real_workspace_replay_recording.ts`
  - Own the recorder state, raw event capture, event normalization, and start/stop lifecycle.
- Modify: `client/src/client_internal_commands.ts`
  - Expose replay recording test-only commands and associated TypeScript types.
- Modify: `client/src/extension.ts`
  - Instantiate the recorder and wire it into the existing internal-command dependency object.
- Create: `src/test/replay/real_workspace_replay_types.ts`
  - Define script, step, target, sampling, report, and anomaly types for the replay system.
- Create: `src/test/replay/real_workspace_replay_targets.ts`
  - Resolve workspace-folder suffixes, relative paths, anchor text, and offset-based cursor/range locations.
- Create: `src/test/replay/real_workspace_replay_analyzer.ts`
  - Turn per-step sampling windows into soft anomaly markers and per-scenario summaries.
- Create: `src/test/replay/real_workspace_replay_sampler.ts`
  - Execute sampling ladders against the existing internal status and metrics commands.
- Create: `src/test/replay/real_workspace_replay_runner.ts`
  - Execute replay steps, coordinate sampling, maintain touched-document restore state, and return structured reports.
- Create: `src/test/replay/real_workspace_replay_script_loader.ts`
  - Load JSON scripts from the source tree and filter them by optional environment variables.
- Create: `src/test/replay/real_workspace_replay_report_writer.ts`
  - Write replay reports under `out/test/perf-reports/real-replay/` without changing the existing perf helper contract.
- Create: `src/test/replay/scripts/rw-prefix-completion.json`
  - First-wave short replay script for auto-trigger completion timing.
- Create: `src/test/replay/scripts/rw-signature-entry.json`
  - First-wave short replay script for signature-help entry timing.
- Create: `src/test/replay/scripts/rw-member-chain.json`
  - First-wave short replay script for member completion timing.
- Create: `src/test/replay/scripts/rw-diagnostics-delete-restore.json`
  - First-wave short replay script for diagnostics update timing.
- Create: `src/test/replay/scripts/rw-mixed-interaction.json`
  - First-wave short replay script for chained completion/signature/hover timing.
- Create: `src/test/suite/client.real-workspace-replay-core.test.ts`
  - Repo-mode TDD coverage for anchor resolution and anomaly detection.
- Create: `src/test/suite/client.real-workspace-replay-recorder.test.ts`
  - Repo-mode TDD coverage for normalized replay recording.
- Create: `src/test/suite/client.real-workspace-replay-runner.test.ts`
  - Repo-mode TDD coverage for step execution, sampling, and restore behavior.
- Create: `src/test/suite/realWorkspace.replay.test.ts`
  - Real-workspace replay entry suite that loads JSON scripts, replays them, and emits reports.
- Modify: `package.json`
  - Add a dedicated `test:client:real:replay` command.
- Modify: `docs/testing.md`
  - Document the new command, suite, report directory, and scenario-filter workflow.

### Task 1: Define replay schema, anchor resolution, and anomaly analysis

**Files:**
- Create: `src/test/replay/real_workspace_replay_types.ts`
- Create: `src/test/replay/real_workspace_replay_targets.ts`
- Create: `src/test/replay/real_workspace_replay_analyzer.ts`
- Test: `src/test/suite/client.real-workspace-replay-core.test.ts`

- [ ] **Step 1: Write the failing repo-mode core test**

```ts
import * as assert from 'assert';
import * as vscode from 'vscode';

import { repoDescribe, openFixture } from './test_helpers';
import type { ReplayAnchor, ReplayStep } from '../replay/real_workspace_replay_types';
import { resolveReplayAnchor } from '../replay/real_workspace_replay_targets';
import { detectReplayAnomalies } from '../replay/real_workspace_replay_analyzer';

repoDescribe('NSF real workspace replay core', () => {
	it('resolves an anchor against a repository fixture', async () => {
		const document = await openFixture('module_completion_current_doc.nsf');
		const anchor: ReplayAnchor = {
			workspaceFolderSuffix: 'nsp-intellision',
			relativePath: 'test_files/module_completion_current_doc.nsf',
			anchorText: 'CompletionDocHelper',
			occurrence: 1,
			characterOffset: 0
		};

		const resolved = await resolveReplayAnchor(anchor);
		const text = document.getText(
			new vscode.Range(resolved.position, resolved.position.translate(0, 'CompletionDocHelper'.length))
		);

		assert.strictEqual(text, 'CompletionDocHelper');
	});

	it('flags a missing completion request inside the sampling window', () => {
		const step: ReplayStep = {
			kind: 'typeText',
			label: 'type prefix',
			payload: { text: 'Pix' },
			afterActionPauseMs: 0,
			samplingWindow: {
				label: 'completion-window',
				delaysMs: [0, 30, 80]
			}
		};

		const anomalies = detectReplayAnomalies(step, [
			{ offsetMs: 0, internalStatus: { completionRequestCount: 0 } },
			{ offsetMs: 30, internalStatus: { completionRequestCount: 0 } },
			{ offsetMs: 80, internalStatus: { completionRequestCount: 0 } }
		]);

		assert.ok(anomalies.includes('completion-request-not-observed'));
	});
});
```

- [ ] **Step 2: Run compile to verify the new test fails before implementation**

Run: `npm run compile`

Expected: FAIL with `TS2307` or `TS2305` errors for missing replay modules such as `../replay/real_workspace_replay_targets`

- [ ] **Step 3: Implement the shared replay types**

```ts
export type ReplayAnchor = {
	workspaceFolderSuffix: string;
	relativePath: string;
	anchorText: string;
	occurrence?: number;
	characterOffset?: number;
};

export type ReplaySamplingWindow = {
	label: string;
	delaysMs: number[];
	captureRuntimeDebug?: boolean;
	captureInteractiveDebug?: boolean;
};

export type ReplayStep =
	| {
			kind: 'openDocument';
			label: string;
			target: ReplayAnchor;
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'placeCursor';
			label: string;
			target: ReplayAnchor;
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'selectRange';
			label: string;
			target: ReplayAnchor;
			payload: { startOffset: number; endOffset: number };
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'typeText';
			label: string;
			payload: { text: string };
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'deleteLeft';
			label: string;
			payload?: { count?: number };
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'invokeCommand';
			label: string;
			payload: { command: string; args?: unknown[] };
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  };

export type ReplayScript = {
	id: string;
	title: string;
	workspaceHint: string;
	targetDocument: ReplayAnchor;
	intent: string;
	tags: string[];
	steps: ReplayStep[];
	cleanup?: { restoreTouchedDocuments: boolean };
};

export type ReplaySampleSnapshot = {
	offsetMs: number;
	internalStatus?: any;
	latestMetrics?: any;
	runtimeDebug?: any;
	interactiveDebug?: any;
};
```

- [ ] **Step 4: Implement anchor resolution and timing-first anomaly detection**

```ts
import * as path from 'path';
import * as vscode from 'vscode';

import type { ReplayAnchor, ReplaySampleSnapshot, ReplayStep } from './real_workspace_replay_types';

export async function resolveReplayAnchor(anchor: ReplayAnchor): Promise<{ uri: vscode.Uri; position: vscode.Position }> {
	const folder = (vscode.workspace.workspaceFolders ?? []).find((item) =>
		item.uri.fsPath.replace(/\\/g, '/').toLowerCase().endsWith(anchor.workspaceFolderSuffix.toLowerCase())
	);
	if (!folder) {
		throw new Error(`Unable to find workspace folder ending with '${anchor.workspaceFolderSuffix}'.`);
	}

	const uri = vscode.Uri.file(path.join(folder.uri.fsPath, anchor.relativePath));
	const document = await vscode.workspace.openTextDocument(uri);
	const occurrence = Math.max(1, anchor.occurrence ?? 1);
	let searchFrom = 0;
	let foundAt = -1;
	for (let index = 0; index < occurrence; index++) {
		foundAt = document.getText().indexOf(anchor.anchorText, searchFrom);
		if (foundAt < 0) {
			throw new Error(`Unable to resolve anchor '${anchor.anchorText}' in ${anchor.relativePath}.`);
		}
		searchFrom = foundAt + anchor.anchorText.length;
	}

	return {
		uri,
		position: document.positionAt(foundAt + (anchor.characterOffset ?? 0))
	};
}

export function detectReplayAnomalies(step: ReplayStep, samples: ReplaySampleSnapshot[]): string[] {
	const anomalies: string[] = [];
	const windowLabel = step.samplingWindow?.label.toLowerCase() ?? '';
	if (windowLabel.includes('completion') || windowLabel.includes('member')) {
		const sawCompletion = samples.some((sample) => (sample.internalStatus?.completionRequestCount ?? 0) > 0);
		if (!sawCompletion) {
			anomalies.push('completion-request-not-observed');
		}
	}
	if (windowLabel.includes('signature') || (step.kind === 'typeText' && step.payload.text.includes('('))) {
		const sawSignatureHelp = samples.some((sample) => (sample.internalStatus?.signatureHelpRequestCount ?? 0) > 0);
		if (!sawSignatureHelp) {
			anomalies.push('signature-help-request-not-observed');
		}
	}
	if (samples.every((sample) => (sample.internalStatus?.activeRpcCount ?? 0) > 0)) {
		anomalies.push('active-rpc-backlog-never-settled');
	}
	return anomalies;
}
```

- [ ] **Step 5: Run compile and the repo-mode core test**

Run: `npm run compile && node .\out\test\runCodeTests.js --mode repo --file-filter real-workspace-replay-core`

Expected: PASS with `client.real-workspace-replay-core.test.ts` loaded and both assertions green

- [ ] **Step 6: Commit the core replay module**

```bash
git add src/test/replay/real_workspace_replay_types.ts src/test/replay/real_workspace_replay_targets.ts src/test/replay/real_workspace_replay_analyzer.ts src/test/suite/client.real-workspace-replay-core.test.ts
git commit -m "test: add replay core schema and analyzer"
```

### Task 2: Add normalized replay recording through test-only internal commands

**Files:**
- Create: `client/src/client_real_workspace_replay_recording.ts`
- Modify: `client/src/client_internal_commands.ts`
- Modify: `client/src/extension.ts`
- Test: `src/test/suite/client.real-workspace-replay-recorder.test.ts`

- [ ] **Step 1: Write the failing recorder integration test**

```ts
import * as assert from 'assert';
import * as vscode from 'vscode';

import { repoDescribe, openFixture } from './test_helpers';

repoDescribe('NSF real workspace replay recorder', () => {
	it('records normalized typing and deletion steps', async function () {
		this.timeout(120000);

		const document = await openFixture('module_completion_current_doc.nsf');
		const editor = await vscode.window.showTextDocument(document, { preview: false });
		editor.selection = new vscode.Selection(new vscode.Position(0, 0), new vscode.Position(0, 0));

		await vscode.commands.executeCommand('nsf._startReplayRecording', {
			id: 'repo-recorder-smoke',
			workspaceHint: 'repo-fixture'
		});
		await vscode.commands.executeCommand('type', { text: 'X' });
		await vscode.commands.executeCommand('deleteLeft');

		const recorded = await vscode.commands.executeCommand<{ steps?: Array<{ kind: string }> }>(
			'nsf._stopReplayRecording'
		);

		assert.ok(Array.isArray(recorded?.steps));
		assert.ok(recorded!.steps!.some((step) => step.kind === 'typeText'));
		assert.ok(recorded!.steps!.some((step) => step.kind === 'deleteLeft'));
	});
});
```

- [ ] **Step 2: Run the targeted repo test and verify it fails before the commands exist**

Run: `npm run compile && node .\out\test\runCodeTests.js --mode repo --file-filter real-workspace-replay-recorder`

Expected: FAIL with `command 'nsf._startReplayRecording' not found` or an equivalent missing-command assertion

- [ ] **Step 3: Implement the recorder state and normalization module**

```ts
import * as vscode from 'vscode';

type RawReplayEvent =
	| { kind: 'selection'; uri: string; line: number; character: number }
	| { kind: 'insert'; uri: string; text: string }
	| { kind: 'delete'; uri: string; deletedLength: number };

type ReplayRecordingState = {
	active: boolean;
	meta: Record<string, unknown>;
	rawEvents: RawReplayEvent[];
};

export function createRealWorkspaceReplayRecorder() {
	const state: ReplayRecordingState = { active: false, meta: {}, rawEvents: [] };
	const push = (event: RawReplayEvent) => {
		if (state.active) {
			state.rawEvents.push(event);
		}
	};

	const disposables = [
		vscode.window.onDidChangeTextEditorSelection((event) => {
			const active = event.selections[0]?.active;
			if (!active) {
				return;
			}
			push({
				kind: 'selection',
				uri: event.textEditor.document.uri.toString(),
				line: active.line,
				character: active.character
			});
		}),
		vscode.workspace.onDidChangeTextDocument((event) => {
			for (const change of event.contentChanges) {
				if (change.text.length > 0 && change.rangeLength === 0) {
					push({ kind: 'insert', uri: event.document.uri.toString(), text: change.text });
				} else if (change.text.length === 0 && change.rangeLength > 0) {
					push({ kind: 'delete', uri: event.document.uri.toString(), deletedLength: change.rangeLength });
				}
			}
		})
	];

	return {
		start(meta?: Record<string, unknown>) {
			state.active = true;
			state.meta = meta ?? {};
			state.rawEvents = [];
		},
		stop() {
			state.active = false;
			return {
				id: String(state.meta.id ?? 'recorded-replay'),
				workspaceHint: String(state.meta.workspaceHint ?? ''),
				steps: state.rawEvents.map((event) =>
					event.kind === 'insert'
						? { kind: 'typeText', label: 'recorded type', payload: { text: event.text } }
						: event.kind === 'delete'
							? { kind: 'deleteLeft', label: 'recorded delete', payload: { count: event.deletedLength } }
							: { kind: 'placeCursor', label: 'recorded cursor', target: { workspaceFolderSuffix: '', relativePath: '', anchorText: '' } }
				)
			};
		},
		dispose() {
			vscode.Disposable.from(...disposables).dispose();
		}
	};
}
```

- [ ] **Step 4: Register the start/stop recording commands and wire the recorder in `extension.ts`**

```ts
// client/src/client_internal_commands.ts
export type ReplayRecordingDraft = {
	id: string;
	workspaceHint: string;
	steps: Array<{ kind: string; label: string; payload?: unknown; target?: unknown }>;
};

export type InternalCommandDeps = {
	// existing deps remain in place
	startReplayRecording: (payload?: Record<string, unknown>) => void;
	stopReplayRecording: () => ReplayRecordingDraft;
};

context.subscriptions.push(
	commands.registerCommand('nsf._startReplayRecording', async (payload?: Record<string, unknown>) =>
		deps.startReplayRecording(payload)
	)
);
context.subscriptions.push(
	commands.registerCommand('nsf._stopReplayRecording', async () => deps.stopReplayRecording())
);
```

```ts
// client/src/extension.ts
import { createRealWorkspaceReplayRecorder } from './client_real_workspace_replay_recording';

const replayRecorder = createRealWorkspaceReplayRecorder();
context.subscriptions.push(replayRecorder);

registerInternalCommands(context, {
	// existing deps remain in place
	startReplayRecording: (payload) => replayRecorder.start(payload ?? {}),
	stopReplayRecording: () => replayRecorder.stop()
});
```

- [ ] **Step 5: Run compile and the recorder repo test**

Run: `npm run compile && node .\out\test\runCodeTests.js --mode repo --file-filter real-workspace-replay-recorder`

Expected: PASS with the recorded draft containing at least one `typeText` step and one `deleteLeft` step

- [ ] **Step 6: Commit the recorder integration**

```bash
git add client/src/client_real_workspace_replay_recording.ts client/src/client_internal_commands.ts client/src/extension.ts src/test/suite/client.real-workspace-replay-recorder.test.ts
git commit -m "test: add replay recorder internal commands"
```

### Task 3: Implement sampling windows, step execution, and restore behavior

**Files:**
- Create: `src/test/replay/real_workspace_replay_sampler.ts`
- Create: `src/test/replay/real_workspace_replay_runner.ts`
- Test: `src/test/suite/client.real-workspace-replay-runner.test.ts`

- [ ] **Step 1: Write the failing runner test against a repository fixture**

```ts
import * as assert from 'assert';

import { repoDescribe } from './test_helpers';
import type { ReplayScript } from '../replay/real_workspace_replay_types';
import { runReplayScript } from '../replay/real_workspace_replay_runner';

repoDescribe('NSF real workspace replay runner', () => {
	it('replays a short script and captures sampled timeline entries', async function () {
		this.timeout(120000);

		const script: ReplayScript = {
			id: 'repo-runner-smoke',
			title: 'Repo runner smoke',
			workspaceHint: 'nsp-intellision',
			targetDocument: {
				workspaceFolderSuffix: 'nsp-intellision',
				relativePath: 'test_files/module_completion_current_doc.nsf',
				anchorText: 'CompletionDocHelper',
				occurrence: 1,
				characterOffset: 0
			},
			intent: 'Verify replay step execution on repo fixtures.',
			tags: ['repo', 'smoke'],
			steps: [
				{
					kind: 'openDocument',
					label: 'open fixture',
					target: {
						workspaceFolderSuffix: 'nsp-intellision',
						relativePath: 'test_files/module_completion_current_doc.nsf',
						anchorText: 'CompletionDocHelper',
						occurrence: 1,
						characterOffset: 0
					}
				},
				{
					kind: 'typeText',
					label: 'type prefix',
					payload: { text: 'Pix' },
					samplingWindow: { label: 'completion-window', delaysMs: [0, 30, 80] }
				}
			],
			cleanup: { restoreTouchedDocuments: true }
		};

		const report = await runReplayScript(script);
		assert.strictEqual(report.scriptId, 'repo-runner-smoke');
		assert.strictEqual(report.steps.length, 2);
		assert.strictEqual(report.steps[1].samples.length, 3);
	});
});
```

- [ ] **Step 2: Run compile and the runner test to verify it fails before implementation**

Run: `npm run compile && node .\out\test\runCodeTests.js --mode repo --file-filter real-workspace-replay-runner`

Expected: FAIL with missing-module errors or `runReplayScript is not a function`

- [ ] **Step 3: Implement the sampling ladder**

```ts
import * as vscode from 'vscode';

import type { ReplaySampleSnapshot, ReplayStep } from './real_workspace_replay_types';

function delay(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

export async function sampleReplayWindow(step: ReplayStep, documentUri?: string): Promise<ReplaySampleSnapshot[]> {
	const delaysMs = step.samplingWindow?.delaysMs ?? [];
	const startedAt = Date.now();
	const samples: ReplaySampleSnapshot[] = [];

	for (const targetDelay of delaysMs) {
		const elapsed = Date.now() - startedAt;
		if (targetDelay > elapsed) {
			await delay(targetDelay - elapsed);
		}

		const [internalStatus, latestMetrics, runtimeDebug, interactiveDebug] = await Promise.all([
			vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
			vscode.commands.executeCommand<any>('nsf._getLatestMetrics'),
			step.samplingWindow?.captureRuntimeDebug && documentUri
				? vscode.commands.executeCommand<any>('nsf._getDocumentRuntimeDebug', { uris: [documentUri] })
				: Promise.resolve(undefined),
			step.samplingWindow?.captureInteractiveDebug && documentUri
				? vscode.commands.executeCommand<any>('nsf._getInteractiveRuntimeDebug', { uri: documentUri })
				: Promise.resolve(undefined)
		]);

		samples.push({ offsetMs: targetDelay, internalStatus, latestMetrics, runtimeDebug, interactiveDebug });
	}

	return samples;
}
```

- [ ] **Step 4: Implement step execution, touched-document restore, and report assembly**

```ts
import * as vscode from 'vscode';

import { detectReplayAnomalies } from './real_workspace_replay_analyzer';
import { sampleReplayWindow } from './real_workspace_replay_sampler';
import { resolveReplayAnchor } from './real_workspace_replay_targets';
import type { ReplayScript } from './real_workspace_replay_types';

function delay(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

export async function runReplayScript(script: ReplayScript): Promise<any> {
	const originalContents = new Map<string, string>();
	let activeEditor: vscode.TextEditor | undefined;
	const stepReports: any[] = [];

	for (const step of script.steps) {
		const actionStartedAtMs = Date.now();

		if (step.kind === 'openDocument') {
			const resolved = await resolveReplayAnchor(step.target);
			const document = await vscode.workspace.openTextDocument(resolved.uri);
			activeEditor = await vscode.window.showTextDocument(document, { preview: false });
			if (!originalContents.has(document.uri.toString())) {
				originalContents.set(document.uri.toString(), document.getText());
			}
		} else if (step.kind === 'placeCursor') {
			const resolved = await resolveReplayAnchor(step.target);
			const document = await vscode.workspace.openTextDocument(resolved.uri);
			activeEditor = await vscode.window.showTextDocument(document, { preview: false });
			activeEditor.selection = new vscode.Selection(resolved.position, resolved.position);
		} else if (step.kind === 'selectRange') {
			const resolved = await resolveReplayAnchor(step.target);
			const document = await vscode.workspace.openTextDocument(resolved.uri);
			activeEditor = await vscode.window.showTextDocument(document, { preview: false });
			const start = resolved.position.translate(0, step.payload.startOffset);
			const end = resolved.position.translate(0, step.payload.endOffset);
			activeEditor.selection = new vscode.Selection(start, end);
		} else if (step.kind === 'typeText') {
			await vscode.commands.executeCommand('type', { text: step.payload.text });
		} else if (step.kind === 'deleteLeft') {
			for (let index = 0; index < Math.max(1, step.payload?.count ?? 1); index++) {
				await vscode.commands.executeCommand('deleteLeft');
			}
		} else if (step.kind === 'invokeCommand') {
			await vscode.commands.executeCommand(step.payload.command, ...(step.payload.args ?? []));
		}

		if (step.afterActionPauseMs && step.afterActionPauseMs > 0) {
			await delay(step.afterActionPauseMs);
		}

		const documentUri = activeEditor?.document.uri.toString();
		const samples = await sampleReplayWindow(step, documentUri);
		stepReports.push({
			stepLabel: step.label,
			stepKind: step.kind,
			actionStartedAtMs,
			actionEndedAtMs: Date.now(),
			documentUri,
			samples,
			anomalies: detectReplayAnomalies(step, samples)
		});
	}

	for (const [uriText, originalText] of originalContents.entries()) {
		const uri = vscode.Uri.parse(uriText);
		const document = await vscode.workspace.openTextDocument(uri);
		const fullRange = new vscode.Range(document.positionAt(0), document.positionAt(document.getText().length));
		const edit = new vscode.WorkspaceEdit();
		edit.replace(uri, fullRange, originalText);
		await vscode.workspace.applyEdit(edit);
	}

	return { scriptId: script.id, steps: stepReports };
}
```

- [ ] **Step 5: Run compile and the runner repo test**

Run: `npm run compile && node .\out\test\runCodeTests.js --mode repo --file-filter real-workspace-replay-runner`

Expected: PASS with the runner producing a report that contains two steps and three samples on the replayed `typeText` action

- [ ] **Step 6: Commit the sampling and runner layer**

```bash
git add src/test/replay/real_workspace_replay_sampler.ts src/test/replay/real_workspace_replay_runner.ts src/test/suite/client.real-workspace-replay-runner.test.ts
git commit -m "test: add replay sampler and runner"
```

### Task 4: Add real-workspace script loading, first-wave JSON scripts, and replay suite

**Files:**
- Create: `src/test/replay/real_workspace_replay_script_loader.ts`
- Create: `src/test/replay/real_workspace_replay_report_writer.ts`
- Create: `src/test/replay/scripts/rw-prefix-completion.json`
- Create: `src/test/replay/scripts/rw-signature-entry.json`
- Create: `src/test/replay/scripts/rw-member-chain.json`
- Create: `src/test/replay/scripts/rw-diagnostics-delete-restore.json`
- Create: `src/test/replay/scripts/rw-mixed-interaction.json`
- Create: `src/test/suite/realWorkspace.replay.test.ts`
- Modify: `package.json`

- [ ] **Step 1: Write the failing real-workspace replay suite**

```ts
import * as assert from 'assert';

import { loadReplayScripts } from '../replay/real_workspace_replay_script_loader';
import { writeReplayReport } from '../replay/real_workspace_replay_report_writer';
import { runReplayScript } from '../replay/real_workspace_replay_runner';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const realDescribe = testMode === 'real' ? describe : describe.skip;

realDescribe('NSF real workspace replay', () => {
	const scriptFilter = (process.env.NSF_TEST_REAL_REPLAY_SCRIPT_FILTER ?? '').toLowerCase();
	const scripts = loadReplayScripts().filter((script) =>
		scriptFilter.length === 0 ? true : script.id.toLowerCase().includes(scriptFilter)
	);

	it('loads at least one replay script', () => {
		assert.ok(scripts.length > 0, 'Expected at least one real-workspace replay script.');
	});

	for (const script of scripts) {
		it(`replays ${script.id}`, async function () {
			this.timeout(300000);
			const report = await runReplayScript(script);
			const reportPath = writeReplayReport(script.id, report);
			assert.strictEqual(report.scriptId, script.id);
			assert.ok(report.steps.length > 0);
			assert.ok(reportPath.replace(/\\/g, '/').includes('/real-replay/'));
		});
	}
});
```

- [ ] **Step 2: Run the real-workspace suite and verify it fails before scripts are present**

Run: `npm run compile && node .\out\test\runCodeTests.js --mode real --workspace "C:\Software\WorkTemp\G66ShaderDevelop\G66ShaderDevelop.code-workspace" --file-filter realWorkspace.replay`

Expected: FAIL with `Expected at least one real-workspace replay script.`

- [ ] **Step 3: Implement the script loader and replay report writer**

```ts
import * as fs from 'fs';
import * as path from 'path';

import type { ReplayScript } from './real_workspace_replay_types';

export function loadReplayScripts(): ReplayScript[] {
	const repoRoot = path.resolve(__dirname, '..', '..', '..');
	const scriptDir = path.join(repoRoot, 'src', 'test', 'replay', 'scripts');
	if (!fs.existsSync(scriptDir)) {
		return [];
	}
	return fs
		.readdirSync(scriptDir)
		.filter((name) => name.endsWith('.json'))
		.sort()
		.map((name) => JSON.parse(fs.readFileSync(path.join(scriptDir, name), 'utf8')) as ReplayScript);
}
```

```ts
import * as fs from 'fs';
import * as path from 'path';

import { getWorkspaceRoot } from '../suite/test_helpers';

export function writeReplayReport(reportName: string, report: unknown): string {
	const reportDir = path.join(getWorkspaceRoot(), 'out', 'test', 'perf-reports', 'real-replay');
	fs.mkdirSync(reportDir, { recursive: true });
	const safeName = reportName.replace(/[^a-z0-9._-]+/gi, '_').toLowerCase();
	const reportPath = path.join(reportDir, `${safeName}.json`);
	fs.writeFileSync(reportPath, `${JSON.stringify(report, null, 2)}\n`, 'utf8');
	console.log(`[real-replay] wrote ${reportPath}`);
	return reportPath;
}
```

- [ ] **Step 4: Add the completion, signature, and member replay scripts**

```json
{
  "id": "rw-prefix-completion",
  "title": "Real workspace prefix completion",
  "workspaceHint": "shader-source",
  "targetDocument": {
    "workspaceFolderSuffix": "shader-source",
    "relativePath": "sfx/uber_fx_common.nsf",
    "anchorText": "diffuseUV = Pixelate(diffuseUV, batch.maintex_pixel_size);",
    "occurrence": 1,
    "characterOffset": 0
  },
  "intent": "Measure auto-trigger completion latency while typing a function prefix in a real workspace file.",
  "tags": ["interactive", "completion", "real-workspace"],
  "steps": [
    {
      "kind": "openDocument",
      "label": "open prefix completion document",
      "target": {
        "workspaceFolderSuffix": "shader-source",
        "relativePath": "sfx/uber_fx_common.nsf",
        "anchorText": "diffuseUV = Pixelate(diffuseUV, batch.maintex_pixel_size);",
        "occurrence": 1,
        "characterOffset": 0
      }
    },
    {
      "kind": "selectRange",
      "label": "select Pixelate suffix",
      "target": {
        "workspaceFolderSuffix": "shader-source",
        "relativePath": "sfx/uber_fx_common.nsf",
        "anchorText": "Pixelate",
        "occurrence": 1,
        "characterOffset": 0
      },
      "payload": { "startOffset": 0, "endOffset": 8 }
    },
    {
      "kind": "typeText",
      "label": "type Pixe",
      "payload": { "text": "Pixe" },
      "samplingWindow": { "label": "completion-window", "delaysMs": [0, 30, 80, 160, 320] }
    }
  ],
  "cleanup": { "restoreTouchedDocuments": true }
}
```

```json
{
  "id": "rw-signature-entry",
  "title": "Real workspace signature entry",
  "workspaceHint": "shader-source",
  "targetDocument": {
    "workspaceFolderSuffix": "shader-source",
    "relativePath": "sfx/uber_fx_common.nsf",
    "anchorText": "diffuseUV = Pixelate(diffuseUV, batch.maintex_pixel_size);",
    "occurrence": 1,
    "characterOffset": 0
  },
  "intent": "Measure signature-help timing when entering a function call.",
  "tags": ["interactive", "signature-help", "real-workspace"],
  "steps": [
    {
      "kind": "openDocument",
      "label": "open signature document",
      "target": {
        "workspaceFolderSuffix": "shader-source",
        "relativePath": "sfx/uber_fx_common.nsf",
        "anchorText": "diffuseUV = Pixelate(diffuseUV, batch.maintex_pixel_size);",
        "occurrence": 1,
        "characterOffset": 0
      }
    },
    {
      "kind": "selectRange",
      "label": "select function arguments",
      "target": {
        "workspaceFolderSuffix": "shader-source",
        "relativePath": "sfx/uber_fx_common.nsf",
        "anchorText": "(diffuseUV, batch.maintex_pixel_size)",
        "occurrence": 1,
        "characterOffset": 0
      },
      "payload": { "startOffset": 0, "endOffset": 37 }
    },
    {
      "kind": "typeText",
      "label": "type open paren",
      "payload": { "text": "(" },
      "samplingWindow": { "label": "signature-window", "delaysMs": [0, 30, 80, 160, 320] }
    }
  ],
  "cleanup": { "restoreTouchedDocuments": true }
}
```

```json
{
  "id": "rw-member-chain",
  "title": "Real workspace member completion chain",
  "workspaceHint": "shader-source",
  "targetDocument": {
    "workspaceFolderSuffix": "shader-source",
    "relativePath": "sfx/uber_fx_common.nsf",
    "anchorText": "addmaskdistort2UV.x",
    "occurrence": 1,
    "characterOffset": 0
  },
  "intent": "Measure member completion latency after trimming a member access down to base-dot.",
  "tags": ["interactive", "member-completion", "real-workspace"],
  "steps": [
    {
      "kind": "openDocument",
      "label": "open member completion document",
      "target": {
        "workspaceFolderSuffix": "shader-source",
        "relativePath": "sfx/uber_fx_common.nsf",
        "anchorText": "addmaskdistort2UV.x",
        "occurrence": 1,
        "characterOffset": 0
      }
    },
    {
      "kind": "selectRange",
      "label": "select member suffix",
      "target": {
        "workspaceFolderSuffix": "shader-source",
        "relativePath": "sfx/uber_fx_common.nsf",
        "anchorText": ".x",
        "occurrence": 1,
        "characterOffset": 0
      },
      "payload": { "startOffset": 0, "endOffset": 2 }
    },
    {
      "kind": "typeText",
      "label": "type member prefix",
      "payload": { "text": ".x" },
      "samplingWindow": { "label": "member-window", "delaysMs": [0, 30, 80, 160, 320] }
    }
  ],
  "cleanup": { "restoreTouchedDocuments": true }
}
```

- [ ] **Step 5: Add the diagnostics and mixed-interaction scripts**

```json
{
  "id": "rw-diagnostics-delete-restore",
  "title": "Real workspace diagnostics delete and restore",
  "workspaceHint": "shader-source",
  "targetDocument": {
    "workspaceFolderSuffix": "shader-source",
    "relativePath": "base/building.nsf",
    "anchorText": "float4 diffuse_color =",
    "occurrence": 1,
    "characterOffset": 0
  },
  "intent": "Measure diagnostics timing after deleting and restoring a terminating token.",
  "tags": ["diagnostics", "edit-latency", "real-workspace"],
  "steps": [
    {
      "kind": "openDocument",
      "label": "open diagnostics document",
      "target": {
        "workspaceFolderSuffix": "shader-source",
        "relativePath": "base/building.nsf",
        "anchorText": "float4 diffuse_color =",
        "occurrence": 1,
        "characterOffset": 0
      }
    },
    {
      "kind": "placeCursor",
      "label": "place cursor near semicolon",
      "target": {
        "workspaceFolderSuffix": "shader-source",
        "relativePath": "base/building.nsf",
        "anchorText": ";",
        "occurrence": 1,
        "characterOffset": 1
      }
    },
    {
      "kind": "deleteLeft",
      "label": "delete semicolon",
      "payload": { "count": 1 },
      "samplingWindow": {
        "label": "diagnostics-delete-window",
        "delaysMs": [0, 30, 80, 160, 320],
        "captureRuntimeDebug": true
      }
    },
    {
      "kind": "typeText",
      "label": "restore semicolon",
      "payload": { "text": ";" },
      "samplingWindow": {
        "label": "diagnostics-restore-window",
        "delaysMs": [0, 30, 80, 160, 320],
        "captureRuntimeDebug": true
      }
    }
  ],
  "cleanup": { "restoreTouchedDocuments": true }
}
```

```json
{
  "id": "rw-mixed-interaction",
  "title": "Real workspace mixed interaction chain",
  "workspaceHint": "shader-source",
  "targetDocument": {
    "workspaceFolderSuffix": "shader-source",
    "relativePath": "sfx/uber_fx_common.nsf",
    "anchorText": "diffuseUV = Pixelate(diffuseUV, batch.maintex_pixel_size);",
    "occurrence": 1,
    "characterOffset": 0
  },
  "intent": "Measure chained completion, signature-help, and hover timing inside one short editing flow.",
  "tags": ["interactive", "mixed", "real-workspace"],
  "steps": [
    {
      "kind": "openDocument",
      "label": "open mixed interaction document",
      "target": {
        "workspaceFolderSuffix": "shader-source",
        "relativePath": "sfx/uber_fx_common.nsf",
        "anchorText": "diffuseUV = Pixelate(diffuseUV, batch.maintex_pixel_size);",
        "occurrence": 1,
        "characterOffset": 0
      }
    },
    {
      "kind": "selectRange",
      "label": "select full call suffix",
      "target": {
        "workspaceFolderSuffix": "shader-source",
        "relativePath": "sfx/uber_fx_common.nsf",
        "anchorText": "Pixelate(diffuseUV, batch.maintex_pixel_size)",
        "occurrence": 1,
        "characterOffset": 0
      },
      "payload": { "startOffset": 0, "endOffset": 41 }
    },
    {
      "kind": "typeText",
      "label": "type Pixe",
      "payload": { "text": "Pixe" },
      "samplingWindow": {
        "label": "mixed-completion-window",
        "delaysMs": [0, 30, 80, 160, 320],
        "captureInteractiveDebug": true
      }
    },
    {
      "kind": "typeText",
      "label": "type open paren",
      "payload": { "text": "(" },
      "samplingWindow": {
        "label": "mixed-signature-window",
        "delaysMs": [0, 30, 80, 160, 320],
        "captureInteractiveDebug": true
      }
    }
  ],
  "cleanup": { "restoreTouchedDocuments": true }
}
```

- [ ] **Step 6: Add the dedicated npm script for replay runs**

```json
{
  "scripts": {
    "test:client:real:replay": "npm run compile && node .\\out\\test\\runCodeTests.js --mode real --workspace \"C:\\Software\\WorkTemp\\G66ShaderDevelop\\G66ShaderDevelop.code-workspace\" --file-filter realWorkspace.replay"
  }
}
```

- [ ] **Step 7: Run the real replay suite and verify it passes**

Run: `npm run test:client:real:replay`

Expected:
- PASS with `realWorkspace.replay.test.ts` loaded
- one JSON report per replay script under `out\test\perf-reports\real-replay\`
- no replay step leaves the real workspace file modified after the test ends

- [ ] **Step 8: Commit the real-workspace replay scripts and suite**

```bash
git add src/test/replay/real_workspace_replay_script_loader.ts src/test/replay/real_workspace_replay_report_writer.ts src/test/replay/scripts src/test/suite/realWorkspace.replay.test.ts package.json
git commit -m "test: add real workspace replay scripts"
```

### Task 5: Document the replay workflow and verify the full phase-one slice

**Files:**
- Modify: `docs/testing.md`

- [ ] **Step 1: Update `docs/testing.md` with the new replay command and suite**

```md
### `npm run test:client:real:replay`

用途：

- 运行真实 workspace 的 replay suite
- 回放录制/编排后的短脚本，采集每一步的 wall-clock、internal status、metrics 和 anomaly 报告
- 将报告写到 `out/test/perf-reports/real-replay/`

适用场景：

- 排查连续真实输入下 completion / signature help / member completion / diagnostics 的延迟与阻塞
- 对比架构改造前后的真实交互时序

说明：

- 该 suite 当前是 analysis-first，不以 hard perf gate 为目标
- 可通过环境变量 `NSF_TEST_REAL_REPLAY_SCRIPT_FILTER` 只运行某个脚本子集
```

- [ ] **Step 2: Run compile and the three repo-mode replay tests together**

Run: `npm run compile && node .\out\test\runCodeTests.js --mode repo --file-filter real-workspace-replay`

Expected: PASS with `client.real-workspace-replay-core.test.ts`, `client.real-workspace-replay-recorder.test.ts`, and `client.real-workspace-replay-runner.test.ts` all green

- [ ] **Step 3: Run the real replay command after the doc update**

Run: `npm run test:client:real:replay`

Expected:
- PASS
- reports written for the selected replay scripts
- no cleanup failures or permanently edited real-workspace documents

- [ ] **Step 4: Run the broader TypeScript integration regression**

Run: `npm run test:client:repo`

Expected: PASS without regressions in existing repo-mode integration suites

- [ ] **Step 5: Commit the docs and final verification slice**

```bash
git add docs/testing.md
git commit -m "docs: document real workspace replay testing"
```

## Self-Review

### Spec coverage

- Recorder exists as a dedicated layer: Task 2.
- Replayer exists as a dedicated layer: Task 3.
- Analyzer exists as a dedicated layer: Task 1 and Task 3.
- Scripts are stored as normalized JSON artifacts: Task 4.
- Real-input replay path is exercised in a real workspace: Task 4.
- Timing-first sampling and soft anomalies are implemented before hard gating: Task 1, Task 3, and Task 5.
- First-wave short scripts cover completion, signature help, member completion, diagnostics, and mixed interaction: Task 4.
- Reports are written under the existing perf-report workflow: Task 4 and Task 5.
- New test command and suite documentation are synced: Task 5.

### Marker scan

- No unresolved planning markers remain.
- Each task names exact files, exact commands, and concrete code or JSON snippets.

### Type consistency

- `ReplayAnchor`, `ReplayStep`, `ReplayScript`, `ReplaySampleSnapshot`, and `detectReplayAnomalies(...)` are introduced in Task 1 and reused consistently in Tasks 2-4.
- Recorder output uses normalized step kinds that match the runner’s supported `ReplayStep['kind']` set.

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-04-13-real-workspace-replay-testing.md`. Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
