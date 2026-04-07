import * as assert from 'assert';
import * as vscode from 'vscode';

import { openFixture, repoDescribe } from './test_helpers';

function assertEditorDefaultsApplied(document: vscode.TextDocument): void {
	const editorConfig = vscode.workspace.getConfiguration('editor', document.uri);
	const quickSuggestions = editorConfig.get<{ other?: string; comments?: string; strings?: string }>(
		'quickSuggestions'
	);
	const suggestOnTriggerCharacters = editorConfig.get<boolean>('suggestOnTriggerCharacters');
	const parameterHintsEnabled = editorConfig.get<boolean>('parameterHints.enabled');

	assert.deepStrictEqual(quickSuggestions, {
		other: 'on',
		comments: 'off',
		strings: 'off'
	});
	assert.strictEqual(suggestOnTriggerCharacters, true);
	assert.strictEqual(parameterHintsEnabled, true);
}

repoDescribe('NSF client integration: Editor Runtime Defaults', () => {
	it('applies editor defaults to nsf documents at runtime', async () => {
		const document = await openFixture('module_completion_current_doc.nsf');
		assertEditorDefaultsApplied(document);
	});

	it('applies editor defaults to hlsl documents at runtime', async () => {
		const document = await openFixture('include_target.hlsl');
		assertEditorDefaultsApplied(document);
	});
});
