import * as assert from 'assert';
import * as vscode from 'vscode';

import { openFixture, positionOf, repoDescribe, waitFor, waitForClientReady } from './test_helpers';

type InternalStatusWithCompletionMetrics = {
	completionRequestCount?: number;
};

repoDescribe('NSF client integration: Completion Auto Trigger', () => {
	it('issues completion requests while typing identifier prefixes', async function () {
		this.timeout(90000);

		const document = await openFixture('module_completion_current_doc.nsf');
		await waitForClientReady('client ready for completion auto trigger');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'completion auto trigger internal commands ready'
		);

		const editor = await vscode.window.showTextDocument(document, { preview: false });
		const start = positionOf(document, 'return Comp', 1, 'return '.length);
		const end = positionOf(document, 'return Comp', 1, 'return Comp'.length);
		await editor.edit((editBuilder) => {
			editBuilder.delete(new vscode.Range(start, end));
		});
		editor.selection = new vscode.Selection(start, start);

		try {
			await vscode.commands.executeCommand('nsf._resetInternalStatus');
			for (const ch of ['C', 'o', 'm', 'p']) {
				await vscode.commands.executeCommand('type', { text: ch });
			}

			const status = await waitFor(
				() =>
					vscode.commands.executeCommand<InternalStatusWithCompletionMetrics>(
						'nsf._getInternalStatus'
					),
				(value) => (value?.completionRequestCount ?? 0) > 0,
				'completion auto trigger request count'
			);

			assert.ok((status?.completionRequestCount ?? 0) > 0, JSON.stringify(status));
		} finally {
			await editor.edit((editBuilder) => {
				editBuilder.replace(new vscode.Range(start, start.translate(0, 4)), 'Comp');
			});
		}
	});
});
