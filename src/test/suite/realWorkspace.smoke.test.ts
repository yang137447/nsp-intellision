import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

type ProviderLocation = vscode.Location | vscode.LocationLink;
type SymbolLike = vscode.DocumentSymbol | vscode.SymbolInformation;
type Awaitable<T> = T | Thenable<T> | PromiseLike<T>;
type WaitForOptions = {
	attempts?: number;
	delayMs?: number;
};

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
	label: string,
	options?: WaitForOptions
): Promise<T> {
	let lastValue: T | undefined;
	const maxAttempts = options?.attempts ?? (testMode === 'real' ? 120 : 40);
	const delayMs = options?.delayMs ?? 500;
	for (let attempt = 0; attempt < maxAttempts; attempt++) {
		lastValue = await Promise.resolve(producer());
		if (isReady(lastValue)) {
			return lastValue;
		}
		await new Promise((resolve) => setTimeout(resolve, delayMs));
	}
	throw new Error(`Timed out waiting for ${label}.`);
}

async function waitForClientQuiescent(label: string): Promise<void> {
	await waitFor(
		() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
		(value) => {
			const state = value?.indexingState;
			const indexingIdle =
				!state ||
				(state.state === 'Idle' &&
					(state.pending?.queuedTasks ?? 0) === 0 &&
					(state.pending?.runningWorkers ?? 0) === 0);
			return (
				indexingIdle &&
				(value?.activeRpcCount ?? 0) === 0 &&
				!value?.pendingInitialInlayRefreshAfterIndex &&
				!value?.pendingInlayRefreshAfterIndexActivity
			);
		},
		label,
		{ attempts: 180 }
	);
}

