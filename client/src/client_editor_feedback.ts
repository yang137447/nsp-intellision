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

import { buildRuntimeSettings } from './client_config_sync';
import { resolveIncludeCandidatesForPath } from './include_underline_support';
import { LSP_METHOD_KEYS } from './lsp_method_keys';
import {
	formatIndexingProgress,
	hasServerIndexingWork,
	isIndexingStateStable,
	normalizeIndexingState,
	type ServerIndexingState
} from './client_indexing_status';

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
	includeUnderlineRefreshCount: number;
	includeUnderlineLastRangeCount: number;
	includeUnderlineLastDocumentUri: string;
	includeUnderlineLastDurationMs: number;
	includeUnderlineTotalDurationMs: number;
	includeUnderlineAvgDurationMs: number;
	inlayProviderRequestCount: number;
	inlayProviderRequestAvgMs: number;
	inlayProviderRequestMaxMs: number;
	inlayRangeAdjustSamples: number;
	inlayRangeAdjustAvgMs: number;
	inlayRangeAdjustMaxMs: number;
	inlayStateCheckSamples: number;
	inlayStateCheckAvgMs: number;
	inlayStateCheckMaxMs: number;
	inlayRpcRequestSamples: number;
	inlayRpcRequestAvgMs: number;
	inlayRpcRequestMaxMs: number;
	inlayAssemblySamples: number;
	inlayAssemblyAvgMs: number;
	inlayAssemblyMaxMs: number;
	lastInlayRequestDocumentUri: string;
	lastInlayRequestEndLine: number;
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
	type CachedInlayHintsEntry = {
		range: Range;
		hints: InlayHint[];
	};
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
	const includeUnderlineMetrics = { count: 0, lastRangeCount: 0, lastDocumentUri: '', lastMs: 0, totalMs: 0 };
	const inlayMetrics = {
		provider: { samples: 0, totalMs: 0, maxMs: 0 },
		rangeAdjust: { samples: 0, totalMs: 0, maxMs: 0 },
		stateCheck: { samples: 0, totalMs: 0, maxMs: 0 },
		rpc: { samples: 0, totalMs: 0, maxMs: 0 },
		assembly: { samples: 0, totalMs: 0, maxMs: 0 },
		lastDocumentUri: '',
		lastEndLine: -1
	};
	const inlayLastGoodByUri = new Map<string, CachedInlayHintsEntry>();
	const maxLastGoodInlayEntries = 32;

	const recordDuration = (metric: { samples: number; totalMs: number; maxMs: number }, durationMs: number): void => {
		metric.samples++;
		metric.totalMs += durationMs;
		metric.maxMs = Math.max(metric.maxMs, durationMs);
	};
	const avgDuration = (metric: { samples: number; totalMs: number }): number =>
		metric.samples > 0 ? metric.totalMs / metric.samples : 0;

	const isPositionWithinRange = (position: Position, range: Range): boolean => {
		if (position.line < range.start.line || position.line > range.end.line) {
			return false;
		}
		if (position.line === range.start.line && position.character < range.start.character) {
			return false;
		}
		if (position.line === range.end.line && position.character > range.end.character) {
			return false;
		}
		return true;
	};

	const doesRangeCover = (coveringRange: Range, innerRange: Range): boolean => {
		if (coveringRange.start.isAfter(innerRange.start)) {
			return false;
		}
		if (coveringRange.end.isBefore(innerRange.end)) {
			return false;
		}
		return true;
	};

	const rememberLastGoodInlayHints = (documentUri: string, range: Range, hints: InlayHint[]): void => {
		const existing = inlayLastGoodByUri.get(documentUri);
		if (
			hints.length === 0 &&
			existing &&
			existing.hints.length > 0 &&
			!doesRangeCover(range, existing.range)
		) {
			return;
		}
		inlayLastGoodByUri.delete(documentUri);
		inlayLastGoodByUri.set(documentUri, { range, hints });
		while (inlayLastGoodByUri.size > maxLastGoodInlayEntries) {
			const oldestKey = inlayLastGoodByUri.keys().next().value as string | undefined;
			if (!oldestKey) {
				break;
			}
			inlayLastGoodByUri.delete(oldestKey);
		}
	};

	const getLastGoodInlayHints = (documentUri: string, range: Range): InlayHint[] | undefined => {
		const cached = inlayLastGoodByUri.get(documentUri);
		if (!cached) {
			return undefined;
		}
		if (!doesRangeCover(cached.range, range)) {
			return undefined;
		}
		const filtered = cached.hints.filter((hint) => isPositionWithinRange(hint.position, range));
		return filtered;
	};

	const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));
	const inlayHintsChanged = new EventEmitter<void>();
	const inlayPrefetchLines = 24;
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
			inlayLastGoodByUri.clear();
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
			['.nsf', '.hlsl']
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
			const candidates = resolveIncludeCandidatesForPath({
				documentFsPath: document.uri.fsPath,
				includePath,
				workspaceFolders: (workspace.workspaceFolders ?? []).map((item) => item.uri.fsPath),
				includePaths: workspace.getConfiguration('nsf').get<string[]>('intellisionPath', []),
				shaderExtensions: getConfiguredShaderExtensions()
			});
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
		const startedAt = Date.now();
		const ranges = computeValidIncludeRanges(editor.document);
		const durationMs = Date.now() - startedAt;
		includeUnderlineMetrics.count++;
		includeUnderlineMetrics.lastRangeCount = ranges.length;
		includeUnderlineMetrics.lastDocumentUri = editor.document.uri.toString();
		includeUnderlineMetrics.lastMs = durationMs;
		includeUnderlineMetrics.totalMs += durationMs;
		editor.setDecorations(includeValidDecoration, ranges);
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
		if (filePath && !isInAnyWorkspaceFolder(filePath)) {
			return;
		}
		let includePathsOverride: string[] | undefined;
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
					const providerStartedAt = Date.now();
					await options.ensureClientStarted(false);
					const client = options.getClient();
					let effectiveRange = range;
					const visibleEditor = window.visibleTextEditors.find(
						(editor) => editor.document.uri.toString() === document.uri.toString()
					);
					if (visibleEditor && visibleEditor.visibleRanges.length > 0) {
						const rangeAdjustStartedAt = Date.now();
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
						const expandedStartLine = Math.max(0, visibleStart.line - inlayPrefetchLines);
						const expandedEndLine = Math.min(document.lineCount - 1, visibleEnd.line + inlayPrefetchLines);
						const expandedStart = new Position(expandedStartLine, 0);
						const expandedEndChar = document.lineAt(expandedEndLine).text.length;
						const expandedEnd = new Position(expandedEndLine, expandedEndChar);
						const start = range.start.isAfter(expandedStart) ? range.start : expandedStart;
						const end = range.end.isBefore(expandedEnd) ? range.end : expandedEnd;
						if (end.isAfter(start)) {
							effectiveRange = new Range(start, end);
						}
						recordDuration(inlayMetrics.rangeAdjust, Date.now() - rangeAdjustStartedAt);
					}
					const lastGoodFallback = (): InlayHint[] => {
						const cached = getLastGoodInlayHints(document.uri.toString(), effectiveRange);
						return cached ?? [];
					};
					if (!client || !client.initializeResult) {
						return lastGoodFallback();
					}
					if (token.isCancellationRequested) {
						return lastGoodFallback();
					}
					const stateCheckStartedAt = Date.now();
					const stateSnapshot = lastIndexingState ?? (await fetchIndexingState(false));
					recordDuration(inlayMetrics.stateCheck, Date.now() - stateCheckStartedAt);
					if (!isIndexingStateStable(stateSnapshot)) {
						return lastGoodFallback();
					}
					options.beginRpcActivity(LSP_METHOD_KEYS.inlayHint);
					let response: any[] | undefined;
					try {
						const rpcStartedAt = Date.now();
						response = await sendInlayHintRequest(document.uri.toString(), effectiveRange, token);
						recordDuration(inlayMetrics.rpc, Date.now() - rpcStartedAt);
					} catch (error) {
						if (token.isCancellationRequested || options.isTransientClientRpcError(error)) {
							return lastGoodFallback();
						}
						throw error;
					} finally {
						options.endRpcActivity();
					}
					if (token.isCancellationRequested || !Array.isArray(response)) {
						return lastGoodFallback();
					}
					const assemblyStartedAt = Date.now();
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
					recordDuration(inlayMetrics.assembly, Date.now() - assemblyStartedAt);
					recordDuration(inlayMetrics.provider, Date.now() - providerStartedAt);
					inlayMetrics.lastDocumentUri = document.uri.toString();
					inlayMetrics.lastEndLine = effectiveRange.end.line;
					rememberLastGoodInlayHints(document.uri.toString(), effectiveRange, hints);
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
			pendingInlayRefreshAfterIndexActivity,
			includeUnderlineRefreshCount: includeUnderlineMetrics.count,
			includeUnderlineLastRangeCount: includeUnderlineMetrics.lastRangeCount,
			includeUnderlineLastDocumentUri: includeUnderlineMetrics.lastDocumentUri,
			includeUnderlineLastDurationMs: includeUnderlineMetrics.lastMs,
			includeUnderlineTotalDurationMs: includeUnderlineMetrics.totalMs,
			includeUnderlineAvgDurationMs:
				includeUnderlineMetrics.count > 0 ? includeUnderlineMetrics.totalMs / includeUnderlineMetrics.count : 0,
			inlayProviderRequestCount: inlayMetrics.provider.samples,
			inlayProviderRequestAvgMs: avgDuration(inlayMetrics.provider),
			inlayProviderRequestMaxMs: inlayMetrics.provider.maxMs,
			inlayRangeAdjustSamples: inlayMetrics.rangeAdjust.samples,
			inlayRangeAdjustAvgMs: avgDuration(inlayMetrics.rangeAdjust),
			inlayRangeAdjustMaxMs: inlayMetrics.rangeAdjust.maxMs,
			inlayStateCheckSamples: inlayMetrics.stateCheck.samples,
			inlayStateCheckAvgMs: avgDuration(inlayMetrics.stateCheck),
			inlayStateCheckMaxMs: inlayMetrics.stateCheck.maxMs,
			inlayRpcRequestSamples: inlayMetrics.rpc.samples,
			inlayRpcRequestAvgMs: avgDuration(inlayMetrics.rpc),
			inlayRpcRequestMaxMs: inlayMetrics.rpc.maxMs,
			inlayAssemblySamples: inlayMetrics.assembly.samples,
			inlayAssemblyAvgMs: avgDuration(inlayMetrics.assembly),
			inlayAssemblyMaxMs: inlayMetrics.assembly.maxMs,
			lastInlayRequestDocumentUri: inlayMetrics.lastDocumentUri,
			lastInlayRequestEndLine: inlayMetrics.lastEndLine
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
