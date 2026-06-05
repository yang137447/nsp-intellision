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

type PreprocessorMacroPresetResponse = {
	entries?: Array<{
		name?: unknown;
		replacement?: unknown;
	}>;
};

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

async function waitForClientReady(label: string, timeoutMs = 120000): Promise<void> {
	const deadline = Date.now() + timeoutMs;
	let lastState = '';
	while (Date.now() < deadline) {
		const value = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
		lastState = JSON.stringify(value ?? null);
		if (value?.clientState === 'ready') {
			return;
		}
		await delay(WAIT_STEP_MS);
	}
	throw new Error(`Timed out waiting for ${label}. Last client state: ${lastState}`);
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

function hasExplicitPreprocessorMacroSetting(): boolean {
	const inspected = vscode.workspace.getConfiguration('nsf').inspect<Record<string, unknown>>('preprocessorMacros');
	return (
		inspected?.globalValue !== undefined ||
		inspected?.workspaceValue !== undefined ||
		inspected?.workspaceFolderValue !== undefined
	);
}

function normalizePreprocessorMacroPreset(
	response: PreprocessorMacroPresetResponse
): Record<string, string | number | boolean> {
	const macros: Record<string, string | number | boolean> = {};
	const entries = Array.isArray(response.entries) ? response.entries : [];
	for (const entry of entries) {
		const name = typeof entry?.name === 'string' ? entry.name.trim() : '';
		const replacement = entry?.replacement;
		if (
			name.length === 0 ||
			!(
				typeof replacement === 'string' ||
				typeof replacement === 'number' ||
				typeof replacement === 'boolean'
			)
		) {
			continue;
		}
		macros[name] = replacement;
	}
	return macros;
}

async function seedReplayPreprocessorMacrosIfMissing(): Promise<void> {
	if (hasExplicitPreprocessorMacroSetting()) {
		return;
	}
	const response = await vscode.commands.executeCommand<PreprocessorMacroPresetResponse>(
		'nsf._sendServerRequest',
		{ method: 'nsf/getPreprocessorMacroPreset', params: {} }
	);
	const preset = normalizePreprocessorMacroPreset(response ?? {});
	assert.ok(Object.keys(preset).length > 0, 'Expected preprocessor macro preset entries from server registry.');
	await vscode.workspace
		.getConfiguration('nsf')
		.update('preprocessorMacros', preset, vscode.ConfigurationTarget.Global);
	await vscode.commands.executeCommand('nsf.restartServer');
	await waitForClientReady('client ready after real replay preprocessor macro seed');
}

async function waitForRealReplayReady(): Promise<void> {
	await waitForCommandAvailable('nsf._getInternalStatus');
	await waitForClientReady('real replay client ready');
	await seedReplayPreprocessorMacrosIfMissing();
	await waitForInternalStatusIdle();
}

realDescribe('NSF real workspace replay', () => {
	const scriptFilter = (process.env.NSF_TEST_REAL_REPLAY_SCRIPT_FILTER ?? '').toLowerCase();
	const includeLongScripts = process.env.NSF_REAL_REPLAY_INCLUDE_LONG === '1';
	const scripts = loadReplayScripts()
		.filter((script) => includeLongScripts || !script.tags.includes('long-running'))
		.filter((script) =>
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
			this.timeout(script.tags.includes('long-running') ? 900000 : 300000);
			const report = await runReplayScript(script);
			const reportPath = writeReplayReport(script.id, report);
			assert.strictEqual(report.scriptId, script.id);
			assert.ok(report.steps.length > 0);
			assert.ok(reportPath.replace(/\\/g, '/').includes('/real-replay/'));

			for (const step of report.steps) {
				const anomalies = step.anomalies ?? [];
				const unexpected = anomalies.filter((entry) => entry !== 'active-rpc-backlog-never-settled');
				assert.strictEqual(
					unexpected.length,
					0,
					`Step ${step.stepLabel ?? 'unknown'} reported unexpected anomalies: ${unexpected.join(', ')}`
				);
			}
		});
	}
});
