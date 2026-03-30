import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	type ProviderLocation,
	countWorkspaceEdits,
	getWorkspaceRoot,
	hoverToText,
	isEmptyDefinitionResult,
	openFixture,
	positionOf,
	repoDescribe,
	toFsPath,
	toRange,
	waitFor,
	waitForHoverText,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from '../test_helpers';

export function registerReferencesRenameTests(): void {
	repoDescribe('NSF client integration: References and Rename', () => {
	it('merges references across candidate units for include symbols when no active unit is selected', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
			await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
			const document = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
			await waitForIndexingIdle('indexing status for include-context references');

			const position = positionOf(document, 'UnitSpecificTone', 1, 2);
			const references = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.Location[]>(
						'vscode.executeReferenceProvider',
						document.uri,
						position
					),
				(value) => Array.isArray(value) && value.length >= 3,
				'include-context reference results'
			);
			assert.strictEqual(references.length, 3);
		});
	});

	it('rejects rename for include symbols when no active unit is selected', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
			await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
			const document = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
			await waitForIndexingIdle('indexing status for include-context rename');

			const position = positionOf(document, 'UnitSpecificTone', 1, 2);
			try {
				const prepareResult = await vscode.commands.executeCommand<any>(
					'vscode.prepareRename',
					document.uri,
					position
				);
				assert.ok(!prepareResult);
			} catch (error) {
				const message = (error as Error).message ?? String(error);
				assert.ok(message.trim().length > 0, 'Expected rename rejection to surface a non-empty error message.');
			}
		});
	});

	it('keeps hover, definition, references, and rename consistent for include-context ambiguous symbols', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
			await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
			const document = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
			await waitForIndexingIdle('indexing status for include-context consistency');

			const position = positionOf(document, 'UnitSpecificTone', 1, 2);
			const hovers = await waitForHoverText(
				document,
				position,
				(text) =>
					text.includes('Include context ambiguous') &&
					text.includes('multi_context_symbol_a.nsf') &&
					text.includes('multi_context_symbol_b.nsf'),
				'include-context consistency hover'
			);
			const hoverText = hoverToText(hovers);
			assert.ok(hoverText.includes('Candidate definitions ('));

			const definitions = await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						document.uri,
						position
					),
				(value) => Array.isArray(value) && value.length >= 2,
				'include-context consistency definition'
			);
			const definitionFiles = Array.from(
				new Set(definitions.map((location) => path.basename(toFsPath(location))))
			).sort();
			assert.deepStrictEqual(definitionFiles, ['multi_context_symbol_a.nsf', 'multi_context_symbol_b.nsf']);

			const references = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.Location[]>(
						'vscode.executeReferenceProvider',
						document.uri,
						position
					),
				(value) => Array.isArray(value) && value.length >= 3,
				'include-context consistency references'
			);
			const referenceFiles = new Set(references.map((location) => path.basename(location.uri.fsPath)));
			assert.ok(referenceFiles.has('multi_context_symbol_common.hlsl'));
			assert.ok(referenceFiles.has('multi_context_symbol_a.nsf'));
			assert.ok(referenceFiles.has('multi_context_symbol_b.nsf'));

			try {
				const prepareResult = await vscode.commands.executeCommand<any>(
					'vscode.prepareRename',
					document.uri,
					position
				);
				assert.ok(!prepareResult, 'Expected rename to stay rejected for include-context ambiguous symbols.');
			} catch (error) {
				const message = (error as Error).message ?? String(error);
				assert.ok(message.trim().length > 0, 'Expected rename rejection to surface a non-empty error message.');
			}
		});
	});

	it('resolves parameter declarations through the client', async () => {
		const document = await openFixture('module_decls.nsf');
		const symbol = 'SuiteParamMatrix';
		const usePosition = positionOf(document, symbol, 2, 2);
		const definitionLocations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					usePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'parameter declaration definitions'
		);

		assert.strictEqual(path.basename(toFsPath(definitionLocations[0])), 'module_decls.nsf');
		const range = toRange(definitionLocations[0]);
		assert.strictEqual(document.getText(range), symbol);
		assert.strictEqual(range.start.line, positionOf(document, symbol, 1, 0).line);
	});

	it('does not return ambiguous fallback definitions', async () => {
		const document = await openFixture('module_decls.nsf');
		const hoverPosition = positionOf(document, 'SuiteMultiB', 2, 2);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover warmup'
		);

		const symbol = 'UndeclaredSymbol';
		const definitionPosition = positionOf(document, symbol, 1, 2);
		const definitionLocations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					definitionPosition
				),
			(value) => Array.isArray(value),
			'ambiguous fallback definitions'
		);

		assert.ok(isEmptyDefinitionResult(definitionLocations));
	});

	it('routes include definition requests through the client', async () => {
		const document = await openFixture('module_suite.nsf');
		const definitionPosition = positionOf(document, 'module_shared.ush', 1, 2);
		const locations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					definitionPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'include definition results'
		);

		assert.strictEqual(
			path.basename(toFsPath(locations[0])),
			'module_shared.ush',
			'Expected include definition to resolve to the target shader file.'
		);
	});

	it('finds references across unopened included shader files', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for cross-file references');
			const suiteDocument = await openFixture('module_suite.nsf');
			const referencePosition = positionOf(suiteDocument, 'SuiteMakeSharedColor', 1, 2);
			const references = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.Location[]>(
						'vscode.executeReferenceProvider',
						suiteDocument.uri,
						referencePosition
					),
				(value) => Array.isArray(value) && value.length >= 3,
				'reference results'
			);

			assert.strictEqual(references.length, 3, 'Expected cross-file references for SuiteMakeSharedColor.');
		});
	});

	it('prepares rename and returns workspace edits across unopened include files', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for cross-file rename');
			const suiteDocument = await openFixture('module_suite.nsf');
			const renamePosition = positionOf(suiteDocument, 'SuiteMakeSharedColor', 1, 2);
			const prepareResult = await waitFor(
				() =>
					vscode.commands.executeCommand<{ range: vscode.Range; placeholder: string }>(
						'vscode.prepareRename',
						suiteDocument.uri,
						renamePosition
					),
				(value) => Boolean(value?.placeholder),
				'prepare rename result'
			);

			assert.strictEqual(prepareResult.placeholder, 'SuiteMakeSharedColor');

			const renameEdit = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.WorkspaceEdit>(
						'vscode.executeDocumentRenameProvider',
						suiteDocument.uri,
						renamePosition,
						'RenamedSuiteColor'
					),
				(value) => Boolean(value) && countWorkspaceEdits(value) >= 3,
				'rename workspace edit'
			);

			assert.strictEqual(countWorkspaceEdits(renameEdit), 3, 'Expected rename edits in the include graph.');
		});
	});

	it('groups include-file conditional branch variants in references and rename', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for include branch variants');
			const document = await openFixture('module_references_rename_include_preprocessor_root.nsf');
			const position = positionOf(document, 'IncludeBranchColor', 1, 2);

			const references = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.Location[]>(
						'vscode.executeReferenceProvider',
						document.uri,
						position
					),
				(value) => Array.isArray(value) && value.length >= 5,
				'include branch reference results'
			);
			assert.strictEqual(references.length, 5, 'Expected all-branch references for the include-file conditional family.');

			const renameEdit = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.WorkspaceEdit>(
						'vscode.executeDocumentRenameProvider',
						document.uri,
						position,
						'RenamedIncludeBranchColor'
					),
				(value) => Boolean(value) && countWorkspaceEdits(value) >= 5,
				'include branch rename workspace edit'
			);
			assert.strictEqual(countWorkspaceEdits(renameEdit), 5, 'Expected all-branch rename edits for the include-file conditional family.');
		});
	});

	it('groups references and rename edits across conditional branch variants in one document', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for conditional symbol family');
			const document = await openFixture('module_references_rename_preprocessor_branch.nsf');
			const symbol = 'BranchFamilyWeight';
			const position = positionOf(document, symbol, 2, 2);

			const references = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.Location[]>(
						'vscode.executeReferenceProvider',
						document.uri,
						position
					),
				(value) => Array.isArray(value) && value.length >= 4,
				'conditional branch reference results'
			);
			assert.strictEqual(
				references.length,
				4,
				`Expected all-branch references for conditional symbol family. Actual=${references
					.map((location) => `${path.basename(location.uri.fsPath)}:${location.range.start.line + 1}`)
					.join(',')}`
			);

			const renameEdit = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.WorkspaceEdit>(
						'vscode.executeDocumentRenameProvider',
						document.uri,
						position,
						'RenamedBranchFamilyWeight'
					),
				(value) => Boolean(value) && countWorkspaceEdits(value) >= 4,
				'conditional branch rename workspace edit'
			);
			assert.strictEqual(countWorkspaceEdits(renameEdit), 4, 'Expected all-branch rename edits for conditional symbol family.');
		});
	});

	it('groups function references and rename edits across conditional branch variants in one document', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for conditional function family');
			const document = await openFixture('module_references_rename_preprocessor_function_branch.nsf');
			const symbol = 'BranchFamilyFunc';
			const position = positionOf(document, symbol, 3, 2);

			const references = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.Location[]>(
						'vscode.executeReferenceProvider',
						document.uri,
						position
					),
				(value) => Array.isArray(value) && value.length >= 5,
				'conditional branch function reference results'
			);
			assert.strictEqual(references.length, 5, 'Expected all-branch function references for conditional symbol family.');

			const renameEdit = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.WorkspaceEdit>(
						'vscode.executeDocumentRenameProvider',
						document.uri,
						position,
						'RenamedBranchFamilyFunc'
					),
				(value) => Boolean(value) && countWorkspaceEdits(value) >= 5,
				'conditional branch function rename workspace edit'
			);
			assert.strictEqual(countWorkspaceEdits(renameEdit), 5, 'Expected all-branch function rename edits for conditional symbol family.');
		});
	});

	it('smoke: handles UTF-16 positions when preparing rename', async () => {
		const document = await openFixture('module_utf16_positions.nsf');
		const renamePosition = positionOf(document, 'foo', 1, 2);

		const prepareResult = await waitFor(
			() =>
				vscode.commands.executeCommand<{ range: vscode.Range; placeholder: string }>(
					'vscode.prepareRename',
					document.uri,
					renamePosition
				),
			(value) => Boolean(value?.placeholder),
			'utf16 prepare rename'
		);

		assert.strictEqual(prepareResult.placeholder, 'foo');
		const expectedStart = positionOf(document, 'foo', 1, 0);
		const expectedEnd = positionOf(document, 'foo', 1, 'foo'.length);
		assert.deepStrictEqual(prepareResult.range.start, expectedStart);
		assert.deepStrictEqual(prepareResult.range.end, expectedEnd);
	});

	});
}

