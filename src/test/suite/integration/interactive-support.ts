import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	type ProviderLocation,
	getCompletionItems,
	getWorkspaceRoot,
	openFixture,
	positionOf,
	repoDescribe,
	toFsPath,
	toRange,
	waitFor,
	waitForCompletionLabels,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from '../test_helpers';

export function registerInteractiveStructMemberCompletionTests(): void {
	repoDescribe('NSF client integration: Interactive Runtime / Member Completion', () => {
	it('provides struct member completion items after dot', async () => {
		const document = await openFixture('module_struct_completion.nsf');
		const completionPosition = positionOf(document, 'instance.', 1, 'instance.'.length);
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'struct member completion items'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('color'), 'Expected struct field completion for color.');
		assert.ok(labels.has('value'), 'Expected struct field completion for value.');

		const items = getCompletionItems(completionResult);
		const colorItem = items.find((item) => item.label.toString() === 'color');
		assert.ok(colorItem);
		assert.strictEqual(colorItem.detail, 'float4');
	});

	it('provides struct member completion for struct keyword declarations', async () => {
		const document = await openFixture('module_struct_completion_struct_keyword.nsf');
		const completionPosition = positionOf(document, 'instance.', 1, 'instance.'.length);
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'struct member completion items'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('normal'));
		assert.ok(labels.has('roughness'));
	});

	it('provides struct member completion from workspace scan when not included', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for remote struct completion');
			const document = await openFixture('module_struct_completion_remote_a.nsf');
			const completionPosition = positionOf(document, 'value.', 1, 'value.'.length);
			const completionItems = await waitForCompletionLabels(
				document,
				completionPosition,
				['foo', 'bar'],
				'remote struct member completion items'
			);

			const labels = new Set(completionItems.map((item) => item.label.toString()));
			assert.ok(labels.has('foo'));
			assert.ok(labels.has('bar'));
		});
	});

	it('provides struct member completion from the active preprocessor branch', async () => {
		const document = await openFixture('module_struct_completion_preprocessor_branch.nsf');
		const completionPosition = positionOf(document, 'instance.', 1, 'instance.'.length);
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'struct member completion items'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('active_member'));
		assert.ok(labels.has('active_gain'));
		assert.ok(!labels.has('inactive_member'));
	});

	it('provides struct member completion for active conditional struct members only', async () => {
		const document = await openFixture('module_struct_completion_conditional_members.nsf');
		const completionPosition = positionOf(document, 'instance.', 1, 'instance.'.length);
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'conditional struct member completion items'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('active_member'));
		assert.ok(labels.has('active_gain'));
		assert.ok(!labels.has('inactive_member'));
	});

	});
}

