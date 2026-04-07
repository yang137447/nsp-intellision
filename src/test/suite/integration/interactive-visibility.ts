import * as assert from 'assert';
import * as vscode from 'vscode';

import {
	getCompletionItems,
	getDocumentRuntimeDebug,
	getInteractiveRuntimeDebug,
	openFixture,
	positionOf,
	repoDescribe,
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
	});
}
