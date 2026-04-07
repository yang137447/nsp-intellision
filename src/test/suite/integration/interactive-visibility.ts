import * as assert from 'assert';
import * as vscode from 'vscode';

import {
	getCompletionItems,
	getInteractiveRuntimeDebug,
	openFixture,
	positionOf,
	repoDescribe,
	waitFor
} from '../test_helpers';

export function registerInteractiveVisibilityTests(): void {
	repoDescribe('NSF client integration: Interactive Visibility', () => {
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