export function registerDefinitionProviderTests(): void {
	repoDescribe('NSF client integration: Interactive Runtime / Definition Provider', () => {
	it('resolves local symbol definitions through the client', async () => {
		const document = await openFixture('module_suite.nsf');
		const usePosition = positionOf(document, 'SuiteLocalTint', 2, 2);
		const definitionLocations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					usePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'local definition results'
		);

		const definitionPath = path.basename(toFsPath(definitionLocations[0]));
		assert.strictEqual(definitionPath, 'module_suite.nsf');
	});

	it('resolves variable declarations through the client', async () => {
		const document = await openFixture('module_decls.nsf');
		const symbol = 'SuiteMultiB';
		const usePosition = positionOf(document, symbol, 2, 2);
		const definitionLocations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					usePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'variable declaration definitions'
		);

		assert.strictEqual(path.basename(toFsPath(definitionLocations[0])), 'module_decls.nsf');
		const range = toRange(definitionLocations[0]);
		assert.strictEqual(document.getText(range), symbol);
		assert.strictEqual(range.start.line, positionOf(document, symbol, 1, 0).line);
	});

	it('resolves struct member definitions without falling back to same-name globals', async () => {
		const document = await openFixture('module_definition_struct_member.nsf');
		const memberPosition = positionOf(document, 'member_color', 3, 2);
		const definitionLocations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					memberPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'struct member definition results'
		);

		assert.strictEqual(toFsPath(definitionLocations[0]), document.uri.fsPath);
		const range = toRange(definitionLocations[0]);
		assert.strictEqual(document.getText(range), 'member_color');
		assert.strictEqual(range.start.line, positionOf(document, 'member_color', 1, 0).line);
		assert.notStrictEqual(range.start.line, positionOf(document, 'member_color', 2, 0).line);
	});

	it('resolves included struct member definitions through workspace summary', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for included struct member definition');
			const document = await openFixture('module_definition_struct_member_include.nsf');
			const memberPosition = positionOf(document, 'included_member_color', 2, 2);
			const definitionLocations = await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						document.uri,
						memberPosition
					),
				(value) =>
					Array.isArray(value) &&
					value.some((location) =>
						toFsPath(location).endsWith('module_definition_struct_member_include_provider.hlsl')
					),
				'included struct member definition results'
			);

			const providerLocation = definitionLocations.find((location) =>
				toFsPath(location).endsWith('module_definition_struct_member_include_provider.hlsl')
			);
			assert.ok(providerLocation);
			const providerDocument = await vscode.workspace.openTextDocument(toFsPath(providerLocation));
			assert.strictEqual(providerDocument.getText(toRange(providerLocation)), 'included_member_color');
			assert.strictEqual(path.basename(toFsPath(providerLocation)), 'module_definition_struct_member_include_provider.hlsl');
		});
	});

	it('resolves inline-include struct member definitions without falling back to same-name globals', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for inline-include struct member definition');
			const document = await openFixture('module_definition_struct_member_inline_include.nsf');
			const memberPosition = positionOf(document, 'inline_member_color', 2, 2);
			const definitionLocations = await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						document.uri,
						memberPosition
					),
				(value) =>
					Array.isArray(value) &&
					value.some((location) =>
						toFsPath(location).endsWith('module_definition_struct_member_inline_include_fields.hlsl')
					),
				'inline-include struct member definition results'
			);

			const includeLocation = definitionLocations.find((location) =>
				toFsPath(location).endsWith('module_definition_struct_member_inline_include_fields.hlsl')
			);
			assert.ok(includeLocation);
			const includeDocument = await vscode.workspace.openTextDocument(toFsPath(includeLocation));
			assert.strictEqual(includeDocument.getText(toRange(includeLocation)), 'inline_member_color');
			assert.strictEqual(
				path.basename(toFsPath(includeLocation)),
				'module_definition_struct_member_inline_include_fields.hlsl'
			);
		});
	});

	it('returns multiple definition targets for include symbols when no active unit is selected', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedRoots = configuration.inspect<string[]>('intellisionPath');
		const originalRoots = inspectedRoots?.workspaceValue ?? [];
		await configuration.update(
			'intellisionPath',
			[path.join(getWorkspaceRoot(), 'test_files', 'include_context')],
			vscode.ConfigurationTarget.Workspace
		);
		await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
		try {
			const document = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
			await waitFor(
				() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
				(value) => !value?.indexingState || value.indexingState.state === 'Idle',
				'indexing status for include-context definition'
			);

			const position = positionOf(document, 'UnitSpecificTone', 1, 2);
			const definitions = await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						document.uri,
						position
					),
				(value) => Array.isArray(value) && value.length >= 2,
				'include-context definition results'
			);

			const definitionFiles = Array.from(
				new Set(definitions.map((location) => path.basename(toFsPath(location))))
			).sort();
			assert.deepStrictEqual(definitionFiles, ['multi_context_symbol_a.nsf', 'multi_context_symbol_b.nsf']);
		} finally {
			await configuration.update('intellisionPath', originalRoots, vscode.ConfigurationTarget.Workspace);
			await openFixture('module_suite.nsf');
		}
	});

	});
}

