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

		const recorded = await vscode.commands.executeCommand<{
			workspaceHint?: string;
			steps?: Array<{
				kind: string;
				target?: {
					workspaceFolderSuffix?: string;
					relativePath?: string;
					anchorText?: string;
					characterOffset?: number;
					occurrence?: number;
				};
				payload?: { text?: string; count?: number };
			}>;
		}>('nsf._stopReplayRecording');

		assert.strictEqual(recorded?.workspaceHint, 'repo-fixture');
		assert.ok(Array.isArray(recorded?.steps));
		const steps = recorded!.steps!;
		assert.strictEqual(steps.length, 3);
		assert.strictEqual(steps[0].kind, 'placeCursor');
		assert.strictEqual(
			steps[0].target?.workspaceFolderSuffix?.toLowerCase(),
			'nsp-intellision'
		);
		assert.strictEqual(steps[0].target?.relativePath, 'test_files/module_completion_current_doc.nsf');
		assert.strictEqual(steps[0].target?.anchorText, document.lineAt(0).text);
		assert.strictEqual(steps[0].target?.characterOffset, 0);
		assert.strictEqual(steps[0].target?.occurrence, 1);
		assert.strictEqual(steps[1].kind, 'typeText');
		assert.strictEqual(steps[1].payload?.text, 'X');
		assert.strictEqual(steps[2].kind, 'deleteLeft');
		assert.strictEqual(steps[2].payload?.count, 1);
	});
});
