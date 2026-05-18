import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

export type ProviderLocation = vscode.Location | vscode.LocationLink;
export type SymbolLike = vscode.DocumentSymbol | vscode.SymbolInformation;
type Awaitable<T> = T | Thenable<T> | PromiseLike<T>;

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
export const repoDescribe = testMode === 'repo' ? describe : describe.skip;

export type WaitForOptions = {
	// Total attempts before timing out.
	attempts?: number;
	// Delay between attempts in milliseconds.
	delayMs?: number;
};

async function waitForWithOptions<T>(
	producer: () => Awaitable<T>,
	isReady: (value: T) => boolean,
	label: string,
	options?: WaitForOptions
): Promise<T> {
	const attempts = options?.attempts ?? 60;
	const delayMs = options?.delayMs ?? 500;
	let lastValue: T | undefined;
	let lastError: unknown;
	for (let attempt = 0; attempt < attempts; attempt++) {
		try {
			lastValue = await Promise.resolve(producer());
			if (isReady(lastValue)) {
				return lastValue;
			}
			lastError = undefined;
		} catch (error) {
			lastError = error;
		}
		await new Promise((resolve) => setTimeout(resolve, delayMs));
	}
	const suffix =
		lastError !== undefined
			? ` Last error: ${lastError instanceof Error ? lastError.message : String(lastError)}`
			: '';
	throw new Error(`Timed out waiting for ${label}.${suffix}`);
}

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
	return waitForWithOptions(producer, isReady, label);
}

export async function waitForFast<T>(
	producer: () => Awaitable<T>,
	isReady: (value: T) => boolean,
	label: string,
	options?: WaitForOptions
): Promise<T> {
	const merged: WaitForOptions = { attempts: 600, delayMs: 50, ...options };
	return waitForWithOptions(producer, isReady, label, merged);
}

// Waits for an intermediate transient state to occur before a terminal state is observed.
// This is useful when the intermediate state can be short-lived and a slow polling loop can miss it.
export async function waitForObservedBefore<T>(
	producer: () => Awaitable<T>,
	didObserve: (value: T) => boolean,
	isTerminal: (value: T) => boolean,
	label: string,
	options?: WaitForOptions
): Promise<T> {
	const attempts = options?.attempts ?? 1200;
	const delayMs = options?.delayMs ?? 25;
	let lastValue: T | undefined;
	let lastError: unknown;
	for (let attempt = 0; attempt < attempts; attempt++) {
		try {
			lastValue = await Promise.resolve(producer());
			lastError = undefined;
		} catch (error) {
			lastError = error;
			await new Promise((resolve) => setTimeout(resolve, delayMs));
			continue;
		}

		if (didObserve(lastValue)) {
			return lastValue;
		}
		if (isTerminal(lastValue)) {
			throw new Error(`Observed terminal state before ${label}.`);
		}
		await new Promise((resolve) => setTimeout(resolve, delayMs));
	}
	const suffix =
		lastError !== undefined
			? ` Last error: ${lastError instanceof Error ? lastError.message : String(lastError)}`
			: '';
	throw new Error(`Timed out waiting for ${label}.${suffix}`);
}

function isIndexingStateStableFromInternalStatus(indexingState: any): boolean {
	if (!indexingState || typeof indexingState !== 'object') {
		return false;
	}
	if (indexingState.state !== 'Idle') {
		return false;
	}
	const queued = indexingState.pending?.queuedTasks ?? 0;
	const running = indexingState.pending?.runningWorkers ?? 0;
	return queued === 0 && running === 0;
}

function isInlayFallbackConditionFromInternalStatus(status: any): boolean {
	const clientState = status?.clientState ?? '';
	if (clientState && clientState !== 'ready') {
		return true;
	}
	// Inlay provider falls back when indexing state is not stable.
	return !isIndexingStateStableFromInternalStatus(status?.indexingState);
}

export async function waitForInlayFallbackPreconditionForTests(
	label: string,
	options?: WaitForOptions
): Promise<any> {
	const merged: WaitForOptions = { attempts: 1200, delayMs: 25, ...options };
	return waitForWithOptions(
		() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
		(status) => isInlayFallbackConditionFromInternalStatus(status),
		label,
		merged
	);
}

export async function waitForInlayHintsDuringClientFallback(
	uri: vscode.Uri,
	range: vscode.Range,
	label: string,
	options?: WaitForOptions
): Promise<vscode.InlayHint[]> {
	const merged: WaitForOptions = { attempts: 1200, delayMs: 25, ...options };
	const result = await waitForWithOptions<vscode.InlayHint[] | undefined>(
		async () => {
			const statusBefore = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
			if (!isInlayFallbackConditionFromInternalStatus(statusBefore)) {
				return undefined;
			}

			const hints = await vscode.commands.executeCommand<vscode.InlayHint[]>(
				'vscode.executeInlayHintProvider',
				uri,
				range
			);
			return hints;
		},
		(value) => Array.isArray(value) && value.length > 0,
		label,
		merged
	);
	assert.ok(Array.isArray(result) && result.length > 0, `Expected inlay hints result for ${label}.`);
	return result as vscode.InlayHint[];
}

