import * as assert from 'assert';
import * as vscode from 'vscode';

import {
	openFixture,
	positionOf,
	repoDescribe,
	touchDocument,
	waitFor,
	waitForIndexingIdle
} from './test_helpers';

type InternalStatusWithCompletionClientMetrics = {
	completionRequestCount?: number;
	completionAwaitSyncSamples?: number;
	completionNextSamples?: number;
};

repoDescribe('NSF client integration: Completion Client Metrics', () => {
	it('awaits pending document sync for identifier-prefix completion in dirty docs', async function () {
		this.timeout(90000);

		await openFixture('module_suite.nsf');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'completion client internal commands ready'
		);

		let document = await openFixture('module_completion_current_doc.nsf');
		const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

		await waitForIndexingIdle('completion client metrics indexing idle');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => (Array.isArray(value) ? value.length : value?.items.length ?? 0) > 0,
			'completion client metrics warmup'
		);

		await vscode.commands.executeCommand('nsf._resetInternalStatus');
		await touchDocument(document);
		document = await vscode.workspace.openTextDocument(document.uri);

		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => (Array.isArray(value) ? value.length : value?.items.length ?? 0) > 0,
			'completion client metrics after dirty edit'
		);

		const status = await waitFor(
			() =>
				vscode.commands.executeCommand<InternalStatusWithCompletionClientMetrics>(
					'nsf._getInternalStatus'
				),
			(value) =>
				(value?.completionRequestCount ?? 0) > 0 &&
				(value?.completionNextSamples ?? 0) > 0 &&
				(value?.completionAwaitSyncSamples ?? 0) > 0,
			'completion client await-sync metrics'
		);

		assert.ok((status?.completionRequestCount ?? 0) > 0);
		assert.ok((status?.completionNextSamples ?? 0) > 0);
		assert.ok((status?.completionAwaitSyncSamples ?? 0) > 0);
	});
});
