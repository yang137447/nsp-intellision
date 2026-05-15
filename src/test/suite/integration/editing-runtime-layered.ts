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
	waitForDiagnosticsWithTouches,
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
				const localStructuralReady = await waitFor(
					() => getDocumentRuntimeDebug([diagnosticsDoc.uri.toString()]),
					(entries) => entries[0]?.localStructuralSnapshotReady === true,
					'layered runtime local structural async rebuild'
				);
				assert.strictEqual(localStructuralReady[0]?.localStructuralSnapshotReady, true);
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
				let shared = await openFixture('layered_runtime_macro_shared.hlsl');
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
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
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
				await waitForClientReady('client ready before changed-window structural debug');

				let lastChangedWindowDebug: unknown;
				let runtime = undefined as Awaited<ReturnType<typeof getDocumentRuntimeDebug>>[number] | undefined;
				for (let attempt = 0; attempt < 60; attempt++) {
					const entries = await getDocumentRuntimeDebug([document.uri.toString()]);
					const internalStatus = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
					lastChangedWindowDebug = { entries, internalStatus };
					runtime = entries[0];
					if (
						runtime?.localStructuralSnapshotReady === true &&
						runtime?.localStructuralChangedWindowOnly === true
					) {
						break;
					}
					await new Promise((resolve) => setTimeout(resolve, 500));
				}
				assert.ok(
					runtime?.localStructuralSnapshotReady === true &&
						runtime?.localStructuralChangedWindowOnly === true,
					`Timed out waiting for changed-window local structural runtime debug. last=${JSON.stringify(lastChangedWindowDebug)}`
				);
				assert.strictEqual(runtime?.localStructuralSnapshotReady, true);
				assert.strictEqual(runtime?.localStructuralChangedWindowOnly, true);
				assert.strictEqual(typeof runtime?.localStructuralChangedWindowStartLine, 'number');
				assert.strictEqual(typeof runtime?.localStructuralChangedWindowEndLine, 'number');
				assert.ok(
					(runtime?.localStructuralChangedWindowStartLine ?? 0) <=
						(runtime?.localStructuralChangedWindowEndLine ?? 0),
					`Expected a valid changed-window range, got ${JSON.stringify(runtime)}`
				);
			} finally {
				const restoreEdit = new vscode.WorkspaceEdit();
				restoreEdit.delete(document.uri, new vscode.Range(editPosition, editPosition.translate(0, 1)));
				await vscode.workspace.applyEdit(restoreEdit);
				await vscode.workspace.openTextDocument(document.uri);
			}
		});

		it('publishes macro-sensitive diagnostics through the global-context layer once context is ready', async function () {
			this.timeout(120000);
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				const rootPath = path.join(getWorkspaceRoot(), 'test_files', 'layered_runtime_macro_root.nsf');
				const rootUri = vscode.Uri.file(rootPath);
				const transientWrongPublishes: string[] = [];
				const observerTasks: Promise<void>[] = [];
				const diagnosticsSubscription = vscode.languages.onDidChangeDiagnostics((event) => {
					if (!event.uris.some((uri) => uri.toString() === rootUri.toString())) {
						return;
					}
					const currentDiagnostics = vscode.languages.getDiagnostics(rootUri);
					const messages = currentDiagnostics.map((diag) => diag.message).join('\n');
					const sawWrongPublish =
						messages.includes('Undefined macro in preprocessor expression: LAYERED_RUNTIME_SHARED_BRANCH.') ||
						messages.includes('Undefined identifier: layeredRuntimeSharedColor.');
					if (!sawWrongPublish) {
						return;
					}
					observerTasks.push(
						getDocumentRuntimeDebug([rootUri.toString()]).then(([runtime]) => {
							if (runtime?.globalContextReady === true) {
								return;
							}
							transientWrongPublishes.push(
								`${runtime?.globalContextReady ?? 'unknown'}:${messages}`
							);
						})
					);
				});
				try {
					const root = await openFixture('layered_runtime_macro_root.nsf');
					await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());

					await waitForDiagnosticsWithTouches(
						root,
						(value) => {
							const messages = value.map((diag) => diag.message).join('\n');
							return (
								!messages.includes('Undefined macro in preprocessor expression: LAYERED_RUNTIME_SHARED_BRANCH.') &&
								!messages.includes('Undefined identifier: layeredRuntimeSharedColor.')
							);
						},
						'layered macro diagnostics settled'
					);

					const [runtime] = await waitFor(
						() => getDocumentRuntimeDebug([root.uri.toString()]),
						(entries) =>
							entries[0]?.globalContextReady === true &&
							(entries[0]?.lastDiagnosticsPublishLayer ?? '').length > 0,
						'layered macro diagnostics publish layer'
					);
					assert.strictEqual(
						runtime?.lastDiagnosticsPublishLayer,
						'global-context',
						`Expected macro-sensitive diagnostics to publish through global-context, got ${JSON.stringify(runtime)}`
					);
					await waitForClientQuiescent('layered macro diagnostics continuity settled');
					await Promise.all(observerTasks);
					assert.deepStrictEqual(
						transientWrongPublishes,
						[],
						`Expected no transient macro-sensitive wrong publish before global context readiness, got ${transientWrongPublishes.join('\n')}`
					);
				} finally {
					diagnosticsSubscription.dispose();
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
				}
			});
		});

		it('keeps current-doc semantic answers available before global context sensitive diagnostics settle', async function () {
			this.timeout(120000);
			let document = await openFixture('module_completion_current_doc.nsf');
			const position = positionOf(document, 'return Comp', 1, 'return Comp'.length);

			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
						'vscode.executeCompletionItemProvider',
						document.uri,
						position
					),
				(value) => getCompletionItems(value).some((item) => item.label.toString() === 'CompletionDocHelper'),
				'current-doc semantic completion'
			);
			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokens>(
						'vscode.provideDocumentSemanticTokens',
						document.uri
					),
				(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
				'seed deferred semantic snapshot before invalidation'
			);
			await waitFor(
				() => getDocumentRuntimeDebug([document.uri.toString()]),
				(entries) => entries[0]?.deferredHasSemanticSnapshot === true,
				'deferred semantic snapshot seeded before invalidation'
			);

			const invalidateEdit = new vscode.WorkspaceEdit();
			invalidateEdit.insert(document.uri, new vscode.Position(0, 0), ' ');
			assert.ok(await vscode.workspace.applyEdit(invalidateEdit), 'Expected semantic invalidation edit to apply.');
			document = await vscode.workspace.openTextDocument(document.uri);

			try {
				const [invalidatedRuntime] = await getDocumentRuntimeDebug([document.uri.toString()]);
				assert.strictEqual(invalidatedRuntime?.currentDocSemanticSnapshotReady, false);

				await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.SemanticTokens>(
							'vscode.provideDocumentSemanticTokens',
							document.uri
						),
					(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
					'deferred-only semantic tokens before current-doc semantic publish'
				);

				const [deferredOnlyRuntime] = await waitFor(
					() => getDocumentRuntimeDebug([document.uri.toString()]),
					(entries) =>
						entries[0]?.deferredHasSemanticSnapshot === true &&
						entries[0]?.currentDocSemanticSnapshotReady === false,
					'deferred semantic current before explicit current-doc semantic publish'
				);
				assert.strictEqual(deferredOnlyRuntime?.deferredHasSemanticSnapshot, true);
				assert.strictEqual(deferredOnlyRuntime?.currentDocSemanticSnapshotReady, false);

				const items = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					position.translate(0, 1)
				);
				assert.ok(
					getCompletionItems(items).some((item) => item.label.toString() === 'CompletionDocHelper'),
					'Expected current-doc semantic completion to stay available immediately after invalidation.'
				);

				const [runtimeAfterCompletion] = await getDocumentRuntimeDebug([document.uri.toString()]);
				assert.strictEqual(runtimeAfterCompletion?.currentDocSemanticSnapshotReady, true);
			} finally {
				const restoreEdit = new vscode.WorkspaceEdit();
				restoreEdit.delete(document.uri, new vscode.Range(0, 0, 0, 1));
				await vscode.workspace.applyEdit(restoreEdit);
				await vscode.workspace.openTextDocument(document.uri);
			}
		});
	});
}
