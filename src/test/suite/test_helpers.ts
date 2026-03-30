import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

export type ProviderLocation = vscode.Location | vscode.LocationLink;
export type SymbolLike = vscode.DocumentSymbol | vscode.SymbolInformation;
type Awaitable<T> = T | Thenable<T> | PromiseLike<T>;

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
export const repoDescribe = testMode === 'repo' ? describe : describe.skip;

export function getWorkspaceRoot(): string {
	const folder = vscode.workspace.workspaceFolders?.[0];
	assert.ok(folder, 'Expected the extension tests to open a workspace folder.');
	return folder.uri.fsPath;
}

export async function openFixture(relativePath: string): Promise<vscode.TextDocument> {
	const absolutePath = path.join(getWorkspaceRoot(), 'test_files', relativePath);
	const document = await vscode.workspace.openTextDocument(absolutePath);
	await vscode.window.showTextDocument(document, { preview: false });
	return document;
}

export async function openExternalDocument(absolutePath: string): Promise<vscode.TextDocument> {
	const document = await vscode.workspace.openTextDocument(absolutePath);
	await vscode.window.showTextDocument(document, { preview: false });
	return document;
}

export async function waitFor<T>(
	producer: () => Awaitable<T>,
	isReady: (value: T) => boolean,
	label: string
): Promise<T> {
	let lastValue: T | undefined;
	let lastError: unknown;
	for (let attempt = 0; attempt < 60; attempt++) {
		try {
			lastValue = await Promise.resolve(producer());
			if (isReady(lastValue)) {
				return lastValue;
			}
			lastError = undefined;
		} catch (error) {
			lastError = error;
		}
		await new Promise((resolve) => setTimeout(resolve, 500));
	}
	const suffix =
		lastError !== undefined
			? ` Last error: ${lastError instanceof Error ? lastError.message : String(lastError)}`
			: '';
	throw new Error(`Timed out waiting for ${label}.${suffix}`);
}

export async function waitForIndexingIdle(label = 'indexing idle'): Promise<any> {
	const commands = await vscode.commands.getCommands(true);
	if (!commands.includes('nsf._getInternalStatus')) {
		const activationDoc = await openFixture('module_suite.nsf');
		await vscode.commands.executeCommand('workbench.action.closeActiveEditor');
		void activationDoc;
	}
	return waitFor(
		() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
		(value) => {
			const state = value?.indexingState;
			if (!state) {
				return true;
			}
			return (
				state.state === 'Idle' &&
				(state.pending?.queuedTasks ?? 0) === 0 &&
				(state.pending?.runningWorkers ?? 0) === 0
			);
		},
		label
	);
}

export async function waitForClientQuiescent(label = 'client quiescent'): Promise<any> {
	const commands = await vscode.commands.getCommands(true);
	if (!commands.includes('nsf._getInternalStatus')) {
		const activationDoc = await openFixture('module_suite.nsf');
		await vscode.commands.executeCommand('workbench.action.closeActiveEditor');
		void activationDoc;
	}
	return waitFor(
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
		label
	);
}

export async function waitForClientReady(label = 'client ready'): Promise<any> {
	const commands = await vscode.commands.getCommands(true);
	if (!commands.includes('nsf._getInternalStatus')) {
		await openFixture('module_suite.nsf');
	}
	return waitFor(
		() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
		(value) => value?.clientState === 'ready',
		label
	);
}

export type DocumentRuntimeDebugEntry = {
	uri: string;
	exists: boolean;
	version?: number;
	epoch?: number;
	analysisFullFingerprint?: string;
	analysisStableFingerprint?: string;
	workspaceSummaryVersion?: number;
	activeUnitPath?: string;
	activeUnitIncludeClosureFingerprint?: string;
	activeUnitBranchFingerprint?: string;
	activeUnitWorkspaceSummaryVersion?: number;
	hasInteractiveSnapshot?: boolean;
	hasLastGoodInteractiveSnapshot?: boolean;
	hasDeferredDocSnapshot?: boolean;
	interactiveAnalysisFullFingerprint?: string;
	interactiveAnalysisStableFingerprint?: string;
	lastGoodAnalysisFullFingerprint?: string;
	deferredAnalysisFullFingerprint?: string;
	deferredAnalysisStableFingerprint?: string;
	changedRangesCount?: number;
};

