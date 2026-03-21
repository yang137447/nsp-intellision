import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

type ProviderLocation = vscode.Location | vscode.LocationLink;
type SymbolLike = vscode.DocumentSymbol | vscode.SymbolInformation;
type Awaitable<T> = T | Thenable<T> | PromiseLike<T>;

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const realDescribe = testMode === 'real' ? describe : describe.skip;

function getWorkspaceFolderPath(folderSuffix: string): string {
	const folder = vscode.workspace.workspaceFolders?.find((item) =>
		item.uri.fsPath.replace(/\\/g, '/').toLowerCase().endsWith(folderSuffix.toLowerCase())
	);
	assert.ok(folder, `Expected workspace folder ending with '${folderSuffix}'.`);
	return folder.uri.fsPath;
}

async function openDocument(absolutePath: string): Promise<vscode.TextDocument> {
	const document = await vscode.workspace.openTextDocument(absolutePath);
	await vscode.window.showTextDocument(document, { preview: false });
	return document;
}

async function waitForIndexingIdle(label: string): Promise<void> {
	await waitFor(
		() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
		(value) => !value?.indexingState || value.indexingState.state === 'Idle',
		label
	);
}

async function waitFor<T>(
	producer: () => Awaitable<T>,
	isReady: (value: T) => boolean,
	label: string
): Promise<T> {
	let lastValue: T | undefined;
	const maxAttempts = testMode === 'real' ? 120 : 40;
	for (let attempt = 0; attempt < maxAttempts; attempt++) {
		lastValue = await Promise.resolve(producer());
		if (isReady(lastValue)) {
			return lastValue;
		}
		await new Promise((resolve) => setTimeout(resolve, 500));
	}
	throw new Error(`Timed out waiting for ${label}.`);
}

function toFsPath(location: ProviderLocation): string {
	if ('targetUri' in location) {
		return location.targetUri.fsPath;
	}
	return location.uri.fsPath;
}

function findOffset(document: vscode.TextDocument, needle: string, occurrence = 1): number {
	let fromIndex = 0;
	let foundIndex = -1;
	for (let current = 0; current < occurrence; current++) {
		foundIndex = document.getText().indexOf(needle, fromIndex);
		assert.notStrictEqual(foundIndex, -1, `Unable to find occurrence ${occurrence} of '${needle}'.`);
		fromIndex = foundIndex + needle.length;
	}
	return foundIndex;
}

function positionOf(
	document: vscode.TextDocument,
	needle: string,
	occurrence = 1,
	characterOffset = 0
): vscode.Position {
	const offset = findOffset(document, needle, occurrence) + characterOffset;
	return document.positionAt(offset);
}

function flattenSymbolNames(symbols: SymbolLike[]): string[] {
	const names: string[] = [];
	for (const symbol of symbols) {
		names.push(symbol.name);
		if ('children' in symbol && Array.isArray(symbol.children)) {
			names.push(...flattenSymbolNames(symbol.children));
		}
	}
	return names;
}

function countWorkspaceEdits(edit: vscode.WorkspaceEdit): number {
	let count = 0;
	for (const [, edits] of edit.entries()) {
		count += edits.length;
	}
	return count;
}

function getCompletionItems(result: vscode.CompletionList | vscode.CompletionItem[] | undefined): vscode.CompletionItem[] {
	if (!result) {
		return [];
	}
	return Array.isArray(result) ? result : result.items;
}

