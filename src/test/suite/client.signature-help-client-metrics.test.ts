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

type InternalStatusWithSignatureHelpClientMetrics = {
	signatureHelpRequestCount?: number;
	signatureHelpAwaitSyncSamples?: number;
	signatureHelpNextSamples?: number;
};

repoDescribe('NSF client integration: Signature Help Client Metrics', () => {
	it('awaits pending document sync for signature help in dirty docs', async function () {
		this.timeout(90000);

		await openFixture('module_suite.nsf');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'signature-help client internal commands ready'
		);

		let document = await openFixture('module_signature_help.nsf');
		const signaturePosition = positionOf(
			document,
			'SigTarget(uv, 2.0, 3',
			1,
			'SigTarget(uv, '.length
		);

		await waitForIndexingIdle('signature-help client metrics indexing idle');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					signaturePosition
				),
			(value) => Boolean(value) && (value?.signatures.length ?? 0) > 0,
			'signature-help client metrics warmup'
		);

		await vscode.commands.executeCommand('nsf._resetInternalStatus');
		await touchDocument(document);
		document = await vscode.workspace.openTextDocument(document.uri);

		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					signaturePosition
				),
			(value) => Boolean(value) && (value?.signatures.length ?? 0) > 0,
			'signature-help client metrics after dirty edit'
		);

		const status = await waitFor(
			() =>
				vscode.commands.executeCommand<InternalStatusWithSignatureHelpClientMetrics>(
					'nsf._getInternalStatus'
				),
			(value) =>
				(value?.signatureHelpRequestCount ?? 0) > 0 &&
				(value?.signatureHelpNextSamples ?? 0) > 0 &&
				(value?.signatureHelpAwaitSyncSamples ?? 0) > 0,
			'signature-help client await-sync metrics'
		);

		assert.ok((status?.signatureHelpRequestCount ?? 0) > 0);
		assert.ok((status?.signatureHelpNextSamples ?? 0) > 0);
		assert.ok((status?.signatureHelpAwaitSyncSamples ?? 0) > 0);
	});
});
