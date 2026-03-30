import * as assert from 'assert';
import * as vscode from 'vscode';

import {
	getCompletionItems,
	hoverToText,
	openFixture,
	positionOf,
	repoDescribe,
	waitFor
} from '../test_helpers';

export function registerInteractiveRuntimeSignatureTests(): void {
	repoDescribe('NSF client integration: Interactive Runtime / Signature Help', () => {
	it('provides signature help with correct active parameter', async () => {
		const document = await openFixture('module_signature_help.nsf');
		const position = positionOf(document, 'SigTarget(uv, 2.0, 3', 1, 'SigTarget(uv, '.length);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('SigTarget'));
		assert.ok((help.signatures[0].parameters?.length ?? 0) >= 3);
	});

	it('provides signature help for overload benchmark fixture', async () => {
		const document = await openFixture('module_signature_help_overload_benchmark.nsf');
		const position = positionOf(document, 'OverloadBench(v3, 2.0, 1', 1, 'OverloadBench(v3, '.length);
		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'overload benchmark signature help'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('OverloadBench'));
		assert.ok((help.signatures[0].parameters?.length ?? 0) >= 3);
	});

	it('keeps hover and signature help aligned for overload-selected call sites', async () => {
		const document = await openFixture('module_signature_help_overload_benchmark.nsf');
		const position = positionOf(document, 'OverloadBench(v3, 2.0, 1)', 1, 'OverloadBench(v3, '.length);
		const hoverPosition = positionOf(document, 'OverloadBench(v3, 2.0, 1)', 1, 2);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'aligned overload signature help'
		);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'aligned overload hover'
		);

		const activeSignature = help.signatures[help.activeSignature];
		const hoverText = hoverToText(hovers);
		assert.ok(activeSignature.label.includes('OverloadBench'));
		assert.ok(hoverText.includes('OverloadBench('));
		assert.ok((activeSignature.parameters?.length ?? 0) >= 3);
	});

	it('keeps signature help available when overload resolver flag toggles', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const previousEnabled = configuration.inspect<boolean>('overloadResolver.enabled')?.workspaceValue;
		const document = await openFixture('module_signature_help_overload_benchmark.nsf');
		const position = positionOf(document, 'OverloadBench(v3, 2.0, 1', 1, 'OverloadBench(v3, '.length);
		try {
			await configuration.update('overloadResolver.enabled', false, vscode.ConfigurationTarget.Workspace);
			const helpDisabled = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SignatureHelp>(
						'vscode.executeSignatureHelpProvider',
						document.uri,
						position
					),
				(value) => Boolean(value) && value.signatures.length > 0,
				'signature help overload resolver disabled'
			);
			assert.ok(helpDisabled.signatures[0].label.includes('OverloadBench'));
			assert.ok((helpDisabled.signatures[0].parameters?.length ?? 0) >= 3);

			await configuration.update('overloadResolver.enabled', true, vscode.ConfigurationTarget.Workspace);
			const helpEnabled = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SignatureHelp>(
						'vscode.executeSignatureHelpProvider',
						document.uri,
						position
					),
				(value) => Boolean(value) && value.signatures.length > 0,
				'signature help overload resolver enabled'
			);
			assert.ok(helpEnabled.signatures[0].label.includes('OverloadBench'));
			assert.ok((helpEnabled.signatures[0].parameters?.length ?? 0) >= 3);
		} finally {
			await configuration.update(
				'overloadResolver.enabled',
				previousEnabled,
				vscode.ConfigurationTarget.Workspace
			);
		}
	});

	it('provides signature help for HLSL built-in functions', async () => {
		const document = await openFixture('module_signature_help_builtin.nsf');
		const text = document.getText();
		const callPos = text.indexOf('lerp(');
		assert.ok(callPos >= 0);

		const position = document.positionAt(callPos + 'lerp('.length);
		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 0);
		assert.ok(help.signatures[0].label.includes('lerp'));
		assert.ok((help.signatures[0].parameters?.length ?? 0) >= 3);
	});

	it('provides signature help for member calls with spaced dot access', async () => {
		const document = await openFixture('module_signature_help_member_call_spaced.nsf');
		const position = positionOf(
			document,
			'gTex . Sample(gSampler, uv)',
			1,
			'gTex . Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for spaced member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
		assert.ok((help.signatures[0].parameters?.length ?? 0) >= 2);
	});

	it('provides signature help for indexed member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'gTexArr[0].Sample(gSampler, uv)',
			1,
			'gTexArr[0].Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for indexed member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('provides Texture2DArray Sample signature help with float3 coordinates', async () => {
		const document = await openFixture('module_texture2d_array_methods.nsf');
		const position = positionOf(
			document,
			'gTexArray.Sample(gSampler, uvw)',
			1,
			'gTexArray.Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for Texture2DArray Sample'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
		assert.ok(help.signatures[0].label.includes('float3 uv'));
	});

	it('provides Texture2DArray Load signature help with int4 coordinates', async () => {
		const document = await openFixture('module_texture2d_array_methods.nsf');
		const position = positionOf(
			document,
			'gTexArray.Load(int4(0, 0, 0, 0))',
			1,
			'gTexArray.Load('.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for Texture2DArray Load'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 0);
		assert.ok(help.signatures[0].label.includes('Load'));
		assert.ok(help.signatures[0].label.includes('int4 location'));
	});

	it('provides signature help for parenthesized member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'(gTex).Sample(gSampler, uv)',
			1,
			'(gTex).Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for parenthesized member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('provides signature help for parenthesized indexed member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'(gTexArr[0]).Sample(gSampler, uv)',
			1,
			'(gTexArr[0]).Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for parenthesized indexed member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('provides signature help for macro wrapped member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'GET_TEX(gTex).Sample(gSampler, uv)',
			1,
			'GET_TEX(gTex).Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for macro wrapped member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('provides signature help for nested macro argument member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'WRAP_TEX(gTexArr[0]).Sample(gSampler, uv)',
			1,
			'WRAP_TEX(gTexArr[0]).Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for nested macro member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('provides signature help for multi-parenthesized indexed member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'((gTexArr[0])).Sample(gSampler, uv)',
			1,
			'((gTexArr[0])).Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for multi-parenthesized indexed member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('shows hover info for complex member-call method sites', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const hoverPosition = positionOf(document, '((gTexArr[0])).Sample', 1, '((gTexArr[0])).'.length + 1);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover for complex member call method'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('(HLSL built-in method)'));
		assert.ok(hoverText.includes('Sample('));
	});

	it('keeps member hover and completion aligned for complex base expressions', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const completionPosition = positionOf(document, '((gTexArr[0])).Sample', 1, '((gTexArr[0])).'.length);
		const hoverPosition = positionOf(document, '((gTexArr[0])).Sample', 1, '((gTexArr[0])).'.length + 1);

		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'aligned member completion'
		);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'aligned member hover'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		const hoverText = hoverToText(hovers);
		assert.ok(labels.has('Sample'));
		assert.ok(hoverText.includes('Sample('));
	});

	it('provides completion items for complex member-call base expressions', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');

		const firstPosition = positionOf(document, '((gTexArr[0])).Sample', 1, '((gTexArr[0])).'.length);
		const firstResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					firstPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'completion for multi-parenthesized member base'
		);
		const firstLabels = new Set(getCompletionItems(firstResult).map((item) => item.label.toString()));
		assert.ok(firstLabels.has('Sample'));

		const secondPosition = positionOf(document, 'WRAP_TEX(gTexArr[0]).Sample', 1, 'WRAP_TEX(gTexArr[0]).'.length);
		const secondResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					secondPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'completion for macro-wrapped member base'
		);
		const secondLabels = new Set(getCompletionItems(secondResult).map((item) => item.label.toString()));
		assert.ok(secondLabels.has('Sample'));
	});

	});
}

