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
} from '../test_helpers';

export function registerWorkspaceSummaryTypeFallbackTests(): void {
	repoDescribe('NSF client integration: Workspace Summary / Type Fallback', () => {
		it('falls back to workspace scan for type definitions when not included', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				await waitForIndexingIdle('indexing idle for type fallback');
				const document = await openFixture('module_definition_scan_a.nsf');
				const position = positionOf(document, 'FGBufferData', 1, 1);

				const definitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							position
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) => toFsPath(location).endsWith('module_definition_scan_b.nsf')),
					'definition provider'
				);

				assert.ok(definitions.length > 0, JSON.stringify(definitions));
				assert.ok(toFsPath(definitions[0]).endsWith('module_definition_scan_b.nsf'));
			});
		});
	});
}

export function registerWorkspaceSummaryDefinitionFallbackTests(): void {
	repoDescribe('NSF client integration: Workspace Summary / Definition Fallback', () => {
		it('falls back to workspace scan for function and variable definitions when not included', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				await waitForIndexingIdle('indexing idle for symbol definition fallback');
				const document = await openFixture('module_definition_scan_symbols_a.nsf');

				const functionPosition = positionOf(document, 'RemoteFunc', 1, 1);
				const functionDefinitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							functionPosition
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) =>
							toFsPath(location).endsWith('module_definition_scan_symbols_b.nsf')
						),
					'definition provider'
				);
				assert.ok(functionDefinitions.length > 0);
				assert.ok(toFsPath(functionDefinitions[0]).endsWith('module_definition_scan_symbols_b.nsf'));

				const variablePosition = positionOf(document, 'RemoteGlobal', 1, 1);
				const variableDefinitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							variablePosition
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) =>
							toFsPath(location).endsWith('module_definition_scan_symbols_b.nsf')
						),
					'definition provider'
				);
				assert.ok(variableDefinitions.length > 0);
				assert.ok(toFsPath(variableDefinitions[0]).endsWith('module_definition_scan_symbols_b.nsf'));
			});
		});

		it('falls back to workspace scan for macro definitions when not included', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				await waitForIndexingIdle('indexing idle for macro definition fallback');
				const document = await openFixture('module_definition_scan_macro_a.nsf');
				const position = positionOf(document, 'REMOTE_MACRO', 1, 2);

				const definitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							position
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) =>
							toFsPath(location).endsWith('module_definition_scan_macro_b.nsf')
						),
					'definition provider'
				);

				assert.ok(definitions.length > 0, JSON.stringify(definitions));
				assert.ok(toFsPath(definitions[0]).endsWith('module_definition_scan_macro_b.nsf'));
			});
		});

		it('resolves multiline FX-style resource declarations through include graph', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				await waitForIndexingIdle('indexing idle for FX declaration fallback');
				const document = await openFixture('module_definition_multiline_fx_decl_a.nsf');

				const texPosition = positionOf(document, 'Tex0', 1, 1);
				const texDefinitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							texPosition
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) =>
							toFsPath(location).endsWith('module_definition_multiline_fx_decl_b.nsf')
						),
					'definition provider'
				);
				assert.ok(texDefinitions.length > 0, JSON.stringify(texDefinitions));
				const texDefinitionPaths = new Set(texDefinitions.map((location) => toFsPath(location)));
				assert.ok(
					Array.from(texDefinitionPaths).some((filePath) =>
						filePath.endsWith('module_definition_multiline_fx_decl_b.nsf')
					),
					Array.from(texDefinitionPaths).join(',')
				);

				const samplerPosition = positionOf(document, 's_diffuse', 1, 1);
				const samplerDefinitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							samplerPosition
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) =>
							toFsPath(location).endsWith('module_definition_multiline_fx_decl_b.nsf')
						),
					'definition provider'
				);
				assert.ok(samplerDefinitions.length > 0);
				assert.ok(toFsPath(samplerDefinitions[0]).endsWith('module_definition_multiline_fx_decl_b.nsf'));
			});
		});
	});
}

export function registerWorkspaceSummarySchedulingTests(): void {
	repoDescribe('NSF client integration: Workspace Summary / Scheduling', () => {
		it('returns workspace symbol candidates for include-context ambiguous definitions', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				await waitForIndexingIdle('indexing idle for workspace symbol candidates');
				const symbols = await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.SymbolInformation[]>(
							'vscode.executeWorkspaceSymbolProvider',
							'UnitSpecificTone'
						),
					(value) => Array.isArray(value) && value.length >= 2,
					'workspace symbol candidates'
				);

				const matching = symbols.filter((symbol) => symbol.name === 'UnitSpecificTone');
				const symbolFiles = Array.from(
					new Set(matching.map((symbol) => path.basename(symbol.location.uri.fsPath)))
				).sort();
				assert.deepStrictEqual(symbolFiles, ['multi_context_symbol_a.nsf', 'multi_context_symbol_b.nsf']);
			});
		});

		it('handles workspace symbol bursts with cancellation-safe responses', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				await waitForIndexingIdle('indexing idle for workspace symbol burst');
				const burst = await vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
					'nsf._spamWorkspaceSymbolRequests',
					{
						query: 'UnitSpecificTone',
						count: 24
					}
				);

				assert.ok(Boolean(burst));
				assert.ok((burst?.completed ?? 0) > 0, 'Expected at least one workspace symbol request to complete.');
				assert.strictEqual(burst?.failed ?? 0, 0);
				assert.ok((burst?.cancelled ?? 0) >= 0);
			});
		});

		it('keeps interactive hover stable while workspace symbol requests are queued', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				await waitForIndexingIdle('indexing idle for workspace symbol hover isolation');
				const hoverDocument = await openFixture('module_hover_docs.nsf');
				const hoverPosition = positionOf(hoverDocument, 'SuiteHoverFunc', 2, 2);

				const burstPromise = vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
					'nsf._spamWorkspaceSymbolRequests',
					{
						query: 'UnitSpecificTone',
						count: 24
					}
				);

				const hoverTexts: string[] = [];
				for (let attempt = 0; attempt < 5; attempt++) {
					const hovers = await waitForHoverText(
						hoverDocument,
						hoverPosition,
						(text) => text.includes('SuiteHoverFunc doc line 1') && text.includes('SuiteHoverFunc('),
						'workspace symbol background hover'
					);
					hoverTexts.push(hoverToText(hovers));
				}

				const burst = await burstPromise;
				assert.ok(hoverTexts.every((text) => text.includes('SuiteHoverFunc doc line 1')));
				assert.ok(hoverTexts.every((text) => !text.includes('Candidate units (')));
				assert.strictEqual(burst?.failed ?? 0, 0);
			});
		});
	});
}
