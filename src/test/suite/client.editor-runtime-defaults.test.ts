import * as assert from 'assert';
import * as vscode from 'vscode';

import { openFixture, repoDescribe } from './test_helpers';

type QuickSuggestionsRuntimeValue = boolean | string | undefined;

function assertQuickSuggestionsEnabledForCode(value: QuickSuggestionsRuntimeValue): void {
	assert.ok(
		value === true || value === 'on' || value === 'offWhenInlineCompletions',
		`Expected editor.quickSuggestions.other to keep code suggestions enabled, got ${JSON.stringify(value)}.`
	);
}

function assertQuickSuggestionsDisabled(value: QuickSuggestionsRuntimeValue, settingName: string): void {
	assert.ok(
		value === false || value === 'off',
		`Expected ${settingName} to be disabled, got ${JSON.stringify(value)}.`
	);
}

function assertEditorDefaultsApplied(document: vscode.TextDocument): void {
	const editorConfig = vscode.workspace.getConfiguration('editor', document.uri);
	const quickSuggestions = editorConfig.get<{
		other?: QuickSuggestionsRuntimeValue;
		comments?: QuickSuggestionsRuntimeValue;
		strings?: QuickSuggestionsRuntimeValue;
	}>('quickSuggestions');
	const suggestOnTriggerCharacters = editorConfig.get<boolean>('suggestOnTriggerCharacters');
	const parameterHintsEnabled = editorConfig.get<boolean>('parameterHints.enabled');

	assertQuickSuggestionsEnabledForCode(quickSuggestions?.other);
	assertQuickSuggestionsDisabled(quickSuggestions?.comments, 'editor.quickSuggestions.comments');
	assertQuickSuggestionsDisabled(quickSuggestions?.strings, 'editor.quickSuggestions.strings');
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
