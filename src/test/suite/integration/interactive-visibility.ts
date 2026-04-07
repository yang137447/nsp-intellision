import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	getCompletionItems,
	getDocumentRuntimeDebug,
	getWorkspaceRoot,
	getInteractiveRuntimeDebug,
	openFixture,
	positionOf,
	repoDescribe,
	withTemporaryIntellisionPath,
	waitForCompletionLabels,
	waitForClientReady,
	waitFor
} from '../test_helpers';

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

		it('prewarms current-context-visible cross-file function symbols from the active unit include closure', async function () {
			this.timeout(120000);

			await waitForClientReady();
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				const root = await openFixture('visibility_root.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());

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
				assert.strictEqual(debug.lastResolvedLayer, 'shared-visible');
			});
		});

		it('keeps the shared-visible shard after closing an unrelated document', async function () {
			this.timeout(120000);

			await waitForClientReady();
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				const root = await openFixture('visibility_root.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());

				const position = positionOf(root, 'VisibleInc', 1, 'VisibleInc'.length);
				await waitForCompletionLabels(
					root,
					position,
					['VisibleIncludeHelper'],
					'initial shared-visible completion'
				);

				let debug = await getInteractiveRuntimeDebug(root.uri.toString());
				assert.strictEqual(debug.lastResolvedLayer, 'shared-visible');

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
				assert.strictEqual(debug.lastResolvedLayer, 'shared-visible');
			});
		});
	});
}