export async function getDocumentRuntimeDebug(
	uris: string[]
): Promise<DocumentRuntimeDebugEntry[]> {
	const response = await waitFor(
		() =>
			vscode.commands.executeCommand<{ documents?: DocumentRuntimeDebugEntry[] }>(
				'nsf._getDocumentRuntimeDebug',
				{ uris }
			),
		(value) => Array.isArray(value?.documents),
		'document runtime debug'
	);
	return response?.documents ?? [];
}

export async function withTemporaryIntellisionPath<T>(
	paths: string[],
	fn: () => Promise<T>
): Promise<T> {
	await waitForClientReady('client ready before intellisionPath update');
	const configuration = vscode.workspace.getConfiguration('nsf');
	const inspected = configuration.inspect<string[]>('intellisionPath');
	const originalPaths = inspected?.workspaceValue ?? [];
	await configuration.update('intellisionPath', paths, vscode.ConfigurationTarget.Workspace);
	const commands = await vscode.commands.getCommands(true);
	if (commands.includes('nsf.restartServer')) {
		await vscode.commands.executeCommand('nsf.restartServer');
	}
	await waitForIndexingIdle('indexing idle after intellisionPath update');
	try {
		return await fn();
	} finally {
		await configuration.update('intellisionPath', originalPaths, vscode.ConfigurationTarget.Workspace);
		if (commands.includes('nsf.restartServer')) {
			await vscode.commands.executeCommand('nsf.restartServer');
		}
		await waitForIndexingIdle('indexing idle after intellisionPath restore');
		await new Promise((resolve) => setTimeout(resolve, 150));
	}
}

export async function touchDocument(document: vscode.TextDocument): Promise<void> {
	const marker = '/*nsf-touch*/';
	let insertPosition = document.positionAt(document.getText().length);
	for (let line = 0; line < document.lineCount; line++) {
		const lineText = document.lineAt(line).text;
		const trimmed = lineText.trim();
		if (
			trimmed.length === 0 ||
			trimmed.startsWith('//') ||
			trimmed.startsWith('/*') ||
			trimmed.startsWith('*') ||
			trimmed.startsWith('*/')
		) {
			continue;
		}
		insertPosition = new vscode.Position(line, lineText.length);
		break;
	}

	const insertEdit = new vscode.WorkspaceEdit();
	insertEdit.insert(document.uri, insertPosition, marker);
	await vscode.workspace.applyEdit(insertEdit);

	const updatedDocument = await vscode.workspace.openTextDocument(document.uri);
	const removeEdit = new vscode.WorkspaceEdit();
	removeEdit.delete(
		document.uri,
		new vscode.Range(
			insertPosition,
			insertPosition.translate(0, marker.length)
		)
	);
	await vscode.workspace.applyEdit(removeEdit);
	void updatedDocument;
}

export function toFsPath(location: ProviderLocation): string {
	if ('targetUri' in location) {
		return location.targetUri.fsPath;
	}
	return location.uri.fsPath;
}

export function toRange(location: ProviderLocation): vscode.Range {
	if ('targetRange' in location) {
		return location.targetRange;
	}
	return location.range;
}

