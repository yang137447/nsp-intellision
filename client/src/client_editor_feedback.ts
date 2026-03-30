import * as fs from 'fs';
import * as path from 'path';

import {
	CancellationToken,
	DecorationRangeBehavior,
	type ExtensionContext,
	EventEmitter,
	InlayHint,
	InlayHintKind,
	languages,
	Position,
	Range,
	type TextDocument,
	type TextEditor,
	window,
	workspace
} from 'vscode';
import type { LanguageClient } from 'vscode-languageclient/node';

import { buildRuntimeSettings, normalizeIncludePaths } from './client_config_sync';
import { LSP_METHOD_KEYS } from './lsp_method_keys';
import {
	formatIndexingProgress,
	hasServerIndexingWork,
	isIndexingStateStable,
	normalizeIndexingState,
	type ServerIndexingState
} from './client_indexing_status';
import { computeSingleFileIncludePaths } from './single_file_config';

export type EditorFeedbackSnapshot = {
	lastIndexingState: ServerIndexingState | undefined;
	lastIndexingEvent: { kind: string; state: string; phase?: string; symbol?: string; token?: number } | undefined;
	activeIndexingCount: number;
	indexingMessage: string;
	indexingUnit: string;
	initialInlayRefreshTriggerCount: number;
	indexSettledInlayRefreshTriggerCount: number;
	pendingInitialInlayRefreshAfterIndex: boolean;
	pendingInlayRefreshAfterIndexActivity: boolean;
};

type EditorFeedbackOptions = {
	context: ExtensionContext;
	isTestMode: boolean;
	isPerfTestMode: boolean;
	getClient: () => LanguageClient | undefined;
	ensureClientStarted: (forceRestart: boolean) => Promise<void>;
	beginRpcActivity: (method: string) => void;
	endRpcActivity: () => void;
	appendClientTrace: (message: string) => void;
	logClient: (message: string) => void;
	pushRecentClientError: (source: string, error: unknown) => void;
	isTransientClientRpcError: (error: unknown) => boolean;
	onStateChanged: () => void;
};

export type EditorFeedbackController = {
	getSnapshot: () => EditorFeedbackSnapshot;
	sendInlayHintRequest: (documentUri: string, range: Range, token?: CancellationToken) => Promise<any[]>;
	sendDocumentSymbolRequest: (documentUri: string, token?: CancellationToken) => Promise<any[]>;
	sendWorkspaceSymbolRequest: (query: string, token?: CancellationToken) => Promise<any[]>;
	collectSpamRequestResults: <T>(tasks: Array<Promise<T>>) => Promise<{ completed: number; cancelled: number; failed: number }>;
	fetchIndexingState: (force?: boolean) => Promise<ServerIndexingState | undefined>;
	requestIndexRebuild: (reason: string, clearDiskCache: boolean) => Promise<ServerIndexingState | undefined>;
	refreshIndexingStateAndMaybeTriggerInlay: () => Promise<void>;
	startGitStormPolling: () => void;
	stopGitStormPolling: () => void;
	scheduleIncludeUnderlineRefresh: () => void;
	scheduleInlayHintsRefresh: (editor?: TextEditor) => void;
	scheduleInlayHintsRefreshWave: (delaysMs: number[]) => boolean;
	updateRuntimeConfiguration: (filePath?: string) => Promise<void>;
	handleIndexingNotification: (payload: {
		state?: string;
		kind?: string;
		phase?: string;
		unit?: string;
		symbol?: string;
		token?: number;
		visited?: number;
		fileBudget?: number;
	}) => void;
	handleIndexingStateChangedNotification: (payload: unknown) => void;
	handleInlayHintsChangedNotification: (payload: { uri?: string }) => void;
	fireInlayHintsChanged: () => void;
	handleClientReady: () => void;
	handleClientStopped: () => void;
};

