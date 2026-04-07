import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	type ProviderLocation,
	getWorkspaceRoot,
	hoverToText,
	openFixture,
	positionOf,
	repoDescribe,
	toFsPath,
	waitFor,
	waitForHoverText,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from './test_helpers';

repoDescribe('NSF client integration: Current Unit Include-Chain Helper Calls', () => {
	it('keeps hover, definition, and signature help aligned for current-unit include-chain helper calls', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
			await waitForIndexingIdle('indexing idle for current-unit include-chain helper call');
			const document = await openFixture('include_context/units/multi_context_parameter_a.nsf');
			const hoverPosition = positionOf(document, 'MultiContextParameterEntry', 1, 2);
			const signaturePosition = positionOf(
				document,
				'MultiContextParameterEntry(color, half4(1.0, 0.0, 0.0, 1.0))',
				1,
				'MultiContextParameterEntry(color, '.length
			);

			const definitions = await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						document.uri,
						hoverPosition
					),
				(value) =>
					Array.isArray(value) &&
					value.some(
						(location) => path.basename(toFsPath(location)) === 'multi_context_parameter_common.hlsl'
					),
				'definition for current-unit include-chain helper call'
			);
			const definitionFile = path.basename(toFsPath(definitions[0]));

			const hovers = await waitForHoverText(
				document,
				hoverPosition,
				(text) =>
					text.includes('half3 MultiContextParameterEntry(half3 color, half4 fog_factors)') &&
					text.includes(definitionFile) &&
					!text.includes('Candidate definitions') &&
					!text.includes('Include context ambiguous'),
				'hover for current-unit include-chain helper call'
			);
			const hoverText = hoverToText(hovers);
			assert.ok(hoverText.includes('Parameters:'), hoverText);
			assert.ok(hoverText.includes(definitionFile), hoverText);

			const help = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SignatureHelp>(
						'vscode.executeSignatureHelpProvider',
						document.uri,
						signaturePosition
					),
				(value) =>
					Boolean(value) &&
					value.signatures.length > 0 &&
					value.signatures.some((signature) =>
						signature.label.includes('half3 MultiContextParameterEntry(half3 color, half4 fog_factors)')
					),
				'signature help for current-unit include-chain helper call'
			);
			assert.strictEqual(help.activeSignature, 0);
			assert.strictEqual(help.activeParameter, 1);
			assert.ok(
				help.signatures[0].label.includes(
					'half3 MultiContextParameterEntry(half3 color, half4 fog_factors)'
				),
				help.signatures[0].label
			);
		});
	});

	it('surfaces ambiguity when current-unit include closure contains same-name helper definitions', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
			await waitForIndexingIdle('indexing idle for ambiguous current-unit helper call');
			const document = await openFixture('include_context/units/multi_context_helper_conflict.nsf');
			const helperPosition = positionOf(document, 'MultiContextHelperConflict', 2, 2);
			const signaturePosition = positionOf(
				document,
				'MultiContextHelperConflict(color, half4(1.0, 0.5, 0.25, 1.0))',
				1,
				'MultiContextHelperConflict(color, '.length
			);

			const definitions = await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						document.uri,
						helperPosition
					),
				(value) => Array.isArray(value) && value.length > 0,
				'definition for ambiguous current-unit helper call'
			);
			const definitionFiles = new Set(definitions.map((location) => path.basename(toFsPath(location))));
			assert.ok(
				definitionFiles.has('multi_context_helper_conflict_a.hlsl'),
				Array.from(definitionFiles).join(', ')
			);
			assert.ok(
				definitionFiles.has('multi_context_helper_conflict_b.hlsl'),
				Array.from(definitionFiles).join(', ')
			);

			const hovers = await waitForHoverText(
				document,
				helperPosition,
				(text) =>
					text.includes('Current-unit include closure ambiguous') &&
					text.includes('Candidate definitions') &&
					text.includes('multi_context_helper_conflict_a.hlsl') &&
					text.includes('multi_context_helper_conflict_b.hlsl'),
				'hover for ambiguous current-unit helper call'
			);
			const hoverText = hoverToText(hovers);
			assert.ok(hoverText.includes('Current-unit include closure ambiguous'), hoverText);

			const help = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SignatureHelp>(
						'vscode.executeSignatureHelpProvider',
						document.uri,
						signaturePosition
					),
				(value) => Boolean(value) && (value?.signatures.length ?? 0) >= 2,
				'signature help for ambiguous current-unit helper call'
			);
			const documentationText = help.signatures
				.map((signature) => {
					const documentation = signature.documentation as any;
					if (!documentation) {
						return '';
					}
					if (typeof documentation === 'string') {
						return documentation;
					}
					if (typeof documentation.value === 'string') {
						return documentation.value;
					}
					return '';
				})
				.join('\n');
			assert.ok(documentationText.includes('Current-unit include closure ambiguous'), documentationText);
			assert.ok(documentationText.includes('multi_context_helper_conflict_a.hlsl'), documentationText);
			assert.ok(documentationText.includes('multi_context_helper_conflict_b.hlsl'), documentationText);
		});
	});
});