export function hoverToText(hovers: vscode.Hover[]): string {
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

export function flattenSymbolNames(symbols: SymbolLike[]): string[] {
	const names: string[] = [];
	for (const symbol of symbols) {
		names.push(symbol.name);
		if ('children' in symbol && Array.isArray(symbol.children)) {
			names.push(...flattenSymbolNames(symbol.children));
		}
	}
	return names;
}

export function findOffset(document: vscode.TextDocument, needle: string, occurrence = 1): number {
	let fromIndex = 0;
	let foundIndex = -1;
	for (let current = 0; current < occurrence; current++) {
		foundIndex = document.getText().indexOf(needle, fromIndex);
		assert.notStrictEqual(foundIndex, -1, `Unable to find occurrence ${occurrence} of '${needle}'.`);
		fromIndex = foundIndex + needle.length;
	}
	return foundIndex;
}

export function positionOf(
	document: vscode.TextDocument,
	needle: string,
	occurrence = 1,
	characterOffset = 0
): vscode.Position {
	const offset = findOffset(document, needle, occurrence) + characterOffset;
	return document.positionAt(offset);
}

export function countWorkspaceEdits(edit: vscode.WorkspaceEdit): number {
	let count = 0;
	for (const [, edits] of edit.entries()) {
		count += edits.length;
	}
	return count;
}

export function diagnosticCodeText(diagnostic: vscode.Diagnostic): string {
	const code = diagnostic.code;
	if (typeof code === 'string' || typeof code === 'number') {
		return String(code);
	}
	if (!code || typeof code !== 'object') {
		return '';
	}
	const value = (code as { value?: unknown }).value;
	if (typeof value === 'string' || typeof value === 'number') {
		return String(value);
	}
	return '';
}

export function getCompletionItems(
	result: vscode.CompletionList | vscode.CompletionItem[] | undefined
): vscode.CompletionItem[] {
	if (!result) {
		return [];
	}
	return Array.isArray(result) ? result : result.items;
}

export function isEmptyDefinitionResult(value: ProviderLocation[] | undefined): boolean {
	return !value || value.length === 0;
}

export async function waitForCompletionLabels(
	document: vscode.TextDocument,
	position: vscode.Position,
	expectedLabels: string[],
	label: string
): Promise<vscode.CompletionItem[]> {
	const result = await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				document.uri,
				position
			),
		(value) => {
			const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
			return expectedLabels.every((expected) => labels.has(expected));
		},
		label
	);
	return getCompletionItems(result);
}

export async function waitForHoverText(
	document: vscode.TextDocument,
	position: vscode.Position,
	isReady: (text: string) => boolean,
	label: string
): Promise<vscode.Hover[]> {
	return waitFor(
		() =>
			vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				document.uri,
				position
			),
		(value) => {
			if (!Array.isArray(value) || value.length === 0) {
				return false;
			}
			return isReady(hoverToText(value));
		},
		label
	);
}

export async function waitForDiagnostics(
	document: vscode.TextDocument,
	isReady: (diagnostics: readonly vscode.Diagnostic[]) => boolean,
	label: string
): Promise<readonly vscode.Diagnostic[]> {
	return waitFor(
		() => vscode.languages.getDiagnostics(document.uri),
		(value) => Array.isArray(value) && isReady(value),
		label
	);
}

export async function waitForDiagnosticsWithTouches(
	document: vscode.TextDocument,
	isReady: (diagnostics: readonly vscode.Diagnostic[]) => boolean,
	label: string,
	attempts = 24,
	delayMs = 1000,
	touchInterval = 4
): Promise<readonly vscode.Diagnostic[]> {
	let lastValue: readonly vscode.Diagnostic[] = [];
	for (let attempt = 0; attempt < attempts; attempt++) {
		lastValue = vscode.languages.getDiagnostics(document.uri);
		if (Array.isArray(lastValue) && isReady(lastValue)) {
			return lastValue;
		}
		if (attempt > 0 && touchInterval > 0 && (attempt + 1) % touchInterval === 0) {
			await touchDocument(document);
		}
		await new Promise((resolve) => setTimeout(resolve, delayMs));
	}
	throw new Error(`Timed out waiting for ${label}.`);
}
