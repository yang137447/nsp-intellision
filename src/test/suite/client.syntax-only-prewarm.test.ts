import * as assert from 'assert';
import * as vscode from 'vscode';

import {
	getCompletionItems,
	getDocumentRuntimeDebug,
	openFixture,
	positionOf,
	repoDescribe,
	waitFor
} from './test_helpers';

repoDescribe('NSF client integration: Interactive Runtime / Syntax-Only DidChange', () => {
	it('keeps last-good interactive state without synchronous current snapshot prewarm for syntax-only edits', async function () {
		this.timeout(90000);

		let document = await openFixture('module_completion_current_doc.nsf');
		const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => {
				const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
				return labels.has('CompletionDocHelper') && labels.has('completionLocalColor');
			},
			'syntax-only prewarm completion warmup'
		);

		const beforeEntries = await getDocumentRuntimeDebug([document.uri.toString()]);
		const before = beforeEntries[0];
		assert.ok(before?.hasInteractiveSnapshot, 'Expected warmup to publish an interactive snapshot.');
		assert.strictEqual(before?.interactiveAnalysisFullFingerprint, before?.analysisFullFingerprint);

		const insertEdit = new vscode.WorkspaceEdit();
		insertEdit.insert(document.uri, new vscode.Position(0, 0), ' ');
		assert.ok(await vscode.workspace.applyEdit(insertEdit), 'Expected syntax-only insert edit to apply.');
		document = await vscode.workspace.openTextDocument(document.uri);

		const deleteEdit = new vscode.WorkspaceEdit();
		deleteEdit.delete(document.uri, new vscode.Range(0, 0, 0, 1));
		assert.ok(await vscode.workspace.applyEdit(deleteEdit), 'Expected syntax-only delete edit to apply.');
		document = await vscode.workspace.openTextDocument(document.uri);

		const afterEntries = await getDocumentRuntimeDebug([document.uri.toString()]);
		const after = afterEntries[0];
		assert.ok(after, 'Expected runtime debug entry after syntax-only edit.');
		assert.strictEqual(
			after?.hasInteractiveSnapshot,
			false,
			`Expected syntax-only didChange not to synchronously publish a new current interactive snapshot. Actual=${JSON.stringify(after)}`
		);
		assert.strictEqual(after?.hasLastGoodInteractiveSnapshot, true);
		assert.ok(
			(after?.lastGoodAnalysisFullFingerprint ?? '').length > 0,
			`Expected last-good interactive fingerprint after syntax-only edit. Actual=${JSON.stringify(after)}`
		);
		assert.notStrictEqual(after?.analysisFullFingerprint, after?.lastGoodAnalysisFullFingerprint);

		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => {
				const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
				return labels.has('CompletionDocHelper') && labels.has('completionLocalColor');
			},
			'syntax-only prewarm completion after edit'
		);
	});

	it('keeps last-good interactive state without synchronous current snapshot prewarm for comment-only edits', async function () {
		this.timeout(90000);

		await vscode.commands.executeCommand('nsf.restartServer');
		let document = await openFixture('module_completion_current_doc.nsf');
		const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => {
				const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
				return labels.has('CompletionDocHelper') && labels.has('completionLocalColor');
			},
			'comment-only prewarm completion warmup'
		);

		const beforeEntries = await getDocumentRuntimeDebug([document.uri.toString()]);
		const before = beforeEntries[0];
		assert.ok(before?.hasInteractiveSnapshot, 'Expected warmup to publish an interactive snapshot.');

		const originalLength = document.getText().length;
		const insertedText = '\n// syntax-only prewarm comment';
		try {
			const insertEdit = new vscode.WorkspaceEdit();
			insertEdit.insert(document.uri, document.positionAt(originalLength), insertedText);
			assert.ok(await vscode.workspace.applyEdit(insertEdit), 'Expected comment-only edit to apply.');
			document = await vscode.workspace.openTextDocument(document.uri);

			const afterEntries = await getDocumentRuntimeDebug([document.uri.toString()]);
			const after = afterEntries[0];
			assert.ok(after, 'Expected runtime debug entry after comment-only edit.');
			assert.strictEqual(
				after?.hasInteractiveSnapshot,
				false,
				`Expected comment-only didChange not to synchronously publish a new current interactive snapshot. Actual=${JSON.stringify(after)}`
			);
			assert.strictEqual(after?.hasLastGoodInteractiveSnapshot, true);
			assert.notStrictEqual(after?.analysisFullFingerprint, after?.lastGoodAnalysisFullFingerprint);

			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
						'vscode.executeCompletionItemProvider',
						document.uri,
						completionPosition
					),
				(value) => {
					const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
					return labels.has('CompletionDocHelper') && labels.has('completionLocalColor');
				},
				'comment-only prewarm completion after edit'
			);
		} finally {
			const cleanupDocument = await vscode.workspace.openTextDocument(document.uri);
			const deleteEdit = new vscode.WorkspaceEdit();
			deleteEdit.delete(
				cleanupDocument.uri,
				new vscode.Range(
					cleanupDocument.positionAt(originalLength),
					cleanupDocument.positionAt(originalLength + insertedText.length)
				)
			);
			await vscode.workspace.applyEdit(deleteEdit);
		}
	});
});
