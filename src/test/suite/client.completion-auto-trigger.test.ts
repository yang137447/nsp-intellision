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

type InternalStatusWithCompletionMetrics = {
	completionRequestCount?: number;
};

repoDescribe('NSF client integration: Completion Auto Trigger', () => {
	it('issues completion requests while typing identifier prefixes', async function () {
		this.timeout(90000);
		await vscode.commands.executeCommand('nsf.restartServer');
		await waitForClientReady('client ready for completion auto trigger');
		await waitForIndexingIdle('indexing idle for completion auto trigger');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'completion auto trigger internal commands ready'
		);

		const pollForCompletionRequest = async (): Promise<InternalStatusWithCompletionMetrics | undefined> => {
			const deadline = Date.now() + 8000;
			let last: InternalStatusWithCompletionMetrics | undefined;
			while (Date.now() < deadline) {
				last = await vscode.commands.executeCommand<InternalStatusWithCompletionMetrics>(
					'nsf._getInternalStatus'
				);
				if ((last?.completionRequestCount ?? 0) > 0) {
					return last;
				}
				await new Promise((resolve) => setTimeout(resolve, 250));
			}
			return last;
		};
		let finalStatus: InternalStatusWithCompletionMetrics | undefined;
		await vscode.commands.executeCommand('workbench.action.closeAllEditors');
		const document = await openFixture('module_completion_current_doc.nsf');
		const editor = await vscode.window.showTextDocument(document, { preview: false });
		const start = positionOf(document, 'return Comp', 1, 'return '.length);
		const end = positionOf(document, 'return Comp', 1, 'return Comp'.length);
		await editor.edit((editBuilder) => {
			editBuilder.delete(new vscode.Range(start, end));
		});
		editor.selection = new vscode.Selection(start, start);
		await vscode.commands.executeCommand('workbench.action.focusActiveEditorGroup');
		await waitFor(
			() => Promise.resolve(vscode.window.activeTextEditor?.document.uri.toString()),
			(value) => value === document.uri.toString(),
			'active editor focused for completion auto trigger'
		);

		try {
			await vscode.commands.executeCommand('nsf._resetInternalStatus');
			for (const ch of ['C', 'o', 'm', 'p']) {
				await typeTextForTests(editor, ch);
				await new Promise((resolve) => setTimeout(resolve, 400));
				finalStatus = await vscode.commands.executeCommand<InternalStatusWithCompletionMetrics>(
					'nsf._getInternalStatus'
				);
				if ((finalStatus?.completionRequestCount ?? 0) > 0) {
					break;
				}
			}

			if ((finalStatus?.completionRequestCount ?? 0) <= 0) {
				finalStatus = await pollForCompletionRequest();
			}
		} finally {
			await editor.edit((editBuilder) => {
				editBuilder.replace(new vscode.Range(start, start.translate(0, 4)), 'Comp');
			});
		}

		assert.ok((finalStatus?.completionRequestCount ?? 0) > 0, JSON.stringify(finalStatus));
	});
});
