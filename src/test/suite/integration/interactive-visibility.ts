import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	getCompletionItems,
	getDocumentRuntimeDebug,
	getWorkspaceRoot,
	hoverToText,
	getInteractiveRuntimeDebug,
	openFixture,
	positionOf,
	repoDescribe,
	resolveMemberBaseForTests,
	waitForActiveUnitAndVisibilityReadyForTests,
	withTemporaryIntellisionPath,
	waitForCompletionLabels,
	waitForClientReady,
	waitForHoverText,
	waitFor
} from '../test_helpers';

function assertCurrentOrSharedVisibleLayer(layer: string | undefined, context: string): void {
	assert.ok(
		layer === 'current' || layer === 'shared-visible',
		`Expected ${context} from current or shared-visible, got ${layer}`
	);
}

export function registerInteractiveVisibilityTests(): void {
	repoDescribe('NSF client integration: Interactive Visibility', () => {
		it('tracks an interactive visibility key alongside the analysis key', async function () {
			this.timeout(90000);

			const document = await openFixture('module_completion_current_doc.nsf');
			await waitForClientReady();

			const runtimeEntries = (await getDocumentRuntimeDebug([
				document.uri.toString()
			])) as Array<{ interactiveVisibilityFingerprint?: string }>;
			const runtime = runtimeEntries[0];
			assert.ok(runtime?.interactiveVisibilityFingerprint);
			assert.ok((runtime?.interactiveVisibilityFingerprint ?? '').length > 0);
		});

		it('reports completion resolution as current layer', async function () {
			this.timeout(90000);

			const document = await openFixture('module_completion_current_doc.nsf');
			const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
						'vscode.executeCompletionItemProvider',
						document.uri,
						completionPosition
					),
				(value) => getCompletionItems(value).some((item) => item.label.toString() === 'CompletionDocHelper'),
				'current-doc completion before runtime debug'
			);

			const debug = await getInteractiveRuntimeDebug(document.uri.toString());
			assert.strictEqual(debug.lastQueryKind, 'completion');
			assert.strictEqual(debug.lastResolvedLayer, 'current');
		});

		it('serves current-context-visible cross-file function symbols from the active unit include closure', async function () {
			this.timeout(120000);

			await waitForClientReady();
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				const root = await openFixture('visibility_root.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());
				await waitForActiveUnitAndVisibilityReadyForTests(
					root.uri.toString(),
					'visibility_root.nsf',
					'visibility_root active unit + interactive visibility settled'
				);

				const position = positionOf(root, 'VisibleInc', 1, 'VisibleInc'.length);
				const items = await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
							'vscode.executeCompletionItemProvider',
							root.uri,
							position
						),
					(value) => getCompletionItems(value).some((item) => item.label.toString() === 'VisibleIncludeHelper'),
					'cross-file visible completion'
				);

				assert.ok(getCompletionItems(items).some((item) => item.label.toString() === 'VisibleIncludeHelper'));
				const debug = await getInteractiveRuntimeDebug(root.uri.toString());
				assert.strictEqual(debug.lastQueryKind, 'completion');
				assert.ok(
					['current', 'shared-visible', 'workspace'].includes(debug.lastResolvedLayer ?? ''),
					`Expected current-context-visible completion to resolve through a known layer, got ${debug.lastResolvedLayer}`
				);
			});
		});

		it('prewarms current-context-visible cross-file global symbols from the active unit include closure', async function () {
			this.timeout(120000);

			await waitForClientReady();
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				const root = await openFixture('visibility_globals_root.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());
				await waitForActiveUnitAndVisibilityReadyForTests(
					root.uri.toString(),
					'visibility_globals_root.nsf',
					'visibility_globals_root active unit + interactive visibility settled'
				);

				const position = positionOf(root, 'VisibleSharedGlo', 1, 'VisibleSharedGlo'.length);
				await waitForCompletionLabels(
					root,
					position,
					['VisibleSharedGlobalColor'],
					'cross-file visible global completion'
				);

				const debug = await waitFor(
					() => getInteractiveRuntimeDebug(root.uri.toString()),
					(value) => value.lastQueryKind === 'completion',
					'shared-visible global completion debug'
				);
				assert.ok(
					['current', 'shared-visible', 'workspace'].includes(debug.lastResolvedLayer ?? ''),
					`Expected global completion to resolve through a known layer, got ${debug.lastResolvedLayer}`
				);
			});
		});

		it('keeps current-doc local completion ahead of shared-visible include helper', async function () {
			this.timeout(120000);

			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				const root = await openFixture('visibility_root.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());
				await waitForActiveUnitAndVisibilityReadyForTests(
					root.uri.toString(),
					'visibility_root.nsf',
					'visibility_root active unit + interactive visibility settled (local completion ordering)'
				);
				const edit = new vscode.WorkspaceEdit();
				edit.insert(
					root.uri,
					positionOf(root, 'return VisibleInc', 1, 0),
					'    float4 visibilityLocalColor = float4(1.0, 0.0, 0.0, 1.0);\n'
				);
				await vscode.workspace.applyEdit(edit);

				const position = positionOf(root, 'return Vis', 1, 'return Vis'.length);
				const result = await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
							'vscode.executeCompletionItemProvider',
							root.uri,
							position
						),
					(value) => {
						const labels = getCompletionItems(value).map((item) => item.label.toString());
						return labels.includes('VisibleIncludeHelper') && labels.includes('visibilityLocalColor');
					},
					'ordered current/shared-visible completion'
				);

				const labels = getCompletionItems(result).map((item) => item.label.toString());
				assert.ok(labels.indexOf('visibilityLocalColor') >= 0);
				assert.ok(labels.indexOf('VisibleIncludeHelper') >= 0);
				assert.ok(labels.indexOf('VisibleIncludeHelper') > labels.indexOf('visibilityLocalColor'));
			});
		});

		it('uses shared-visible cross-file type information for member completion, hover, and signature help', async function () {
			this.timeout(120000);

			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				const root = await openFixture('visibility_member_root.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());
				await waitForActiveUnitAndVisibilityReadyForTests(
					root.uri.toString(),
					'visibility_member_root.nsf',
					'visibility_member_root active unit + interactive visibility settled'
				);

				const memberPos = positionOf(root, 'visibleStruct.', 1, 'visibleStruct.'.length);
				const memberItems = await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
							'vscode.executeCompletionItemProvider',
							root.uri,
							memberPos
						),
					(value) => getCompletionItems(value).some((item) => item.label.toString() === 'SharedVisibleField'),
					'shared-visible member completion'
				);
				assert.ok(getCompletionItems(memberItems).some((item) => item.label.toString() === 'SharedVisibleField'));
				const memberDebug = await waitFor(
					() => getInteractiveRuntimeDebug(root.uri.toString()),
					(value) => value.lastQueryKind === 'completion',
					'shared-visible member completion debug'
				);
				assertCurrentOrSharedVisibleLayer(memberDebug.lastResolvedLayer, 'shared-visible member completion');

				const hoverPos = positionOf(root, 'SharedVisibleHelper', 1, 2);
				const hoverText = hoverToText(
					await waitForHoverText(
						root,
						hoverPos,
						(text) => text.includes('SharedVisibleHelper') && text.includes('float4'),
						'shared-visible hover'
					)
				);
				assert.ok(hoverText.includes('SharedVisibleHelper'));
				const hoverDebug = await waitFor(
					() => getInteractiveRuntimeDebug(root.uri.toString()),
					(value) => value.lastQueryKind === 'hover',
					'shared-visible hover debug'
				);
				assertCurrentOrSharedVisibleLayer(hoverDebug.lastResolvedLayer, 'shared-visible hover');

				const sigPos = positionOf(root, 'SharedVisibleHelper(', 1, 'SharedVisibleHelper('.length);
				const sig = await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.SignatureHelp>(
							'vscode.executeSignatureHelpProvider',
							root.uri,
							sigPos
						),
					(value) => Boolean(value) && (value?.signatures.length ?? 0) > 0,
					'shared-visible signature help'
				);
				assert.ok(sig.signatures.some((item) => item.label.includes('SharedVisibleHelper')));
			});
		});

		it('reports include_closure_decl when member-base resolution comes from the active unit include closure', async function () {
			this.timeout(120000);

			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				const root = await openFixture('visibility_member_closure_root.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());
				const fragment = await openFixture('visibility_member_closure_fragment.hlsl');
				await waitFor(
					() => getDocumentRuntimeDebug([fragment.uri.toString()]),
					(value) =>
						Array.isArray(value) &&
						value[0]?.activeUnitPath?.toLowerCase().endsWith('visibility_member_closure_root.nsf'),
					'fragment runtime bound to closure root active unit'
				);

				const resolution = await resolveMemberBaseForTests(
					fragment.uri.toString(),
					positionOf(fragment, 'SharedVisibleField', 1, 1).line,
					positionOf(fragment, 'SharedVisibleField', 1, 1).character
				);
				assert.strictEqual(resolution.resolved, true, JSON.stringify(resolution));
				assert.strictEqual(resolution.base, 'closureValue');
				assert.strictEqual(resolution.typeName, 'SharedVisibleStruct');
				assert.strictEqual(resolution.resolutionPath, 'include_closure_decl');

				const debug = await getInteractiveRuntimeDebug(fragment.uri.toString());
				assert.strictEqual(debug.lastMemberBaseSymbol, 'closureValue', JSON.stringify(debug));
				assert.strictEqual(
					debug.lastMemberBaseResolutionPath,
					'include_closure_decl',
					JSON.stringify(debug)
				);
			});
		});

		it('keeps the shared-visible shard after closing an unrelated document', async function () {
			this.timeout(120000);

			await waitForClientReady();
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				const root = await openFixture('visibility_root.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());
				await waitForActiveUnitAndVisibilityReadyForTests(
					root.uri.toString(),
					'visibility_root.nsf',
					'visibility_root active unit + interactive visibility settled (shard close)'
				);

				const position = positionOf(root, 'VisibleInc', 1, 'VisibleInc'.length);
				await waitForCompletionLabels(
					root,
					position,
					['VisibleIncludeHelper'],
					'initial shared-visible completion'
				);

				let debug = await getInteractiveRuntimeDebug(root.uri.toString());
				assert.strictEqual(debug.lastQueryKind, 'completion');

				await openFixture('module_completion_current_doc.nsf');
				await vscode.commands.executeCommand('workbench.action.closeActiveEditor');
				await vscode.window.showTextDocument(root, { preview: false });

				const items = await waitForCompletionLabels(
					root,
					position,
					['VisibleIncludeHelper'],
					'shared-visible completion after unrelated close'
				);

				assert.ok(getCompletionItems(items).some((item) => item.label.toString() === 'VisibleIncludeHelper'));
				debug = await getInteractiveRuntimeDebug(root.uri.toString());
				assert.strictEqual(debug.lastQueryKind, 'completion');
			});
		});
	});
}
