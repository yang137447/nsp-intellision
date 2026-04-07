import * as assert from 'assert';
import * as vscode from 'vscode';

import { drainMetricsWindow, waitForNextMetricsRevision } from './perf_helpers';
import {
	getCompletionItems,
	openFixture,
	positionOf,
	repoDescribe,
	touchDocument,
	waitFor,
	waitForClientQuiescent,
	waitForIndexingIdle
} from './test_helpers';

repoDescribe('NSF client integration: Interactive Runtime / Metrics', () => {
	it('exports request queue, context build, owner didChange, and prewarm timings for current-doc interactive flow', async function () {
		this.timeout(90000);

		let document = await openFixture('module_completion_current_doc.nsf');
		const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

		await waitForIndexingIdle('interactive metrics indexing idle');
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
			'interactive metrics completion warmup'
		);
		await waitForClientQuiescent('interactive metrics client quiescent before drain');

		const drained = await drainMetricsWindow('interactive metrics current-doc flow');
		await touchDocument(document);
		document = await vscode.workspace.openTextDocument(document.uri);

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
			'interactive metrics completion after didChange'
		);

		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'interactive metrics flush after didChange'
		);
		const interactiveRuntime = metrics.payload?.interactiveRuntime;
		const completionMetrics = metrics.payload?.completionMetrics;
		assert.ok(
			(interactiveRuntime?.requestQueueWaitSamples ?? 0) > 0,
			`Expected requestQueueWaitSamples > 0. Actual=${JSON.stringify(interactiveRuntime)}`
		);
		assert.ok(
			(interactiveRuntime?.requestContextBuildSamples ?? 0) > 0,
			`Expected requestContextBuildSamples > 0. Actual=${JSON.stringify(interactiveRuntime)}`
		);
		assert.ok(
			(interactiveRuntime?.ownerDidChangeSamples ?? 0) > 0,
			`Expected ownerDidChangeSamples > 0. Actual=${JSON.stringify(interactiveRuntime)}`
		);
		assert.ok(
			(interactiveRuntime?.prewarmSamples ?? 0) > 0,
			`Expected prewarmSamples > 0. Actual=${JSON.stringify(interactiveRuntime)}`
		);
		assert.ok(
			(completionMetrics?.interactiveCollectSamples ?? 0) > 0,
			`Expected interactiveCollectSamples > 0. Actual=${JSON.stringify(completionMetrics)}`
		);
		assert.ok(
			(completionMetrics?.workspaceSummaryQuerySamples ?? 0) > 0,
			`Expected workspaceSummaryQuerySamples > 0. Actual=${JSON.stringify(completionMetrics)}`
		);
		assert.ok(
			(completionMetrics?.itemAssemblySamples ?? 0) > 0,
			`Expected itemAssemblySamples > 0. Actual=${JSON.stringify(completionMetrics)}`
		);
		assert.ok(
			(completionMetrics?.responseWriteSamples ?? 0) > 0,
			`Expected responseWriteSamples > 0. Actual=${JSON.stringify(completionMetrics)}`
		);
	});

	it('keeps owner didChange metrics while dropping synchronous prewarm for syntax-only edits', async function () {
		this.timeout(90000);

		let document = await openFixture('module_completion_current_doc.nsf');
		const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

		await waitForIndexingIdle('interactive syntax-only metrics indexing idle');
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
			'interactive syntax-only metrics completion warmup'
		);
		await waitForClientQuiescent('interactive syntax-only metrics quiescent before drain');

		const drained = await drainMetricsWindow('interactive metrics syntax-only flow');
		const insertEdit = new vscode.WorkspaceEdit();
		insertEdit.insert(document.uri, new vscode.Position(0, 0), ' ');
		assert.ok(await vscode.workspace.applyEdit(insertEdit));
		document = await vscode.workspace.openTextDocument(document.uri);

		const deleteEdit = new vscode.WorkspaceEdit();
		deleteEdit.delete(document.uri, new vscode.Range(0, 0, 0, 1));
		assert.ok(await vscode.workspace.applyEdit(deleteEdit));
		document = await vscode.workspace.openTextDocument(document.uri);

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
			'interactive syntax-only metrics completion after edit'
		);

		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'interactive syntax-only metrics flush after didChange'
		);
		const interactiveRuntime = metrics.payload?.interactiveRuntime;
		assert.ok(
			(interactiveRuntime?.requestQueueWaitSamples ?? 0) > 0,
			`Expected requestQueueWaitSamples > 0. Actual=${JSON.stringify(interactiveRuntime)}`
		);
		assert.ok(
			(interactiveRuntime?.requestContextBuildSamples ?? 0) > 0,
			`Expected requestContextBuildSamples > 0. Actual=${JSON.stringify(interactiveRuntime)}`
		);
		assert.ok(
			(interactiveRuntime?.ownerDidChangeSamples ?? 0) >= 2,
			`Expected ownerDidChangeSamples >= 2 for insert+delete syntax-only edits. Actual=${JSON.stringify(interactiveRuntime)}`
		);
		assert.strictEqual(
			interactiveRuntime?.prewarmSamples ?? 0,
			0,
			`Expected syntax-only edits not to synchronously prewarm interactive snapshots. Actual=${JSON.stringify(interactiveRuntime)}`
		);
	});

	it('keeps owner didChange metrics while dropping synchronous prewarm for comment-only edits', async function () {
		this.timeout(90000);

		await vscode.commands.executeCommand('nsf.restartServer');
		let document = await openFixture('module_completion_current_doc.nsf');
		const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

		await waitForIndexingIdle('interactive comment-only metrics indexing idle');
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
			'interactive comment-only metrics completion warmup'
		);
		await waitForClientQuiescent('interactive comment-only metrics quiescent before drain');

		const drained = await drainMetricsWindow('interactive metrics comment-only flow');
		const insertedText = '\n// interactive metrics comment-only';
		const originalLength = document.getText().length;
		const insertEdit = new vscode.WorkspaceEdit();
		insertEdit.insert(document.uri, document.positionAt(originalLength), insertedText);
		assert.ok(await vscode.workspace.applyEdit(insertEdit));
		document = await vscode.workspace.openTextDocument(document.uri);

		try {
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
				'interactive comment-only metrics completion after edit'
			);

			const metrics = await waitForNextMetricsRevision(
				drained.revision ?? 0,
				'interactive comment-only metrics flush after didChange'
			);
			const interactiveRuntime = metrics.payload?.interactiveRuntime;
			assert.ok(
				(interactiveRuntime?.requestQueueWaitSamples ?? 0) > 0,
				`Expected requestQueueWaitSamples > 0. Actual=${JSON.stringify(interactiveRuntime)}`
			);
			assert.ok(
				(interactiveRuntime?.requestContextBuildSamples ?? 0) > 0,
				`Expected requestContextBuildSamples > 0. Actual=${JSON.stringify(interactiveRuntime)}`
			);
			assert.ok(
				(interactiveRuntime?.ownerDidChangeSamples ?? 0) >= 1,
				`Expected ownerDidChangeSamples >= 1 for comment-only edit. Actual=${JSON.stringify(interactiveRuntime)}`
			);
			assert.strictEqual(
				interactiveRuntime?.prewarmSamples ?? 0,
				0,
				`Expected comment-only edits not to synchronously prewarm interactive snapshots. Actual=${JSON.stringify(interactiveRuntime)}`
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
