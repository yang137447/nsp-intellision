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