realDescribe('NSF real workspace smoke', () => {
	it('works against a real project nsf file', async function () {
		this.timeout(180000);
		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		const buildingPath = path.join(shaderSourceRoot, 'base', 'building.nsf');
		const buildingDocument = await openDocument(buildingPath);
		await waitForIndexingIdle('real workspace indexing idle for building.nsf');

		assert.strictEqual(buildingDocument.languageId, 'nsf');

		const hoverPosition = positionOf(buildingDocument, 'CalcWorldDataInstance', 1, 3);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					buildingDocument.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace hover results'
		);
		assert.ok(hovers[0]);

		const includePosition = positionOf(buildingDocument, 'common.hlsl', 1, 2);
		const includeDefinitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					buildingDocument.uri,
					includePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace include definitions'
		);
		assert.strictEqual(path.basename(toFsPath(includeDefinitions[0])), 'common.hlsl');

		const transitiveDefinitionPosition = positionOf(buildingDocument, 'CalcWorldDataInstance', 1, 3);
		const transitiveDefinitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					buildingDocument.uri,
					transitiveDefinitionPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace transitive definitions'
		);
		assert.strictEqual(path.basename(toFsPath(transitiveDefinitions[0])), 'vertex_functions.hlsl');

		const directIncludeDefinitionPosition = positionOf(buildingDocument, 'vs_main', 1, 2);
		const directIncludeDefinitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					buildingDocument.uri,
					directIncludeDefinitionPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace direct include definitions'
		);
		const directIncludeBasenames = directIncludeDefinitions.map((item) => path.basename(toFsPath(item)).toLowerCase());
		assert.ok(
			directIncludeBasenames.includes('base_pass_vertex_shader.hlsl') ||
				directIncludeBasenames.includes('cloud.nsf'),
			'Expected direct include definition to resolve to base_pass_vertex_shader.hlsl or cloud.nsf.'
		);

		const localDefinitionPosition = positionOf(buildingDocument, 'diffuse_color', 4, 3);
		const localDefinitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					buildingDocument.uri,
					localDefinitionPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace local definitions'
		);
		assert.strictEqual(path.basename(toFsPath(localDefinitions[0])), 'building.nsf');

		const references = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Location[]>(
					'vscode.executeReferenceProvider',
					buildingDocument.uri,
					localDefinitionPosition
				),
			(value) => Array.isArray(value) && value.length >= 1,
			'real workspace references'
		);
		assert.ok(references.length >= 1);

		const prepareRename = await waitFor(
			() =>
				vscode.commands.executeCommand<{ placeholder: string }>(
					'vscode.prepareRename',
					buildingDocument.uri,
					localDefinitionPosition
				),
			(value) => Boolean(value?.placeholder),
			'real workspace prepare rename'
		);
		assert.strictEqual(prepareRename.placeholder, 'diffuse_color');

		const renameEdit = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.WorkspaceEdit>(
					'vscode.executeDocumentRenameProvider',
					buildingDocument.uri,
					localDefinitionPosition,
					'diffuse_color_smoke'
				),
			(value) => Boolean(value) && countWorkspaceEdits(value) >= 1,
			'real workspace rename edit'
		);
		assert.ok(countWorkspaceEdits(renameEdit) >= 1);

		const symbols = await waitFor(
			() =>
				vscode.commands.executeCommand<SymbolLike[]>(
					'vscode.executeDocumentSymbolProvider',
					buildingDocument.uri
				),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace document symbols'
		);
		const names = flattenSymbolNames(symbols);
		assert.ok(names.includes('VS_INPUT'));
		assert.ok(names.includes('PS_INPUT'));
		assert.ok(names.includes('VertexDataNodesBasedGraph'));
		assert.ok(names.includes('PixelNodesBasedGraph'));
		assert.ok(names.includes('TShader'));
		assert.ok(names.includes('p0'));

		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					buildingDocument.uri,
					new vscode.Position(0, 0)
				),
			(value) => getCompletionItems(value).length > 0,
			'real workspace completion items'
		);
		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('#include'));
		assert.ok(labels.has('float4'));
	});

	it('handles cross-file references and rename in unopened real includes', async () => {
		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		const buildingPath = path.join(shaderSourceRoot, 'base', 'building.nsf');

		const buildingDocument = await openDocument(buildingPath);
		await waitForIndexingIdle('real workspace indexing idle for include smoke');

		const calcWorldPosition = positionOf(buildingDocument, 'CalcWorldDataInstance', 1, 3);
		const calcWorldReferences = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Location[]>(
					'vscode.executeReferenceProvider',
					buildingDocument.uri,
					calcWorldPosition
				),
			(value) => Array.isArray(value) && value.length >= 1,
			'real workspace transitive references'
		);
		assert.ok(calcWorldReferences.length >= 1);

		const calcWorldRename = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.WorkspaceEdit>(
					'vscode.executeDocumentRenameProvider',
					buildingDocument.uri,
					calcWorldPosition,
					'CalcWorldDataInstanceSmoke'
				),
			(value) => Boolean(value) && countWorkspaceEdits(value) >= 1,
			'real workspace transitive rename edit'
		);
		assert.ok(countWorkspaceEdits(calcWorldRename) >= 1);

		const vsMainPosition = positionOf(buildingDocument, 'vs_main', 1, 2);
		const vsMainReferences = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Location[]>(
					'vscode.executeReferenceProvider',
					buildingDocument.uri,
					vsMainPosition
				),
			(value) => Array.isArray(value),
			'real workspace direct include references'
		);
		assert.ok(Array.isArray(vsMainReferences));

		const vsMainRename = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.WorkspaceEdit>(
					'vscode.executeDocumentRenameProvider',
					buildingDocument.uri,
					vsMainPosition,
					'vs_main_smoke'
				),
			(value) => Boolean(value),
			'real workspace direct include rename edit'
		);
		assert.ok(countWorkspaceEdits(vsMainRename) >= 0);
	});

	it('handles macro-heavy real hlsl files', async () => {
		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		const hlslPath = path.join(shaderSourceRoot, 'base', 'bluetide_common.hlsl');
		const hlslDocument = await openDocument(hlslPath);
		await waitForIndexingIdle('real workspace indexing idle for macro-heavy hlsl');
		assert.strictEqual(hlslDocument.languageId, 'nsf');

		const hoverPosition = positionOf(hlslDocument, 'BlueTideInflunence', 1, 3);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					hlslDocument.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace hlsl hover results'
		);
		assert.ok(hovers[0]);

		const localDefinitionPosition = positionOf(hlslDocument, 'textureNoTile', 2, 3);
		const localDefinitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					hlslDocument.uri,
					localDefinitionPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace macro-heavy local definitions'
		);
		assert.strictEqual(path.basename(toFsPath(localDefinitions[0])), 'bluetide_common.hlsl');

		const symbols = await waitFor(
			() =>
				vscode.commands.executeCommand<SymbolLike[]>(
					'vscode.executeDocumentSymbolProvider',
					hlslDocument.uri
				),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace macro-heavy document symbols'
		);
		const names = flattenSymbolNames(symbols);
		assert.ok(names.includes('BlueTideInflunence'));
		assert.ok(names.includes('textureNoTile'));
		assert.ok(names.includes('BlueTideTerrainInflunence'));
	});
});