export function createEditorFeedbackController(options: EditorFeedbackOptions): EditorFeedbackController {
	let lastIndexingEvent: { kind: string; state: string; phase?: string; symbol?: string; token?: number } | undefined;
	let lastIndexingState: ServerIndexingState | undefined;
	let pendingIndexingStateRequest: Promise<ServerIndexingState | undefined> | undefined;
	const activeIndexingTokens = new Set<number>();
	let indexingMessage = '';
	let indexingUnit = '';
	let pendingInitialInlayRefreshAfterIndex = false;
	let pendingInlayRefreshAfterIndexActivity = false;
	let initialInlayRefreshTriggerCount = 0;
	let indexSettledInlayRefreshTriggerCount = 0;
	let gitStormPollingTimer: NodeJS.Timeout | undefined;
	let lastSentSingleFileSettingsKey = '';

	const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));
	const inlayHintsChanged = new EventEmitter<void>();
	const inlayPrefetchLines = 80;
	let inlayRefreshTimer: NodeJS.Timeout | undefined;
	let includeUnderlineRefreshTimer: NodeJS.Timeout | undefined;
	const inlayDeferredRefreshTimers = new Set<NodeJS.Timeout>();
	const includeValidDecoration = window.createTextEditorDecorationType({
		textDecoration: 'underline',
		rangeBehavior: DecorationRangeBehavior.ClosedClosed
	});
	options.context.subscriptions.push(includeValidDecoration);
	options.context.subscriptions.push(inlayHintsChanged);
	options.context.subscriptions.push({
		dispose: () => {
			if (inlayRefreshTimer) {
				clearTimeout(inlayRefreshTimer);
				inlayRefreshTimer = undefined;
			}
			if (includeUnderlineRefreshTimer) {
				clearTimeout(includeUnderlineRefreshTimer);
				includeUnderlineRefreshTimer = undefined;
			}
			stopGitStormPolling();
			for (const timeout of inlayDeferredRefreshTimers) {
				clearTimeout(timeout);
			}
			inlayDeferredRefreshTimers.clear();
		}
	});

	const isInlayTargetEditor = (editor: TextEditor | undefined): editor is TextEditor => {
		if (!editor) {
			return false;
		}
		const document = editor.document;
		if (document.uri.scheme !== 'file') {
			return false;
		}
		return document.languageId === 'hlsl' || document.languageId === 'nsf';
	};

	const isIncludeValidUnderlineEnabled = (): boolean =>
		workspace.getConfiguration('nsf').get<boolean>('include.validUnderline', true);

	const getConfiguredShaderExtensions = (): string[] => {
		const configured = workspace.getConfiguration('nsf').get<string[]>(
			'shaderFileExtensions',
			['.nsf', '.hlsl', '.hlsli', '.fx', '.usf', '.ush']
		);
		const normalized: string[] = [];
		const seen = new Set<string>();
		for (const item of configured) {
			const trimmed = item.trim();
			if (!trimmed) {
				continue;
			}
			const ext = trimmed.startsWith('.') ? trimmed : `.${trimmed}`;
			const key = ext.toLowerCase();
			if (seen.has(key)) {
				continue;
			}
			seen.add(key);
			normalized.push(ext);
		}
		return normalized;
	};

	const resolveIncludeCandidatesForDocument = (document: TextDocument, includePath: string): string[] => {
		const isAbsolutePath = (value: string): boolean => {
			if (path.isAbsolute(value)) {
				return true;
			}
			return /^[a-zA-Z]:[\\/]/.test(value);
		};
		const workspaceFolders = (workspace.workspaceFolders ?? []).map((item) => item.uri.fsPath);
		const includePaths = workspace.getConfiguration('nsf').get<string[]>('intellisionPath', []);
		const candidates: string[] = [];
		if (isAbsolutePath(includePath)) {
			candidates.push(includePath);
		} else {
			candidates.push(path.join(path.dirname(document.uri.fsPath), includePath));
			for (const inc of includePaths) {
				const trimmed = inc.trim();
				if (!trimmed) {
					continue;
				}
				if (isAbsolutePath(trimmed)) {
					candidates.push(path.join(trimmed, includePath));
				}
			}
			for (const folder of workspaceFolders) {
				for (const inc of includePaths) {
					const trimmed = inc.trim();
					if (!trimmed) {
						continue;
					}
					const base = isAbsolutePath(trimmed) ? trimmed : path.join(folder, trimmed);
					candidates.push(path.join(base, includePath));
				}
			}
		}
		const hasExtension = path.extname(includePath).length > 0;
		const extensions = getConfiguredShaderExtensions();
		const expanded: string[] = [];
		const seen = new Set<string>();
		for (const candidate of candidates) {
			if (hasExtension) {
				const key = path.normalize(candidate).toLowerCase();
				if (seen.has(key)) {
					continue;
				}
				seen.add(key);
				expanded.push(candidate);
				continue;
			}
			for (const ext of extensions) {
				const withExt = `${candidate}${ext}`;
				const key = path.normalize(withExt).toLowerCase();
				if (seen.has(key)) {
					continue;
				}
				seen.add(key);
				expanded.push(withExt);
			}
		}
		return expanded;
	};

	const computeValidIncludeRanges = (document: TextDocument): Range[] => {
		const ranges: Range[] = [];
		const lines = document.getText().split(/\r?\n/);
		for (let lineIndex = 0; lineIndex < lines.length; lineIndex++) {
			const line = lines[lineIndex];
			const matched = line.match(/^\s*#\s*include\s*[<"]([^">]+)[>"]/);
			if (!matched) {
				continue;
			}
			const includePath = matched[1];
			const fullMatch = matched[0];
			const relativeStart = fullMatch.indexOf(includePath);
			if (relativeStart < 0) {
				continue;
			}
			const start = (matched.index ?? 0) + relativeStart;
			const end = start + includePath.length;
			const candidates = resolveIncludeCandidatesForDocument(document, includePath);
			const found = candidates.some((candidate) => {
				try {
					return fs.existsSync(candidate);
				} catch {
					return false;
				}
			});
			if (!found) {
				continue;
			}
			ranges.push(new Range(lineIndex, start, lineIndex, end));
		}
		return ranges;
	};

	const applyIncludeUnderlineForEditor = (editor: TextEditor | undefined): void => {
		if (!isInlayTargetEditor(editor)) {
			return;
		}
		if (!isIncludeValidUnderlineEnabled()) {
			editor.setDecorations(includeValidDecoration, []);
			return;
		}
		editor.setDecorations(includeValidDecoration, computeValidIncludeRanges(editor.document));
	};

	const scheduleIncludeUnderlineRefresh = (): void => {
		if (includeUnderlineRefreshTimer) {
			clearTimeout(includeUnderlineRefreshTimer);
		}
		includeUnderlineRefreshTimer = setTimeout(() => {
			includeUnderlineRefreshTimer = undefined;
			for (const editor of window.visibleTextEditors) {
				applyIncludeUnderlineForEditor(editor);
			}
		}, 60);
	};

	const scheduleInlayHintsRefresh = (editor?: TextEditor): void => {
		if (!isInlayTargetEditor(editor)) {
			return;
		}
		if (inlayRefreshTimer) {
			clearTimeout(inlayRefreshTimer);
		}
		inlayRefreshTimer = setTimeout(() => {
			inlayRefreshTimer = undefined;
			inlayHintsChanged.fire();
		}, 40);
	};

	const refreshInlayHintsForVisibleEditors = (): void => {
		let fired = false;
		for (const editor of window.visibleTextEditors) {
			if (!isInlayTargetEditor(editor)) {
				continue;
			}
			scheduleInlayHintsRefresh(editor);
			fired = true;
		}
		if (!fired) {
			scheduleInlayHintsRefresh(window.activeTextEditor);
		}
	};

	const scheduleInlayHintsRefreshWave = (delaysMs: number[]): boolean => {
		const hasTarget =
			window.visibleTextEditors.some((editor) => isInlayTargetEditor(editor)) ||
			isInlayTargetEditor(window.activeTextEditor);
		if (!hasTarget) {
			return false;
		}
		for (const delay of delaysMs) {
			const timeout = setTimeout(() => {
				inlayDeferredRefreshTimers.delete(timeout);
				refreshInlayHintsForVisibleEditors();
			}, Math.max(0, delay));
			inlayDeferredRefreshTimers.add(timeout);
		}
		return true;
	};

	const tryCompleteInitialInlayRefresh = (delaysMs: number[]): boolean => {
		if (!pendingInitialInlayRefreshAfterIndex && !pendingInlayRefreshAfterIndexActivity) {
			return false;
		}
		if (!scheduleInlayHintsRefreshWave(delaysMs)) {
			return false;
		}
		if (pendingInitialInlayRefreshAfterIndex) {
			initialInlayRefreshTriggerCount++;
		}
		pendingInitialInlayRefreshAfterIndex = false;
		pendingInlayRefreshAfterIndexActivity = false;
		indexSettledInlayRefreshTriggerCount++;
		return true;
	};

	const sendInlayHintRequest = async (
		documentUri: string,
		range: Range,
		token?: CancellationToken
	): Promise<any[]> => {
		const client = options.getClient();
		if (!client) {
			return [];
		}
		try {
			const response = await client.sendRequest<any[]>(
				LSP_METHOD_KEYS.inlayHint,
				{
					textDocument: { uri: documentUri },
					range: {
						start: { line: range.start.line, character: range.start.character },
						end: { line: range.end.line, character: range.end.character }
					}
				},
				token
			);
			return Array.isArray(response) ? response : [];
		} catch (error) {
			if (token?.isCancellationRequested || options.isTransientClientRpcError(error)) {
				return [];
			}
			throw error;
		}
	};

	const sendDocumentSymbolRequest = async (
		documentUri: string,
		token?: CancellationToken
	): Promise<any[]> => {
		const client = options.getClient();
		if (!client) {
			return [];
		}
		try {
			const response = await client.sendRequest<any[]>(
				LSP_METHOD_KEYS.documentSymbol,
				{
					textDocument: { uri: documentUri }
				},
				token
			);
			return Array.isArray(response) ? response : [];
		} catch (error) {
			if (token?.isCancellationRequested || options.isTransientClientRpcError(error)) {
				return [];
			}
			throw error;
		}
	};

	const sendWorkspaceSymbolRequest = async (
		query: string,
		token?: CancellationToken
	): Promise<any[]> => {
		const client = options.getClient();
		if (!client) {
			return [];
		}
		try {
			const response = await client.sendRequest<any[]>(
				LSP_METHOD_KEYS.workspaceSymbol,
				{ query },
				token
			);
			return Array.isArray(response) ? response : [];
		} catch (error) {
			if (token?.isCancellationRequested || options.isTransientClientRpcError(error)) {
				return [];
			}
			throw error;
		}
	};

	const collectSpamRequestResults = async <T>(tasks: Array<Promise<T>>): Promise<{
		completed: number;
		cancelled: number;
		failed: number;
	}> => {
		let completed = 0;
		let cancelled = 0;
		let failed = 0;
		for (const task of tasks) {
			try {
				await task;
				completed++;
			} catch (error) {
				const message = (error as Error).message ?? String(error);
				if (message.includes('-32800') || message.toLowerCase().includes('cancel')) {
					cancelled++;
				} else {
					failed++;
				}
			}
		}
		return { completed, cancelled, failed };
	};

	const fetchIndexingState = async (force = false): Promise<ServerIndexingState | undefined> => {
		const client = options.getClient();
		if (!client || !client.initializeResult) {
			return undefined;
		}
		if (!force && pendingIndexingStateRequest) {
			return pendingIndexingStateRequest;
		}
		const task = (async () => {
			try {
				options.beginRpcActivity('nsf/getIndexingState');
				const payload = await client.sendRequest<unknown>('nsf/getIndexingState');
				const parsed = normalizeIndexingState(payload);
				if (parsed) {
					lastIndexingState = parsed;
					options.onStateChanged();
				}
				return parsed;
			} catch (error) {
				if (options.isTransientClientRpcError(error)) {
					return undefined;
				}
				options.appendClientTrace(`fetch nsf/getIndexingState failed ${(error as Error).message ?? String(error)}`);
				options.logClient(`fetch nsf/getIndexingState failed ${(error as Error).message ?? String(error)}`);
				options.pushRecentClientError('nsf/getIndexingState', error);
				return undefined;
			} finally {
				options.endRpcActivity();
			}
		})();
		pendingIndexingStateRequest = task;
		task.finally(() => {
			if (pendingIndexingStateRequest === task) {
				pendingIndexingStateRequest = undefined;
			}
		});
		return task;
	};

	const waitForIndexingReasonToSettle = async (
		targetReason: string,
		previousEpoch: number | undefined,
		timeoutMs = 180000
	): Promise<ServerIndexingState | undefined> => {
		const deadline = Date.now() + timeoutMs;
		let sawRebuildStart = false;
		while (Date.now() < deadline) {
			const state = await fetchIndexingState(true);
			if (state) {
				const epoch = typeof state.epoch === 'number' ? state.epoch : undefined;
				const epochAdvanced =
					typeof previousEpoch === 'number' && typeof epoch === 'number' ? epoch > previousEpoch : false;
				if (epochAdvanced || (state.reason === targetReason && hasServerIndexingWork(state))) {
					sawRebuildStart = true;
				}
				if (
					state.reason === targetReason &&
					isIndexingStateStable(state) &&
					(sawRebuildStart || typeof previousEpoch !== 'number')
				) {
					return state;
				}
			}
			await sleep(350);
		}
		return undefined;
	};

	const requestIndexRebuild = async (
		reason: string,
		clearDiskCache: boolean
	): Promise<ServerIndexingState | undefined> => {
		const client = options.getClient();
		if (!client || !client.initializeResult) {
			return undefined;
		}
		const beforeState = lastIndexingState ?? (await fetchIndexingState(true));
		const previousEpoch = typeof beforeState?.epoch === 'number' ? beforeState.epoch : undefined;
		options.beginRpcActivity(LSP_METHOD_KEYS.rebuildIndex);
		try {
			await client.sendRequest<void>(LSP_METHOD_KEYS.rebuildIndex, {
				reason,
				clearDiskCache
			});
		} catch (error) {
			options.appendClientTrace(`send nsf/rebuildIndex failed ${(error as Error).message ?? String(error)}`);
			options.logClient(`send nsf/rebuildIndex failed ${(error as Error).message ?? String(error)}`);
			options.pushRecentClientError('nsf/rebuildIndex', error);
			throw error;
		} finally {
			options.endRpcActivity();
		}
		return waitForIndexingReasonToSettle(reason, previousEpoch);
	};

	const refreshIndexingStateAndMaybeTriggerInlay = async (): Promise<void> => {
		const state = await fetchIndexingState(true);
		if (!state) {
			return;
		}
		if (hasServerIndexingWork(state)) {
			pendingInlayRefreshAfterIndexActivity = true;
		}
		if (isIndexingStateStable(state)) {
			tryCompleteInitialInlayRefresh([0, 120]);
		}
	};

	const startGitStormPolling = (): void => {
		if (gitStormPollingTimer) {
			return;
		}
		gitStormPollingTimer = setInterval(() => {
			void fetchIndexingState(true).then((state) => {
				if (!state) {
					return;
				}
				if (isIndexingStateStable(state)) {
					if (gitStormPollingTimer) {
						clearInterval(gitStormPollingTimer);
						gitStormPollingTimer = undefined;
					}
					scheduleInlayHintsRefreshWave([0, 120]);
				}
			});
		}, 600);
	};

	const stopGitStormPolling = (): void => {
		if (!gitStormPollingTimer) {
			return;
		}
		clearInterval(gitStormPollingTimer);
		gitStormPollingTimer = undefined;
	};

	const isInAnyWorkspaceFolder = (filePath: string): boolean => {
		const folders = workspace.workspaceFolders;
		if (!folders || folders.length === 0) {
			return false;
		}
		const normalized = path.resolve(filePath).toLowerCase();
		for (const folder of folders) {
			const folderPath = path.resolve(folder.uri.fsPath).toLowerCase();
			if (normalized === folderPath) {
				return true;
			}
			const relative = path.relative(folderPath, normalized);
			if (relative && !relative.startsWith('..') && !path.isAbsolute(relative)) {
				return true;
			}
		}
		return false;
	};

	const updateRuntimeConfiguration = async (filePath?: string): Promise<void> => {
		const client = options.getClient();
		if (!client) {
			return;
		}
		let includePathsOverride: string[] | undefined;
		const configuredIncludePaths = normalizeIncludePaths(
			workspace.getConfiguration('nsf').get<string[]>('intellisionPath', [])
		);
		if (filePath && !isInAnyWorkspaceFolder(filePath)) {
			if (configuredIncludePaths.length > 0) {
				includePathsOverride = computeSingleFileIncludePaths(filePath, configuredIncludePaths);
			} else {
				includePathsOverride = [];
			}
		}
		const runtimeSettings = buildRuntimeSettings(options.isTestMode, options.isPerfTestMode, includePathsOverride);
		const key = JSON.stringify(runtimeSettings);
		if (key === lastSentSingleFileSettingsKey) {
			return;
		}
		lastSentSingleFileSettingsKey = key;
		options.beginRpcActivity(LSP_METHOD_KEYS.didChangeConfiguration);
		try {
			await client.sendNotification(LSP_METHOD_KEYS.didChangeConfiguration, {
				settings: {
					nsf: runtimeSettings
				}
			});
		} finally {
			options.endRpcActivity();
		}
	};

	const handleIndexingNotification = (payload: {
		state?: string;
		kind?: string;
		phase?: string;
		unit?: string;
		symbol?: string;
		token?: number;
		visited?: number;
		fileBudget?: number;
	}): void => {
		const state = payload?.state ?? '';
		const kind = payload?.kind ?? '';
		const phase = payload?.phase ?? '';
		const symbol = payload?.symbol ?? '';
		const unit = payload?.unit ?? '';
		const token = payload?.token;
		lastIndexingEvent = { kind, state, phase: phase || undefined, symbol: symbol || undefined, token };
		if (state === 'begin' && typeof token === 'number') {
			activeIndexingTokens.add(token);
			pendingInlayRefreshAfterIndexActivity = true;
		} else if (state === 'end' && typeof token === 'number') {
			activeIndexingTokens.delete(token);
			if (activeIndexingTokens.size === 0) {
				tryCompleteInitialInlayRefresh([80, 260]);
			}
		}
		const kindLabel = kind === 'workspaceIndex' ? 'workspace' : (kind === 'definitionFallback' ? 'def' : kind);
		const phaseLabel = phase === 'unitIncludeClosure' ? 'unit' : phase;
		const label = phaseLabel || kindLabel;
		let progress = '';
		if (typeof payload?.visited === 'number' && typeof payload?.fileBudget === 'number') {
			progress = `(${payload.visited}/${payload.fileBudget})`;
		} else if (typeof payload?.visited === 'number') {
			progress = `(${payload.visited})`;
		}
		indexingMessage = [label, symbol, progress].filter((value) => typeof value === 'string' && value.length > 0).join(' ');
		indexingUnit = unit ? path.basename(unit) : '';
		void fetchIndexingState(false).then((snapshot) => {
			if (snapshot && hasServerIndexingWork(snapshot)) {
				pendingInlayRefreshAfterIndexActivity = true;
			}
			if (snapshot && isIndexingStateStable(snapshot)) {
				tryCompleteInitialInlayRefresh([40, 160]);
			}
		});
		options.onStateChanged();
	};

	const handleIndexingStateChangedNotification = (payload: unknown): void => {
		const parsed = normalizeIndexingState(payload);
		if (!parsed) {
			return;
		}
		lastIndexingState = parsed;
		if (hasServerIndexingWork(parsed)) {
			pendingInlayRefreshAfterIndexActivity = true;
		}
		if (isIndexingStateStable(parsed)) {
			tryCompleteInitialInlayRefresh([0, 120]);
		}
		options.onStateChanged();
	};

	const handleInlayHintsChangedNotification = (payload: { uri?: string }): void => {
		const uri = typeof payload?.uri === 'string' ? payload.uri : '';
		if (!uri) {
			inlayHintsChanged.fire();
			return;
		}
		const editor = window.visibleTextEditors.find((item) => item.document.uri.toString() === uri);
		if (editor) {
			scheduleInlayHintsRefresh(editor);
		}
	};

	const handleClientReady = (): void => {
		pendingInitialInlayRefreshAfterIndex = true;
		pendingInlayRefreshAfterIndexActivity = true;
		void refreshIndexingStateAndMaybeTriggerInlay();
	};

	const handleClientStopped = (): void => {
		activeIndexingTokens.clear();
		indexingMessage = '';
		lastIndexingState = undefined;
		pendingIndexingStateRequest = undefined;
		pendingInitialInlayRefreshAfterIndex = false;
		pendingInlayRefreshAfterIndexActivity = false;
		stopGitStormPolling();
		options.onStateChanged();
	};

	options.context.subscriptions.push(
		languages.registerInlayHintsProvider(
			[
				{ scheme: 'file', language: 'nsf' },
				{ scheme: 'file', language: 'hlsl' }
			],
			{
				onDidChangeInlayHints: inlayHintsChanged.event,
				provideInlayHints: async (document, range, token) => {
					await options.ensureClientStarted(false);
					const client = options.getClient();
					if (!client || !client.initializeResult) {
						return [];
					}
					if (token.isCancellationRequested) {
						return [];
					}
					let effectiveRange = range;
					const visibleEditor = window.visibleTextEditors.find(
						(editor) => editor.document.uri.toString() === document.uri.toString()
					);
					if (visibleEditor && visibleEditor.visibleRanges.length > 0) {
						let visibleStart = visibleEditor.visibleRanges[0].start;
						let visibleEnd = visibleEditor.visibleRanges[0].end;
						for (const visibleRange of visibleEditor.visibleRanges) {
							if (visibleRange.start.isBefore(visibleStart)) {
								visibleStart = visibleRange.start;
							}
							if (visibleRange.end.isAfter(visibleEnd)) {
								visibleEnd = visibleRange.end;
							}
						}
						const expandedStartLine = Math.max(0, visibleStart.line - 80);
						const expandedEndLine = Math.min(document.lineCount - 1, visibleEnd.line + 80);
						const expandedStart = new Position(expandedStartLine, 0);
						const expandedEndChar = document.lineAt(expandedEndLine).text.length;
						const expandedEnd = new Position(expandedEndLine, expandedEndChar);
						const start = range.start.isAfter(expandedStart) ? range.start : expandedStart;
						const end = range.end.isBefore(expandedEnd) ? range.end : expandedEnd;
						if (end.isAfter(start)) {
							effectiveRange = new Range(start, end);
						}
					}
					const stateSnapshot = lastIndexingState ?? (await fetchIndexingState(false));
					if (!isIndexingStateStable(stateSnapshot)) {
						return [];
					}
					options.beginRpcActivity(LSP_METHOD_KEYS.inlayHint);
					let response: any[] | undefined;
					try {
						response = await sendInlayHintRequest(document.uri.toString(), effectiveRange, token);
					} catch (error) {
						if (token.isCancellationRequested) {
							return [];
						}
						throw error;
					} finally {
						options.endRpcActivity();
					}
					if (token.isCancellationRequested || !Array.isArray(response)) {
						return [];
					}
					const hints: InlayHint[] = [];
					for (const item of response) {
						const line = typeof item?.position?.line === 'number' ? item.position.line : -1;
						const character = typeof item?.position?.character === 'number' ? item.position.character : -1;
						if (line < 0 || character < 0) {
							continue;
						}
						const label = typeof item?.label === 'string' ? item.label : '';
						if (label.length === 0) {
							continue;
						}
						const hint = new InlayHint(
							new Position(line, character),
							label,
							item?.kind === 1 ? InlayHintKind.Type : InlayHintKind.Parameter
						);
						if (typeof item?.paddingLeft === 'boolean') {
							hint.paddingLeft = item.paddingLeft;
						}
						if (typeof item?.paddingRight === 'boolean') {
							hint.paddingRight = item.paddingRight;
						}
						hints.push(hint);
					}
					return hints;
				}
			}
		)
	);

	return {
		getSnapshot: () => ({
			lastIndexingState,
			lastIndexingEvent,
			activeIndexingCount: activeIndexingTokens.size,
			indexingMessage,
			indexingUnit,
			initialInlayRefreshTriggerCount,
			indexSettledInlayRefreshTriggerCount,
			pendingInitialInlayRefreshAfterIndex,
			pendingInlayRefreshAfterIndexActivity
		}),
		sendInlayHintRequest,
		sendDocumentSymbolRequest,
		sendWorkspaceSymbolRequest,
		collectSpamRequestResults,
		fetchIndexingState,
		requestIndexRebuild,
		refreshIndexingStateAndMaybeTriggerInlay,
		startGitStormPolling,
		stopGitStormPolling,
		scheduleIncludeUnderlineRefresh,
		scheduleInlayHintsRefresh,
		scheduleInlayHintsRefreshWave,
		updateRuntimeConfiguration,
		handleIndexingNotification,
		handleIndexingStateChangedNotification,
		handleInlayHintsChangedNotification,
		fireInlayHintsChanged: () => inlayHintsChanged.fire(),
		handleClientReady,
		handleClientStopped
	};
}
