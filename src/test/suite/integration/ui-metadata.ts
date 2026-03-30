import * as assert from 'assert';
import * as vscode from 'vscode';

import {
	type ProviderLocation,
	hoverToText,
	openFixture,
	positionOf,
	repoDescribe,
	waitFor
} from '../test_helpers';

export function registerInteractiveUiMetadataTests(): void {
	repoDescribe('NSF client integration: Interactive Runtime / UI Metadata', () => {
	it('smoke: supports UI metadata block variable declarations', async () => {
		const document = await openFixture('module_sas_ui_decl.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'ui meta diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: u_b_max_random_distance.'));
		assert.ok(!messages.includes('Undefined identifier: SasUiLabel.'));
		assert.ok(!messages.includes('Duplicate global declaration: SasUiMax.'));
		assert.ok(!messages.includes('Duplicate global declaration: SasUiMin.'));

		const usagePosition = positionOf(document, 'u_b_max_random_distance', 2, 2);
		const definitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					usagePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'ui meta definition'
		);

		const expectedDeclPos = positionOf(document, 'u_b_max_random_distance', 1, 0);
		const definition = definitions[0];
		const definitionUri = 'uri' in definition ? definition.uri : definition.targetUri;
		const definitionRange = 'range' in definition ? definition.range : definition.targetRange;
		assert.strictEqual(definitionUri.toString(), document.uri.toString());
		assert.strictEqual(definitionRange.start.line, expectedDeclPos.line);

		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					usagePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'ui meta hover'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('Type: float'), hoverText);
	});

	it('supports labeled metadata block variable declarations consistently', async () => {
		const document = await openFixture('module_metadata_decl_with_label.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'labeled ui meta diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: u_uv_info.'));

		const usagePosition = positionOf(document, 'u_uv_info', 2, 2);
		const definitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					usagePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'labeled ui meta definition'
		);

		const expectedDeclPos = positionOf(document, 'u_uv_info', 1, 0);
		const definition = definitions[0];
		const definitionUri = 'uri' in definition ? definition.uri : definition.targetUri;
		const definitionRange = 'range' in definition ? definition.range : definition.targetRange;
		assert.strictEqual(definitionUri.toString(), document.uri.toString());
		assert.strictEqual(definitionRange.start.line, expectedDeclPos.line);

		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					usagePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'labeled ui meta hover'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('Type: float4'), hoverText);
	});

	});
}
