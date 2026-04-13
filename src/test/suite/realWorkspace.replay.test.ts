import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import { loadReplayScripts } from '../replay/real_workspace_replay_script_loader';
import { ReplayScript } from '../replay/real_workspace_replay_types';
import { writeReplayReport } from '../replay/real_workspace_replay_report_writer';
import { runReplayScript } from '../replay/real_workspace_replay_runner';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const realDescribe = testMode === 'real' ? describe : describe.skip;

const WAIT_STEP_MS = 500;

function delay(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

function getWorkspaceFolderPath(folderSuffix: string): string | undefined {
	const folder = vscode.workspace.workspaceFolders?.find((item) =>
		item.uri.fsPath.replace(/\\/g, '/').toLowerCase().endsWith(folderSuffix.toLowerCase())
	);
	return folder?.uri.fsPath;
}

async function openScriptTargetDocument(script: ReplayScript): Promise<void> {
	const folderPath = getWorkspaceFolderPath(script.targetDocument.workspaceFolderSuffix);
	if (!folderPath) {
		return;
	}
	try {
		const uri = vscode.Uri.file(path.join(folderPath, script.targetDocument.relativePath));
		const document = await vscode.workspace.openTextDocument(uri);
		await vscode.window.showTextDocument(document, { preview: false });
	} catch {
		// best-effort activation attempt; ignore errors
	}
}

async function waitForCommandAvailable(command: string, timeoutMs = 120000): Promise<void> {
	const deadline = Date.now() + timeoutMs;
	while (Date.now() < deadline) {
		const commands = await vscode.commands.getCommands(true);
		if (commands.includes(command)) {
			return;
		}
		await delay(WAIT_STEP_MS);
	}
	throw new Error(`Timed out waiting for command ${command}`);
}

async function waitForInternalStatusIdle(timeoutMs = 120000): Promise<void> {
	const deadline = Date.now() + timeoutMs;
	while (Date.now() < deadline) {
		try {
			const value = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
			const state = value?.indexingState;
			const idle =
				!state ||
				(state.state === 'Idle' &&
					(state.pending?.queuedTasks ?? 0) === 0 &&
					(state.pending?.runningWorkers ?? 0) === 0);
			if (idle) {
				return;
			}
		} catch {
			// ignore
		}
		await delay(WAIT_STEP_MS);
	}
	throw new Error('Timed out waiting for real workspace internal status to settle');
}

async function waitForRealReplayReady(): Promise<void> {
	await waitForCommandAvailable('nsf._getInternalStatus');
	await waitForInternalStatusIdle();
}

realDescribe('NSF real workspace replay', () => {
	const scriptFilter = (process.env.NSF_TEST_REAL_REPLAY_SCRIPT_FILTER ?? '').toLowerCase();
	const scripts = loadReplayScripts().filter((script) =>
		scriptFilter.length === 0 ? true : script.id.toLowerCase().includes(scriptFilter)
	);

	before(async function () {
		this.timeout(180000);
		if (scripts.length === 0) {
			return;
		}
		await openScriptTargetDocument(scripts[0]);
		await waitForRealReplayReady();
	});

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

			for (const step of report.steps) {
				if (Array.isArray(step.samples) && step.samples.length > 0) {
					assert.strictEqual(
						step.anomalies?.length ?? 0,
						0,
						`Step ${step.stepLabel ?? 'unknown'} reported anomalies: ${step.anomalies?.join(', ')}`
					);
				}
			}
		});
	}
});
