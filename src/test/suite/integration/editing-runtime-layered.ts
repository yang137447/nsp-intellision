import * as assert from 'assert';

import * as vscode from 'vscode';

import {
	getCompletionItems,
	getDocumentRuntimeDebug,
	getWorkspaceRoot,
	openFixture,
	positionOf,
	repoDescribe,
	waitFor,
	waitForClientReady,
	waitForClientQuiescent,
	waitForDiagnostics,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from '../test_helpers';

import * as path from 'path';

export function registerEditingRuntimeLayeredTests(): void {
	repoDescribe('NSF client integration: Editing Runtime / Layered Debug', () => {
		it('reports document state, semantic layer ownership, and global context readiness in runtime debug', async function () {
			this.timeout(120000);
			let completionDoc = await openFixture('module_completion_current_doc.nsf');

			const completionPosition = positionOf(
				completionDoc,
				'return Comp',
				1,
				'return Comp'.length
			);
			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
						'vscode.executeCompletionItemProvider',
						completionDoc.uri,
						completionPosition
					),
				(value) => {
					const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
					return labels.has('CompletionDocHelper') && labels.has('completionLocalColor');
				},
				'layered runtime completion warmup'
			);
			await waitForIndexingIdle('layered runtime indexing idle');

			await waitForClientReady('client ready before layered runtime debug');
			let lastCompletionDebug: unknown;
			const debug = await waitFor(
				() =>
					getDocumentRuntimeDebug([completionDoc.uri.toString()]).then((entries) => {
						lastCompletionDebug = entries[0];
						return entries;
					}),
				(entries) => Boolean(entries[0]?.exists && entries[0]?.currentDocSemanticSnapshotReady),
				`layered runtime debug. last=${JSON.stringify(lastCompletionDebug)}`
			);

			const runtime = debug[0];
			assert.strictEqual(typeof runtime?.analysisFullFingerprint, 'string');
			assert.strictEqual(typeof runtime?.interactiveVisibilityFingerprint, 'string');
			assert.strictEqual(typeof runtime?.localStructuralSnapshotReady, 'boolean');
			assert.strictEqual(runtime?.globalContextReady, true);
			assert.strictEqual(runtime?.currentDocSemanticSnapshotReady, true);

			const semanticInvalidationEdit = new vscode.WorkspaceEdit();
			semanticInvalidationEdit.insert(completionDoc.uri, new vscode.Position(0, 0), ' ');
			assert.ok(
				await vscode.workspace.applyEdit(semanticInvalidationEdit),
				'Expected semantic invalidation edit to apply.'
			);
			completionDoc = await vscode.workspace.openTextDocument(completionDoc.uri);

			try {
				const semanticInvalidated = await getDocumentRuntimeDebug([completionDoc.uri.toString()]);
				const invalidatedCompletionRuntime = semanticInvalidated[0];
				assert.strictEqual(invalidatedCompletionRuntime?.exists, true);
				assert.strictEqual(invalidatedCompletionRuntime?.currentDocSemanticSnapshotReady, false);

				await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
							'vscode.executeCompletionItemProvider',
							completionDoc.uri,
							completionPosition.translate(0, 1)
						),
					(value) => {
						const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
						return labels.has('CompletionDocHelper') && labels.has('completionLocalColor');
					},
					'layered runtime completion rewarm'
				);

				const semanticRebuilt = await waitFor(
					() => getDocumentRuntimeDebug([completionDoc.uri.toString()]),
					(entries) => Boolean(entries[0]?.exists && entries[0]?.currentDocSemanticSnapshotReady),
					'layered runtime semantic rebuild'
				);
				assert.strictEqual(semanticRebuilt[0]?.currentDocSemanticSnapshotReady, true);
			} finally {
				const semanticRestoreEdit = new vscode.WorkspaceEdit();
				semanticRestoreEdit.delete(completionDoc.uri, new vscode.Range(0, 0, 0, 1));
				await vscode.workspace.applyEdit(semanticRestoreEdit);
				completionDoc = await vscode.workspace.openTextDocument(completionDoc.uri);
			}

			let diagnosticsDoc = await openFixture('module_diagnostics_missing_semicolon.nsf');
			await waitForDiagnostics(
				diagnosticsDoc,
				(diagnostics) => diagnostics.some((diag) => diag.message.includes('Missing semicolon.')),
				'layered runtime diagnostics publish'
			);
			const diagnosticsInvalidationEdit = new vscode.WorkspaceEdit();
			diagnosticsInvalidationEdit.insert(diagnosticsDoc.uri, new vscode.Position(0, 0), '(');
			assert.ok(
				await vscode.workspace.applyEdit(diagnosticsInvalidationEdit),
				'Expected diagnostics invalidation edit to apply.'
			);
			diagnosticsDoc = await vscode.workspace.openTextDocument(diagnosticsDoc.uri);

			try {
				const diagnosticsInvalidated = await getDocumentRuntimeDebug([diagnosticsDoc.uri.toString()]);
				const invalidatedDiagnosticsRuntime = diagnosticsInvalidated[0];
				assert.strictEqual(invalidatedDiagnosticsRuntime?.exists, true);
				assert.strictEqual(invalidatedDiagnosticsRuntime?.lastDiagnosticsPublishLayer, '');
				assert.strictEqual(invalidatedDiagnosticsRuntime?.localStructuralSnapshotReady, true);
			} finally {
				const diagnosticsRestoreEdit = new vscode.WorkspaceEdit();
				diagnosticsRestoreEdit.delete(diagnosticsDoc.uri, new vscode.Range(0, 0, 0, 1));
				await vscode.workspace.applyEdit(diagnosticsRestoreEdit);
				diagnosticsDoc = await vscode.workspace.openTextDocument(diagnosticsDoc.uri);
			}
		});

		it('treats active unit/include/macro state as a separate global context snapshot', async function () {
			this.timeout(120000);
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				let root = await openFixture('layered_runtime_macro_root.nsf');
				let shared = await openFixture('layered_runtime_macro_shared.ush');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());
				await waitForClientReady('client ready before global context debug');

				let lastGlobalContextDebug: unknown;
				const initialRuntime = await waitFor(
					() =>
						getDocumentRuntimeDebug([root.uri.toString(), shared.uri.toString()]).then((entries) => {
							lastGlobalContextDebug = entries;
							return entries;
						}),
					(entries) => entries[0]?.globalContextReady === true && entries[1]?.globalContextReady === true,
					`global context snapshot readiness. last=${JSON.stringify(lastGlobalContextDebug)}`
				);

				const initialSnapshotId = (initialRuntime[0] as any)?.globalContextSnapshotId;
				assert.ok(
					(initialSnapshotId ?? '').length > 0,
					`Expected initial root globalContextSnapshotId, got ${JSON.stringify(initialRuntime[0])}`
				);
				assert.ok(
					((initialRuntime[1] as any)?.globalContextSnapshotId ?? '').length > 0,
					`Expected initial shared globalContextSnapshotId, got ${JSON.stringify(initialRuntime[1])}`
				);
				assert.strictEqual(initialSnapshotId, (initialRuntime[1] as any)?.globalContextSnapshotId);
				assert.ok((initialRuntime[0]?.activeUnitIncludeClosureFingerprint ?? '').length > 0);
				assert.ok((initialRuntime[0]?.activeUnitBranchFingerprint ?? '').length > 0);
				const rootNeutralEditPosition = positionOf(
					root,
					'return layeredRuntimeSharedColor();',
					0,
					'return layeredRuntimeSharedColor();'.length
				);
				const sharedFollowupEditPosition = positionOf(
					shared,
					'return float4(1.0, 0.25, 0.0, 1.0);',
					0,
					'return float4(1.0, 0.25, 0.0, 1.0);'.length
				);

				try {
					const rootNeutralEdit = new vscode.WorkspaceEdit();
					rootNeutralEdit.insert(root.uri, rootNeutralEditPosition, ' ');
					assert.ok(
						await vscode.workspace.applyEdit(rootNeutralEdit),
						'Expected neutral active-unit edit to apply.'
					);
					root = await vscode.workspace.openTextDocument(root.uri);

					const sharedFollowupEdit = new vscode.WorkspaceEdit();
					sharedFollowupEdit.insert(shared.uri, sharedFollowupEditPosition, ' ');
					assert.ok(
						await vscode.workspace.applyEdit(sharedFollowupEdit),
						'Expected non-active follow-up edit to apply.'
					);
					shared = await vscode.workspace.openTextDocument(shared.uri);

					const runtime = await waitFor(
						() => getDocumentRuntimeDebug([root.uri.toString(), shared.uri.toString()]),
						(entries) => entries[0]?.globalContextReady === true && entries[1]?.globalContextReady === true,
						'global context snapshot reuse after neutral active-unit edit'
					);

					assert.ok(
						((runtime[1] as any)?.globalContextSnapshotId ?? '').length > 0,
						`Expected shared globalContextSnapshotId after reuse, got ${JSON.stringify(runtime[1])}`
					);
					assert.strictEqual(
						(runtime[0] as any)?.globalContextSnapshotId,
						initialSnapshotId,
						`Expected root to keep logical global context id after neutral edit, got ${JSON.stringify(runtime[0])}`
					);
					assert.strictEqual(
						(runtime[1] as any)?.globalContextSnapshotId,
						initialSnapshotId,
						`Expected shared to keep logical global context id after non-active follow-up edit, got ${JSON.stringify(runtime[1])}`
					);
					assert.strictEqual(
						(runtime[0] as any)?.globalContextSnapshotId,
						(runtime[1] as any)?.globalContextSnapshotId,
						`Expected shared global context identity after neutral active-unit edit, got root=${JSON.stringify(runtime[0])} shared=${JSON.stringify(runtime[1])}`
					);
				} finally {
					const sharedRestoreEdit = new vscode.WorkspaceEdit();
					sharedRestoreEdit.delete(
						shared.uri,
						new vscode.Range(sharedFollowupEditPosition, sharedFollowupEditPosition.translate(0, 1))
					);
					await vscode.workspace.applyEdit(sharedRestoreEdit);
					shared = await vscode.workspace.openTextDocument(shared.uri);

					const rootRestoreEdit = new vscode.WorkspaceEdit();
					rootRestoreEdit.delete(
						root.uri,
						new vscode.Range(rootNeutralEditPosition, rootNeutralEditPosition.translate(0, 1))
					);
					await vscode.workspace.applyEdit(rootRestoreEdit);
					root = await vscode.workspace.openTextDocument(root.uri);
				}
			});
		});

		it('marks local structural snapshot ready after changed-window structural analysis', async function () {
			this.timeout(120000);
			let document = await openFixture('module_diagnostics_missing_semicolon.nsf');

			await waitForDiagnostics(
				document,
				(value) => value.filter((diag) => diag.message.includes('Missing semicolon.')).length === 3,
				'initial structural diagnostics'
			);

			const editPosition = positionOf(document, 'float a = 1', 1, 'float a = 1'.length);
			const applyEdit = new vscode.WorkspaceEdit();
			applyEdit.insert(document.uri, editPosition, ';');
			assert.ok(await vscode.workspace.applyEdit(applyEdit), 'Expected changed-window structural edit to apply.');
			document = await vscode.workspace.openTextDocument(document.uri);

			try {
				await waitForDiagnostics(
					document,
					(value) => value.filter((diag) => diag.message.includes('Missing semicolon.')).length === 2,
					'changed-window structural diagnostics'
				);
				await waitForIndexingIdle('changed-window structural indexing idle');
				await waitForClientQuiescent('client quiescent before changed-window structural debug');
				await waitForClientReady('client ready before changed-window structural debug');

				let lastChangedWindowDebug: unknown;
				const [runtime] = await waitFor(
					() =>
						getDocumentRuntimeDebug([document.uri.toString()]).then((entries) => {
							lastChangedWindowDebug = entries[0];
							return entries;
						}),
					(entries) =>
						entries[0]?.localStructuralSnapshotReady === true &&
						entries[0]?.lastDiagnosticsPublishLayer === 'local-structural' &&
						(entries[0] as any)?.localStructuralChangedWindowOnly === true,
					`changed-window local structural runtime debug. last=${JSON.stringify(lastChangedWindowDebug)}`
				);
				assert.strictEqual(runtime?.localStructuralSnapshotReady, true);
				assert.strictEqual(runtime?.lastDiagnosticsPublishLayer, 'local-structural');
				assert.strictEqual((runtime as any)?.localStructuralChangedWindowOnly, true);
				assert.strictEqual(typeof (runtime as any)?.localStructuralChangedWindowStartLine, 'number');
				assert.strictEqual(typeof (runtime as any)?.localStructuralChangedWindowEndLine, 'number');
				assert.ok(
					((runtime as any)?.localStructuralChangedWindowStartLine ?? 0) <=
						((runtime as any)?.localStructuralChangedWindowEndLine ?? 0),
					`Expected a valid changed-window range, got ${JSON.stringify(runtime)}`
				);
			} finally {
				const restoreEdit = new vscode.WorkspaceEdit();
				restoreEdit.delete(document.uri, new vscode.Range(editPosition, editPosition.translate(0, 1)));
				await vscode.workspace.applyEdit(restoreEdit);
				await vscode.workspace.openTextDocument(document.uri);
			}
		});
	});
}