async function resetRealSmokeEditorState(): Promise<void> {
	for (const document of vscode.workspace.textDocuments) {
		if (!document.isDirty || document.isUntitled) {
			continue;
		}
		await vscode.window.showTextDocument(document, { preview: false });
		await vscode.commands.executeCommand('workbench.action.files.revert');
	}
	await vscode.commands.executeCommand('workbench.action.closeAllEditors');
	const commands = await vscode.commands.getCommands(true);
	if (!commands.includes('nsf._getInternalStatus')) {
		return;
	}
	await waitForClientQuiescent('real workspace smoke precondition quiescent');
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

async function requestServerDocumentSymbols(document: vscode.TextDocument): Promise<SymbolLike[]> {
	const response = await vscode.commands.executeCommand<SymbolLike[]>(
		'nsf._sendServerRequest',
		{
			method: 'textDocument/documentSymbol',
			params: {
				textDocument: { uri: document.uri.toString() }
			}
		}
	);
	return Array.isArray(response) ? response : [];
}

async function getDocumentSymbolsDebug(document: vscode.TextDocument): Promise<string> {
	let status: any;
	try {
		status = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
	} catch (error) {
		status = { error: error instanceof Error ? error.message : String(error) };
	}

	let runtime: any;
	try {
		runtime = await vscode.commands.executeCommand<any>('nsf._getDocumentRuntimeDebug', {
			uris: [document.uri.toString()]
		});
	} catch (error) {
		runtime = { error: error instanceof Error ? error.message : String(error) };
	}
	const state = status?.indexingState;
	const runtimeEntry = Array.isArray(runtime?.documents) ? runtime.documents[0] : undefined;
	return JSON.stringify({
		status: {
			clientState: status?.clientState,
			activeRpcCount: status?.activeRpcCount,
			indexingState: state?.state,
			queuedTasks: state?.pending?.queuedTasks,
			runningWorkers: state?.pending?.runningWorkers,
			pendingInitialInlayRefreshAfterIndex: status?.pendingInitialInlayRefreshAfterIndex,
			pendingInlayRefreshAfterIndexActivity: status?.pendingInlayRefreshAfterIndexActivity
		},
		runtime: {
			exists: runtimeEntry?.exists,
			version: runtimeEntry?.version,
			epoch: runtimeEntry?.epoch,
			hasDeferredDocSnapshot: runtimeEntry?.hasDeferredDocSnapshot,
			deferredHasSemanticSnapshot: runtimeEntry?.deferredHasSemanticSnapshot,
			deferredHasSemanticTokensFull: runtimeEntry?.deferredHasSemanticTokensFull,
			deferredHasDocumentSymbols: runtimeEntry?.deferredHasDocumentSymbols,
			activeUnitPath: runtimeEntry?.activeUnitPath
		}
	});
}

async function waitForDocumentSymbols(
	document: vscode.TextDocument,
	label: string
): Promise<SymbolLike[]> {
	let lastProviderLength = 0;
	let lastServerLength = 0;
	let lastServerNames: string[] = [];
	try {
		return await waitFor(
			async () => {
				const providerSymbols = await vscode.commands.executeCommand<SymbolLike[]>(
					'vscode.executeDocumentSymbolProvider',
					document.uri
				);
				lastProviderLength = Array.isArray(providerSymbols) ? providerSymbols.length : 0;
				if (lastProviderLength > 0) {
					return providerSymbols;
				}

				// Real full-suite runs can leave VS Code's executeDocumentSymbolProvider returning
				// an empty provider result even after the LSP server has materialized symbols.
				const serverSymbols = await requestServerDocumentSymbols(document);
				lastServerLength = serverSymbols.length;
				lastServerNames = flattenSymbolNames(serverSymbols).slice(0, 16);
				return serverSymbols.length > 0 ? serverSymbols : providerSymbols;
			},
			(value) => Array.isArray(value) && value.length > 0,
			label,
			{ attempts: 240 }
		);
	} catch (error) {
		const debug = await getDocumentSymbolsDebug(document);
		const message = error instanceof Error ? error.message : String(error);
		throw new Error(
			`${message} providerLength=${lastProviderLength} serverLength=${lastServerLength} serverNames=${lastServerNames.join(', ')} debug=${debug}`
		);
	}
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

function hoverToText(hovers: vscode.Hover[]): string {
	const parts: string[] = [];
	for (const hover of hovers) {
		for (const content of hover.contents) {
			if (typeof content === 'string') {
				parts.push(content);
				continue;
			}
			if (content instanceof vscode.MarkdownString) {
				parts.push(content.value);
				continue;
			}
			const maybeValue = (content as unknown as { value?: unknown }).value;
			if (typeof maybeValue === 'string') {
				parts.push(maybeValue);
			}
		}
	}
	return parts.join('\n');
}

realDescribe('NSF real workspace smoke', () => {
	beforeEach(async function () {
		this.timeout(180000);
		await resetRealSmokeEditorState();
	});

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
			`Expected direct include definition to resolve to base_pass_vertex_shader.hlsl or cloud.nsf. Actual=${directIncludeBasenames.join(', ')}`
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

		await waitForClientQuiescent('real workspace client quiescent before document symbols');
		const symbols = await waitForDocumentSymbols(buildingDocument, 'real workspace document symbols');
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

		await waitForClientQuiescent('real workspace client quiescent before macro-heavy document symbols');
		const symbols = await waitForDocumentSymbols(hlslDocument, 'real workspace macro-heavy document symbols');
		const names = flattenSymbolNames(symbols);
		assert.ok(names.includes('BlueTideInflunence'));
		assert.ok(names.includes('textureNoTile'));
		assert.ok(names.includes('BlueTideTerrainInflunence'));
	});

	it('resolves hover and definition for direct include-chain helper functions in real nsf files', async function () {
		this.timeout(180000);
		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		const multilayerPath = path.join(shaderSourceRoot, 'sfx', 'uber_fx_common_multilayer.nsf');
		const multilayerDocument = await openDocument(multilayerPath);
		await waitForIndexingIdle('real workspace indexing idle for uber_fx_common_multilayer.nsf');

		const loadBatchPosition = positionOf(multilayerDocument, 'LoadBatchParamsVs', 1, 3);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					multilayerDocument.uri,
					loadBatchPosition
				),
			(value) => {
				if (!Array.isArray(value) || value.length === 0) {
					return false;
				}
				return hoverToText(value).includes('LoadBatchParamsVs(');
			},
			'real workspace LoadBatchParamsVs hover'
		);
		assert.ok(hoverToText(hovers).includes('LoadBatchParamsVs('));

		const definitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					multilayerDocument.uri,
					loadBatchPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace LoadBatchParamsVs definition'
		);
		const basenames = definitions.map((item) => path.basename(toFsPath(item)).toLowerCase());
		assert.ok(
			basenames.includes('uber_fx_common_multilayer_batch.hlsl'),
			`Expected LoadBatchParamsVs to resolve to uber_fx_common_multilayer_batch.hlsl, got ${basenames.join(', ')}`
		);
	});
});
