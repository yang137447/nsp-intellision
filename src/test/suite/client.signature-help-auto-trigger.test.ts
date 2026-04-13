import * as assert from 'assert';
import * as vscode from 'vscode';

import {
	openFixture,
	positionOf,
	repoDescribe,
	typeTextForTests,
	waitFor,
	waitForClientReady,
	waitForIndexingIdle
} from './test_helpers';

type InternalStatusWithSignatureHelpMetrics = {
	signatureHelpRequestCount?: number;
};

repoDescribe('NSF client integration: Signature Help Auto Trigger', () => {
	it('issues signature-help requests when typing opening paren', async function () {
		this.timeout(90000);

		await vscode.commands.executeCommand('nsf.restartServer');
		await vscode.commands.executeCommand('workbench.action.closeAllEditors');
		const document = await openFixture('module_signature_help.nsf');
		await waitForClientReady('client ready for signature-help auto trigger');
		await waitForIndexingIdle('indexing idle for signature-help auto trigger');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'signature-help auto trigger internal commands ready'
		);

		const editor = await vscode.window.showTextDocument(document, { preview: false });
		const start = positionOf(document, 'SigTarget(uv, 2.0, 3)', 1, 'SigTarget'.length);
		const end = positionOf(document, 'SigTarget(uv, 2.0, 3)', 1, 'SigTarget(uv, 2.0, 3)'.length);
		await editor.edit((editBuilder) => {
			editBuilder.delete(new vscode.Range(start, end));
		});
		editor.selection = new vscode.Selection(start, start);
		await vscode.commands.executeCommand('workbench.action.focusActiveEditorGroup');
		await waitFor(
			() => Promise.resolve(vscode.window.activeTextEditor?.document.uri.toString()),
			(value) => value === document.uri.toString(),
			'active editor focused for signature-help auto trigger'
		);

		try {
			await vscode.commands.executeCommand('nsf._resetInternalStatus');
			await typeTextForTests(editor, '(');

			const status = await waitFor(
				() =>
					vscode.commands.executeCommand<InternalStatusWithSignatureHelpMetrics>(
						'nsf._getInternalStatus'
					),
				(value) => (value?.signatureHelpRequestCount ?? 0) > 0,
				'signature-help auto trigger request count'
			);

			assert.ok((status?.signatureHelpRequestCount ?? 0) > 0, JSON.stringify(status));
		} finally {
			await editor.edit((editBuilder) => {
				editBuilder.replace(new vscode.Range(start, start.translate(0, 1)), '(uv, 2.0, 3)');
			});
		}
	});
});