export async function waitForIndexingIdle(label = 'indexing idle', options?: WaitForOptions): Promise<any> {
	const commands = await vscode.commands.getCommands(true);
	if (!commands.includes('nsf._getInternalStatus')) {
		const activationDoc = await openFixture('module_suite.nsf');
		await vscode.commands.executeCommand('workbench.action.closeActiveEditor');
		void activationDoc;
	}
	return waitForWithOptions(
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
		label,
		options
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
	activeUnitProfileDefines?: Record<string, number>;
	activeUnitProfileShaderKey?: string;
	activeUnitProfileSourcePath?: string;
	activeUnitWorkspaceSummaryVersion?: number;
	hasCurrentDocSemanticSnapshot?: boolean;
	hasLastGoodCurrentDocSemanticSnapshot?: boolean;
	hasDeferredDocSnapshot?: boolean;
	currentDocSemanticAnalysisFullFingerprint?: string;
	currentDocSemanticAnalysisStableFingerprint?: string;
	lastGoodCurrentDocSemanticAnalysisFullFingerprint?: string;
	deferredAnalysisFullFingerprint?: string;
	deferredAnalysisStableFingerprint?: string;
	deferredHasSemanticSnapshot?: boolean;
	deferredHasSemanticTokensFull?: boolean;
	deferredHasDocumentSymbols?: boolean;
	deferredHasFullDiagnostics?: boolean;
	deferredObservedDiagnosticsReadyBeforeInlayFull?: boolean;
	deferredHasInlayHintsFull?: boolean;
	deferredSemanticTokensRangeCacheCount?: number;
	deferredInlayRangeCacheCount?: number;
	changedRangesCount?: number;
	interactiveVisibilityFingerprint?: string;
	globalContextReady?: boolean;
	localStructuralSnapshotReady?: boolean;
	localStructuralPublishObserved?: boolean;
	localStructuralChangedWindowOnly?: boolean;
	localStructuralChangedWindowStartLine?: number;
	localStructuralChangedWindowEndLine?: number;
	currentDocSemanticSnapshotReady?: boolean;
	lastDiagnosticsPublishLayer?: string;
};

export type InteractiveRuntimeDebugResponse = {
	uri?: string;
	lastQueryKind?: string;
	lastResolvedLayer?: string;
	lastSymbol?: string;
	lastMemberBaseSymbol?: string;
	lastMemberBaseResolutionPath?: string;
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

export async function waitForDocumentRuntimeDebugEntry(
	uri: string,
	isReady: (entry: DocumentRuntimeDebugEntry | undefined) => boolean,
	label: string,
	options?: WaitForOptions
): Promise<DocumentRuntimeDebugEntry | undefined> {
	const entries = await waitForWithOptions(
		() => getDocumentRuntimeDebug([uri]),
		(value) => isReady(value[0]),
		label,
		options
	);
	return entries[0];
}

export async function waitForActiveUnitAndVisibilityReadyForTests(
	uri: string,
	expectedActiveUnitPathSuffix: string,
	label: string
): Promise<DocumentRuntimeDebugEntry> {
	const expectedUri = uri.toLowerCase();
	const expectedSuffix = expectedActiveUnitPathSuffix.toLowerCase();

	const response = await waitFor(
		() =>
			vscode.commands.executeCommand<{ documents?: DocumentRuntimeDebugEntry[] }>(
				'nsf._getDocumentRuntimeDebug',
				{ uris: [uri] }
			),
		(value) => {
			const doc = value?.documents?.[0];
			return (
				Array.isArray(value?.documents) &&
				typeof doc?.uri === 'string' &&
				doc.uri.toLowerCase() === expectedUri &&
				typeof doc.activeUnitPath === 'string' &&
				doc.activeUnitPath.toLowerCase().endsWith(expectedSuffix) &&
				typeof doc.activeUnitIncludeClosureFingerprint === 'string' &&
				doc.activeUnitIncludeClosureFingerprint.length > 0 &&
				typeof doc.interactiveVisibilityFingerprint === 'string' &&
				doc.interactiveVisibilityFingerprint.length > 0
			);
		},
		label
	);

	return response.documents![0];
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
	await waitForIndexingIdle('indexing idle after intellisionPath update');
	try {
		return await fn();
	} finally {
		await configuration.update('intellisionPath', originalPaths, vscode.ConfigurationTarget.Workspace);
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

function assertEditorTargetsDocument(editor: vscode.TextEditor, document: vscode.TextDocument): void {
	assert.strictEqual(
		editor.document.uri.toString(),
		document.uri.toString(),
		`Expected editor document ${editor.document.uri.toString()} to match target ${document.uri.toString()}.`
	);
}

/**
 * Uses VS Code's native `type` command after explicitly restoring editor focus.
 *
 * This helper exists for auto-trigger scenarios where the platform typing path
 * itself is part of the behavior under test.
 */
export async function typeWithEditorFocusForTests(
	editor: vscode.TextEditor,
	text: string
): Promise<void> {
	assert.ok(text.length > 0, 'Expected non-empty text for typeWithEditorFocusForTests.');
	const document = editor.document;
	assertEditorTargetsDocument(editor, document);

	await vscode.window.showTextDocument(document, {
		preview: false,
		preserveFocus: false,
		viewColumn: editor.viewColumn
	});
	await vscode.commands.executeCommand('workbench.action.focusActiveEditorGroup');
	await waitForFast(
		() => Promise.resolve(vscode.window.activeTextEditor?.document.uri.toString()),
		(value) => value === document.uri.toString(),
		`focused editor for native typing: ${document.uri.toString()}`
	);

	const initialVersion = document.version;
	await vscode.commands.executeCommand('type', { text });
	await waitForFast(
		() => Promise.resolve(document.version),
		(value) => value > initialVersion,
		`native typing edit applied: ${document.uri.toString()}`
	);
}

/**
 * Deterministic replacement for `vscode.commands.executeCommand('type')` in tests
 * that only need text mutation, not editor-native auto-trigger behavior.
 *
 * `type` is focus-sensitive and can become a no-op under repo-mode and replay runner scenarios.
 * We instead apply an explicit edit against the provided editor and update the selection ourselves.
 */
export async function typeTextForTests(editor: vscode.TextEditor, text: string): Promise<void> {
	assert.ok(text.length > 0, 'Expected non-empty text for typeTextForTests.');

	const document = editor.document;
	const selection = editor.selection;
	assertEditorTargetsDocument(editor, document);

	const startOffset = document.offsetAt(selection.start);

	const ok = await editor.edit(
		(editBuilder) => {
			editBuilder.replace(new vscode.Range(selection.start, selection.end), text);
		},
		{ undoStopBefore: false, undoStopAfter: false }
	);
	assert.ok(ok, 'Expected editor edit to succeed in typeTextForTests.');

	const updatedDocument = editor.document;
	const nextPosition = updatedDocument.positionAt(startOffset + text.length);
	editor.selection = new vscode.Selection(nextPosition, nextPosition);
}

/**
 * Deterministic replacement for `vscode.commands.executeCommand('deleteLeft')` in tests.
 *
 * Applies deletions relative to the provided editor selection/cursor and updates selection explicitly.
 */
export async function deleteLeftForTests(editor: vscode.TextEditor, count = 1): Promise<void> {
	assert.ok(count >= 0, `Expected non-negative delete count, got ${count}.`);

	for (let index = 0; index < count; index++) {
		const document = editor.document;
		const selection = editor.selection;
		assertEditorTargetsDocument(editor, document);

		// `deleteLeft` deletes the current selection as a single operation.
		if (!selection.isEmpty) {
			const startOffset = document.offsetAt(selection.start);
			const ok = await editor.edit(
				(editBuilder) => {
					editBuilder.delete(new vscode.Range(selection.start, selection.end));
				},
				{ undoStopBefore: false, undoStopAfter: false }
			);
			assert.ok(ok, 'Expected editor edit to succeed in deleteLeftForTests(selection).');

			const updatedDocument = editor.document;
			const nextPosition = updatedDocument.positionAt(startOffset);
			editor.selection = new vscode.Selection(nextPosition, nextPosition);
			continue;
		}

		const cursorOffset = document.offsetAt(selection.active);
		if (cursorOffset <= 0) {
			return;
		}

		const startOffset = cursorOffset - 1;
		const start = document.positionAt(startOffset);
		const end = document.positionAt(cursorOffset);

		const ok = await editor.edit(
			(editBuilder) => {
				editBuilder.delete(new vscode.Range(start, end));
			},
			{ undoStopBefore: false, undoStopAfter: false }
		);
		assert.ok(ok, 'Expected editor edit to succeed in deleteLeftForTests(char).');

		const updatedDocument = editor.document;
		const nextPosition = updatedDocument.positionAt(startOffset);
		editor.selection = new vscode.Selection(nextPosition, nextPosition);
	}
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

export async function getInteractiveRuntimeDebug(
	uri: string
): Promise<InteractiveRuntimeDebugResponse> {
	const expectedUri = uri.toLowerCase();
	return waitFor(
		() =>
			vscode.commands.executeCommand<InteractiveRuntimeDebugResponse>(
				'nsf._getInteractiveRuntimeDebug',
				{ uri }
			),
		(value) => typeof value?.uri === 'string' && value.uri.toLowerCase() === expectedUri,
		'interactive runtime debug'
	);
}

export type MemberBaseResolutionDebugResponse = {
	resolved?: boolean;
	typeName?: string;
	resolutionPath?: string;
	base?: string;
};

export async function resolveMemberBaseForTests(
	uri: string,
	line: number,
	character: number
): Promise<MemberBaseResolutionDebugResponse> {
	return waitFor(
		() =>
			vscode.commands.executeCommand<MemberBaseResolutionDebugResponse>(
				'nsf._resolveMemberBaseForTests',
				{ uri, line, character }
			),
		(value) => typeof value === 'object' && value !== null,
		'member-base resolution debug'
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
