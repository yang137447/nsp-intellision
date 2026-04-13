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
		assert.ok(
			resolved.uri.fsPath.replace(/\\/g, '/').toLowerCase().endsWith(
				'test_files/module_completion_current_doc.nsf'
			)
		);
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

	it('does not flag completion anomaly when cumulative counters increase', () => {
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
			{ offsetMs: 0, internalStatus: { completionRequestCount: 1, activeRpcCount: 0 } },
			{ offsetMs: 30, internalStatus: { completionRequestCount: 1, activeRpcCount: 0 } },
			{ offsetMs: 80, internalStatus: { completionRequestCount: 2, activeRpcCount: 0 } }
		]);
		assert.ok(!anomalies.includes('completion-request-not-observed'));
	});

	it('returns no anomalies when no samples were captured', () => {
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
		const anomalies = detectReplayAnomalies(step, []);
		assert.deepStrictEqual(anomalies, []);
	});
});
