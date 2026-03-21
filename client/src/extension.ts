/* --------------------------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License. See License.txt in the project root for license information.
 * ------------------------------------------------------------------------------------------ */

import * as fs from 'fs';
import * as path from 'path';
import {
	workspace,
	ExtensionContext,
	window,
	commands,
	ConfigurationTarget,
	StatusBarAlignment,
	Uri,
	TextDocument,
	TextEditor,
	languages,
	Position,
	Range,
	InlayHint,
	InlayHintKind,
	EventEmitter,
	CancellationToken,
	DecorationRangeBehavior,
	ProgressLocation
} from 'vscode';

import { computeSingleFileIncludePaths } from './single_file_config';
import {
	DIAGNOSTICS_INDETERMINATE_REASON_KEYS,
	SIGNATURE_HELP_INDETERMINATE_REASON_KEYS
} from './indeterminate_reasons';
import {
	METRICS_SUMMARY_EMPTY_REASON,
	METRICS_SUMMARY_LABELS,
	METRICS_SUMMARY_PART_DELIMITER
} from './metrics_summary_labels';
import { LSP_METHOD_KEYS } from './lsp_method_keys';

import {
	LanguageClient,
	LanguageClientOptions,
	RevealOutputChannelOn,
	ServerOptions,
	State,
	TransportKind
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let clientStarting: Promise<void> | undefined;

export function activate(context: ExtensionContext) {
	const clientOutputChannel = window.createOutputChannel('NSF LSP Trace');
	context.subscriptions.push(clientOutputChannel);
	const logClient = (message: string): void => {
		clientOutputChannel.appendLine(`[client] ${message}`);
	};
	const appendClientTrace = (message: string): void => {
		clientOutputChannel.appendLine(`[trace] ${message}`);
	};
	type RecentClientError = {
		at: number;
		source: string;
		message: string;
		stack: string;
	};
	const recentClientErrors: RecentClientError[] = [];
	const isTransientClientRpcError = (error: unknown): boolean => {
		const message = (error instanceof Error ? error.message : String(error)).toLowerCase();
		return (
			message.includes('request cancelled') ||
			message.includes('connection got disposed') ||
			message.includes('write after end') ||
			message.includes('language client is not ready yet') ||
			message.includes('-32800')
		);
	};
	const pushRecentClientError = (source: string, error: unknown): void => {
		if (isTransientClientRpcError(error)) {
			return;
		}
		const err = error instanceof Error ? error : undefined;
		const message = err?.message ?? String(error);
		const stack = err?.stack ?? message;
		recentClientErrors.push({
			at: Date.now(),
			source,
			message,
			stack
		});
		logClient(`[error:${source}] ${message}`);
		if (stack && stack !== message) {
			clientOutputChannel.appendLine(stack);
		}
	};
	const isTestMode = (process.env.NSF_TEST_MODE ?? '').trim().length > 0;
	const onUnhandledRejection = (reason: unknown): void => {
		if (isTransientClientRpcError(reason)) {
			return;
		}
		pushRecentClientError('unhandledRejection', reason);
	};
	const onUncaughtException = (error: Error): void => {
		if (isTransientClientRpcError(error)) {
			return;
		}
		pushRecentClientError('uncaughtException', error);
	};
	process.on('unhandledRejection', onUnhandledRejection);
	process.on('uncaughtException', onUncaughtException);
	context.subscriptions.push({
		dispose: () => {
			process.off('unhandledRejection', onUnhandledRejection);
			process.off('uncaughtException', onUncaughtException);
		}
	});

	const isDefinitionTraceEnabled = (): boolean => false;
	const logDefinitionTrace = (message: string): void => {
		if (!isDefinitionTraceEnabled()) {
			return;
		}
		clientOutputChannel.appendLine(`[definition] ${message}`);
	};
	appendClientTrace('activate');
	logClient('activate');

	const statusBarItem = window.createStatusBarItem(StatusBarAlignment.Right, 100);
	statusBarItem.command = 'nsf.checkStatus';
	context.subscriptions.push(statusBarItem);

	const unitStatusBarItem = window.createStatusBarItem(StatusBarAlignment.Left, 100);
	unitStatusBarItem.command = 'nsf.selectUnit';
	context.subscriptions.push(unitStatusBarItem);

	let clientStateLabel: 'stopped' | 'starting' | 'ready' | 'error' = 'stopped';
	const activeIndexingTokens = new Set<number>();
	type ServerIndexingState = {
		epoch?: number;
		state?: string;
		reason?: string;
		updatedAtMs?: number;
		pending?: {
			queuedTasks?: number;
			runningWorkers?: number;
			dirtyFiles?: number;
			probeRemainingBudget?: number;
		};
		progress?: {
			phase?: string;
			visited?: number;
			total?: number;
		};
		limits?: {
			workerCount?: number;
			queueCapacity?: number;
		};
	};
	type MetricsMethodStats = {
		count?: number;
		cancelled?: number;
		failed?: number;
		avgMs?: number;
		maxMs?: number;
	};
	type MetricsDiagnostics = {
		count?: number;
		truncated?: number;
		timedOut?: number;
		heavyRulesSkipped?: number;
		indeterminateTotal?: number;
		indeterminateReasons?: Record<string, number>;
		staleDroppedBeforeBuild?: number;
		staleDroppedBeforePublish?: number;
		canceledInPending?: number;
		queueWaitAvgMs?: number;
		queueWaitMaxMs?: number;
		queueMaxPendingTotal?: number;
		queueMaxReadyTotal?: number;
		avgMs?: number;
		maxMs?: number;
	};
	type MetricsDiagnosticsQueue = {
		pendingFast?: number;
		pendingFull?: number;
		pendingTotal?: number;
		readyFast?: number;
		readyFull?: number;
		readyTotal?: number;
	};
	type MetricsFastAst = {
		lookups?: number;
		cacheHits?: number;
		cacheReused?: number;
		rebuilds?: number;
		functionsIndexed?: number;
	};
	type MetricsIncludeGraphCache = {
		lookups?: number;
		cacheHits?: number;
		rebuilds?: number;
		invalidations?: number;
	};
	type MetricsFullAst = {
		lookups?: number;
		cacheHits?: number;
		rebuilds?: number;
		invalidations?: number;
		functionsIndexed?: number;
		includesIndexed?: number;
		documentsCached?: number;
	};
	type MetricsSignatureHelp = {
		indeterminateTotal?: number;
		indeterminateReasons?: Record<string, number>;
		overloadResolver?: {
			attempts?: number;
			resolved?: number;
			ambiguous?: number;
			noViable?: number;
			shadowMismatch?: number;
		};
	};
	type MetricsPayload = {
		methods?: Record<string, MetricsMethodStats>;
		diagnostics?: MetricsDiagnostics;
		diagnosticsQueue?: MetricsDiagnosticsQueue;
		fastAst?: MetricsFastAst;
		includeGraphCache?: MetricsIncludeGraphCache;
		fullAst?: MetricsFullAst;
		signatureHelp?: MetricsSignatureHelp;
	};
	let indexingMessage = '';
	let indexingUnit = '';
	let lastIndexingEvent: { kind: string; state: string; phase?: string; symbol?: string; token?: number } | undefined;
	let lastIndexingState: ServerIndexingState | undefined;
	let pendingIndexingStateRequest: Promise<ServerIndexingState | undefined> | undefined;
	let gitStormPollingTimer: NodeJS.Timeout | undefined;
	let activeRpcCount = 0;
	let lastRpcMethod = '';
	let latestMetricsPayload: MetricsPayload | undefined;
	let latestMetricsSummary = '';
	let pendingInitialInlayRefreshAfterIndex = false;
	let pendingInlayRefreshAfterIndexActivity = false;
	let sawIndexingEventSinceReady = false;
	let initialInlayRefreshTriggerCount = 0;
	let indexSettledInlayRefreshTriggerCount = 0;
	let lastGitStormKickAtMs = 0;

	const normalizeIndexingState = (payload: unknown): ServerIndexingState | undefined => {
		if (!payload || typeof payload !== 'object') {
			return undefined;
		}
		return payload as ServerIndexingState;
	};
	const summarizeTopReasons = (pairs: Array<[string, number | undefined]>, topN = 2): string => {
		const normalized = pairs
			.map(([name, value]) => [name, Math.trunc(value ?? 0)] as const)
			.filter(([, value]) => value > 0)
			.sort((a, b) => b[1] - a[1]);
		if (normalized.length === 0) {
			return '';
		}
		return normalized
			.slice(0, topN)
			.map(([name, value]) => `${name}:${value}`)
			.join(',');
	};
	const summarizeTopReasonsFromMap = (
		reasonMap: Record<string, number> | undefined,
		preferredOrder: readonly string[],
		topN = 2
	): string => {
		if (!reasonMap) {
			return '';
		}
		const preferred = preferredOrder.map((name) => [name, reasonMap[name]] as [string, number | undefined]);
		const known = new Set(preferredOrder);
		for (const [name, value] of Object.entries(reasonMap)) {
			if (known.has(name)) {
				continue;
			}
			preferred.push([name, value]);
		}
		return summarizeTopReasons(
			preferred,
			topN
		);
	};
	const isIndexingStateStable = (state: ServerIndexingState | undefined): boolean => {
		if (!state) {
			return false;
		}
		if (state.state !== 'Idle') {
			return false;
		}
		const queued = state.pending?.queuedTasks ?? 0;
		const running = state.pending?.runningWorkers ?? 0;
		return queued === 0 && running === 0;
	};
	const hasServerIndexingWork = (state: ServerIndexingState | undefined): boolean => {
		if (!state) {
			return false;
		}
		if (state.state && state.state !== 'Idle') {
			return true;
		}
		const queued = state.pending?.queuedTasks ?? 0;
		const running = state.pending?.runningWorkers ?? 0;
		return queued > 0 || running > 0;
	};
	const formatIndexingProgress = (state: ServerIndexingState | undefined): string => {
		if (!state) {
			return '';
		}
		const visited = state.progress?.visited;
		const total = state.progress?.total;
		if (typeof visited !== 'number') {
			return '';
		}
		if (typeof total === 'number' && total > 0) {
			return `(${visited}/${total})`;
		}
		return `(${visited}/?)`;
	};

	const getRpcShortLabel = (method: string): string => {
		switch (method) {
			case LSP_METHOD_KEYS.initialize:
				return 'Init';
			case LSP_METHOD_KEYS.shutdown:
				return 'Shutdown';
			case LSP_METHOD_KEYS.hover:
				return 'Hover';
			case LSP_METHOD_KEYS.definition:
				return 'Def';
			case LSP_METHOD_KEYS.references:
				return 'Refs';
			case LSP_METHOD_KEYS.documentHighlight:
				return 'HL';
			case LSP_METHOD_KEYS.completion:
				return 'Comp';
			case LSP_METHOD_KEYS.signatureHelp:
				return 'Sig';
			case LSP_METHOD_KEYS.documentSymbol:
				return 'Symbols';
			case LSP_METHOD_KEYS.workspaceSymbol:
				return 'WS Sym';
			case LSP_METHOD_KEYS.publishDiagnostics:
				return 'Diag';
			case LSP_METHOD_KEYS.inlayHint:
				return 'Inlay';
			case LSP_METHOD_KEYS.didChangeConfiguration:
				return 'Config';
			case LSP_METHOD_KEYS.setActiveUnit:
				return 'Unit';
			case LSP_METHOD_KEYS.rebuildIndex:
				return 'Index';
			default: {
				if (!method) {
					return '';
				}
				const slash = method.lastIndexOf('/');
				const tail = slash >= 0 ? method.slice(slash + 1) : method;
				if (tail.length <= 10) {
					return tail;
				}
				return tail.slice(0, 10);
			}
		}
	};
	const beginRpcActivity = (method: string): void => {
		activeRpcCount += 1;
		lastRpcMethod = method;
		refreshStatusBar();
	};
	const endRpcActivity = (): void => {
		activeRpcCount = Math.max(0, activeRpcCount - 1);
		if (activeRpcCount === 0) {
			lastRpcMethod = '';
		}
		refreshStatusBar();
	};

	const pinnedUnitStorageKey = 'nsf.pinnedUnitUri';
	let pinnedUnitUri: Uri | undefined;
	let lastActiveNsfUri: Uri | undefined;
	let effectiveUnitUri: Uri | undefined;
	let lastSentUnitUri = '';

	const tryParseUri = (value: unknown): Uri | undefined => {
		if (typeof value !== 'string' || value.length === 0) {
			return undefined;
		}
		try {
			return Uri.parse(value);
		} catch {
			return undefined;
		}
	};

	pinnedUnitUri = tryParseUri(context.workspaceState.get<string>(pinnedUnitStorageKey));

	const isNsfUnitDocument = (document: TextDocument): boolean => {
		if (document.uri.scheme !== 'file') {
			return false;
		}
		return path.extname(document.uri.fsPath).toLowerCase() === '.nsf';
	};

	const isSameWorkspaceFolder = (a: Uri, b: Uri): boolean => {
		const wa = workspace.getWorkspaceFolder(a);
		const wb = workspace.getWorkspaceFolder(b);
		if (!wa || !wb) {
			return false;
		}
		return wa.uri.toString() === wb.uri.toString();
	};

	const findSiblingNsfUnit = (document: TextDocument): Uri | undefined => {
		if (document.uri.scheme !== 'file') {
			return undefined;
		}
		try {
			const dir = path.dirname(document.uri.fsPath);
			const entries = fs.readdirSync(dir, { withFileTypes: true });
			const nsf = entries
				.filter((e) => e.isFile() && e.name.toLowerCase().endsWith('.nsf'))
				.map((e) => e.name)
				.sort((x, y) => x.localeCompare(y))[0];
			if (!nsf) {
				return undefined;
			}
			return Uri.file(path.join(dir, nsf));
		} catch {
			return undefined;
		}
	};

	const resolveEffectiveUnit = (document: TextDocument | undefined): Uri | undefined => {
		if (pinnedUnitUri) {
			return pinnedUnitUri;
		}
		if (document && isNsfUnitDocument(document)) {
			return document.uri;
		}
		if (document && lastActiveNsfUri && isSameWorkspaceFolder(document.uri, lastActiveNsfUri)) {
			return lastActiveNsfUri;
		}
		if (document) {
			return findSiblingNsfUnit(document);
		}
		return lastActiveNsfUri;
	};

	const refreshUnitStatusBar = (): void => {
		const name = effectiveUnitUri?.scheme === 'file' ? path.basename(effectiveUnitUri.fsPath) : '';
		const prefix = pinnedUnitUri ? '$(pin) ' : '';
		unitStatusBarItem.text = name ? `${prefix}NSF: ${name}` : `${prefix}NSF: (no unit)`;
		unitStatusBarItem.tooltip = pinnedUnitUri
			? `固定工作单位：${name || pinnedUnitUri.toString()}`
			: name
				? `当前工作单位：${name}`
				: '选择 NSF 工作单位';
		unitStatusBarItem.show();
	};

	const notifyServerActiveUnit = async (): Promise<void> => {
		if (!client || !effectiveUnitUri) {
			return;
		}
		const uri = effectiveUnitUri.toString();
		if (uri === lastSentUnitUri) {
			return;
		}
		lastSentUnitUri = uri;
		try {
			beginRpcActivity(LSP_METHOD_KEYS.setActiveUnit);
			await client.sendNotification(LSP_METHOD_KEYS.setActiveUnit, { uri });
		} catch (error) {
			appendClientTrace(`send nsf/setActiveUnit failed ${(error as Error).message ?? String(error)}`);
			logClient(`send nsf/setActiveUnit failed ${(error as Error).message ?? String(error)}`);
			pushRecentClientError('nsf/setActiveUnit', error);
		} finally {
			endRpcActivity();
		}
	};

	const updateEffectiveUnitFromDocument = (document: TextDocument | undefined): void => {
		if (document && isNsfUnitDocument(document)) {
			lastActiveNsfUri = document.uri;
		}
		effectiveUnitUri = resolveEffectiveUnit(document);
		refreshUnitStatusBar();
		void notifyServerActiveUnit();
	};

	const setPinnedUnit = async (uri: Uri | undefined): Promise<void> => {
		pinnedUnitUri = uri;
		if (uri) {
			await context.workspaceState.update(pinnedUnitStorageKey, uri.toString());
			lastActiveNsfUri = uri;
		} else {
			await context.workspaceState.update(pinnedUnitStorageKey, undefined);
		}
		updateEffectiveUnitFromDocument(window.activeTextEditor?.document);
	};

	const refreshStatusBar = (): void => {
		const indexingActive = activeIndexingTokens.size > 0 || hasServerIndexingWork(lastIndexingState);
		const stateLabel = lastIndexingState?.state ?? '';
		const phaseLabel = lastIndexingState?.progress?.phase ?? '';
		const stateProgress = formatIndexingProgress(lastIndexingState);
		const stateMessage = [phaseLabel || stateLabel, stateProgress].filter((item) => item && item.length > 0).join(' ');
		let text = 'NSF';
		if (clientStateLabel === 'starting') {
			text = '$(sync~spin) NSF: Starting';
		} else if (clientStateLabel === 'error') {
			text = '$(error) NSF: Error';
		} else if (indexingActive) {
			const message = stateMessage || indexingMessage;
			text = message ? `$(sync~spin) NSF: Indexing ${message}` : '$(sync~spin) NSF: Indexing';
		} else if (activeRpcCount > 0) {
			const short = getRpcShortLabel(lastRpcMethod);
			const suffix = short ? ` ${short}` : '';
			text = `$(sync~spin) NSF: Working${suffix}`;
		} else if (clientStateLabel === 'ready') {
			text = '$(check) NSF';
		} else {
			text = 'NSF';
		}
		statusBarItem.text = text;
		if (indexingActive) {
			const message = stateMessage || indexingMessage;
			const unit = indexingUnit ? `\n单位：${indexingUnit}` : '';
			const reason = lastIndexingState?.reason ? `\n原因：${lastIndexingState.reason}` : '';
			const queued = lastIndexingState?.pending?.queuedTasks ?? 0;
			const running = lastIndexingState?.pending?.runningWorkers ?? 0;
			const limits =
				typeof lastIndexingState?.limits?.workerCount === 'number' &&
				typeof lastIndexingState?.limits?.queueCapacity === 'number'
					? `\n并发：${lastIndexingState.limits.workerCount}，队列：${lastIndexingState.limits.queueCapacity}`
					: '';
			const pending = `\n排队：${queued}，运行：${running}`;
			statusBarItem.tooltip = (message ? `正在索引：${message}` : '正在索引') + pending + limits + reason + unit;
		} else if (activeRpcCount > 0) {
			const short = getRpcShortLabel(lastRpcMethod);
			const label = short ? `\n标签：${short}` : '';
			const method = lastRpcMethod ? `\n方法：${lastRpcMethod}` : '';
			statusBarItem.tooltip = `正在与服务端通信（${activeRpcCount}）` + label + method;
		} else if (clientStateLabel === 'ready') {
			statusBarItem.tooltip = latestMetricsSummary
				? `NSF 语言服务已就绪\n${latestMetricsSummary}`
				: 'NSF 语言服务已就绪';
		} else if (clientStateLabel === 'starting') {
			statusBarItem.tooltip = 'NSF 语言服务启动中';
		} else if (clientStateLabel === 'error') {
			statusBarItem.tooltip = 'NSF 语言服务错误（点击查看状态）';
		} else if (lastIndexingEvent) {
			const extra = lastIndexingEvent.symbol ? `${lastIndexingEvent.kind} ${lastIndexingEvent.symbol}` : lastIndexingEvent.kind;
			statusBarItem.tooltip = `最近索引：${extra}`;
		} else {
			statusBarItem.tooltip = 'NSF 语言服务';
		}
		statusBarItem.show();
	};

	refreshStatusBar();
	updateEffectiveUnitFromDocument(window.activeTextEditor?.document);

	const resolveConfiguredServerPath = (configuredPath: string): string => {
		if (path.isAbsolute(configuredPath)) {
			return configuredPath;
		}
		const folder = workspace.workspaceFolders?.[0];
		if (!folder) {
			return context.asAbsolutePath(configuredPath);
		}
		return path.join(folder.uri.fsPath, configuredPath);
	};

	const resolveServerPath = (): string | undefined => {
		const configured = workspace.getConfiguration('nsf').get<string>('serverPath', '').trim();
		const candidates: string[] = [];
		if (configured.length > 0) {
			candidates.push(resolveConfiguredServerPath(configured));
		}
		candidates.push(context.asAbsolutePath(path.join('server_cpp', 'build', 'nsf_lsp.exe')));
		candidates.push(context.asAbsolutePath(path.join('server_cpp', 'build', 'nsf_lsp')));

		for (const candidate of candidates) {
			if (candidate.length > 0 && fs.existsSync(candidate)) {
				return candidate;
			}
		}
		return undefined;
	};

	type DiagnosticsMode = 'basic' | 'balanced' | 'full';
	type DiagnosticsRuntimeSettings = {
		mode: DiagnosticsMode;
		expensiveRules: boolean;
		indeterminate: {
			enabled: boolean;
			severity: number;
			maxItems: number;
			suppressWhenErrors: boolean;
		};
		timeBudgetMs: number;
		maxItems: number;
		workerCount: number;
		autoWorkerCount: boolean;
		fast: {
			enabled: boolean;
			delayMs: number;
			timeBudgetMs: number;
			maxItems: number;
		};
		full: {
			enabled: boolean;
			delayMs: number;
			expensiveRules: boolean;
			timeBudgetMs: number;
			maxItems: number;
		};
	};

	const resolveDiagnosticsMode = (): DiagnosticsMode => {
		const mode = workspace.getConfiguration('nsf').get<string>('diagnostics.mode', 'balanced');
		if (mode === 'basic' || mode === 'full') {
			return mode;
		}
		return 'balanced';
	};

	const createDiagnosticsModeDefaults = (mode: DiagnosticsMode): DiagnosticsRuntimeSettings => {
		if (mode === 'basic') {
			return {
				mode,
				expensiveRules: false,
				indeterminate: {
					enabled: true,
					severity: 4,
					maxItems: 100,
					suppressWhenErrors: true
				},
				timeBudgetMs: 700,
				maxItems: 600,
				workerCount: 1,
				autoWorkerCount: true,
				fast: {
					enabled: true,
					delayMs: 120,
					timeBudgetMs: 120,
					maxItems: 120
				},
				full: {
					enabled: false,
					delayMs: 900,
					expensiveRules: false,
					timeBudgetMs: 700,
					maxItems: 600
				}
			};
		}
		if (mode === 'full') {
			return {
				mode,
				expensiveRules: true,
				indeterminate: {
					enabled: true,
					severity: 4,
					maxItems: 400,
					suppressWhenErrors: true
				},
				timeBudgetMs: 2000,
				maxItems: 2000,
				workerCount: 2,
				autoWorkerCount: true,
				fast: {
					enabled: true,
					delayMs: 60,
					timeBudgetMs: 240,
					maxItems: 400
				},
				full: {
					enabled: true,
					delayMs: 250,
					expensiveRules: true,
					timeBudgetMs: 2000,
					maxItems: 2000
				}
			};
		}
		return {
			mode,
			expensiveRules: true,
			indeterminate: {
				enabled: true,
				severity: 4,
				maxItems: 200,
				suppressWhenErrors: true
			},
			timeBudgetMs: 1200,
			maxItems: 1200,
			workerCount: 2,
			autoWorkerCount: true,
			fast: {
				enabled: true,
				delayMs: 90,
				timeBudgetMs: 180,
				maxItems: 240
			},
			full: {
				enabled: true,
				delayMs: 700,
				expensiveRules: true,
				timeBudgetMs: 1200,
				maxItems: 1200
			}
		};
	};

	const clampDiagnosticsSeverity = (value: number): number => Math.min(4, Math.max(1, Math.trunc(value)));

	const readDiagnosticsRuntimeSettings = (): DiagnosticsRuntimeSettings => {
		const defaults = createDiagnosticsModeDefaults(resolveDiagnosticsMode());
		return {
			mode: defaults.mode,
			expensiveRules: defaults.expensiveRules,
			indeterminate: {
				enabled: defaults.indeterminate.enabled,
				severity: clampDiagnosticsSeverity(defaults.indeterminate.severity),
				maxItems: Math.max(0, defaults.indeterminate.maxItems),
				suppressWhenErrors: defaults.indeterminate.suppressWhenErrors
			},
			timeBudgetMs: Math.max(
				defaults.timeBudgetMs,
				isTestMode ? 5000 : defaults.timeBudgetMs
			),
			maxItems: Math.max(
				defaults.maxItems,
				isTestMode ? 2400 : defaults.maxItems
			),
			workerCount: Math.max(1, defaults.workerCount),
			autoWorkerCount: defaults.autoWorkerCount,
			fast: {
				enabled: isTestMode ? false : defaults.fast.enabled,
				delayMs: Math.max(0, defaults.fast.delayMs),
				timeBudgetMs: Math.max(defaults.fast.timeBudgetMs, 60),
				maxItems: Math.max(defaults.fast.maxItems, 60)
			},
			full: {
				enabled: defaults.full.enabled,
				delayMs: Math.max(0, defaults.full.delayMs),
				expensiveRules: defaults.full.expensiveRules,
				timeBudgetMs: Math.max(
					defaults.full.timeBudgetMs,
					isTestMode ? 5000 : defaults.full.timeBudgetMs
				),
				maxItems: Math.max(
					defaults.full.maxItems,
					isTestMode ? 2400 : defaults.full.maxItems
				)
			}
		};
	};

	const buildRuntimeSettings = (includePathsOverride?: string[]) => {
		const config = workspace.getConfiguration('nsf');
		return {
			intellisionPath:
				includePathsOverride ?? normalizeIncludePaths(config.get<string[]>('intellisionPath', [])),
			shaderFileExtensions: config.get<string[]>(
				'shaderFileExtensions',
				['.nsf', '.hlsl', '.hlsli', '.fx', '.usf', '.ush']
			),
			defines: config.get<string[]>('defines', []),
			debugDefinitionTrace: false,
			inlayHints: {
				enabled: config.get<boolean>('inlayHints.enabled', true),
				parameterNames: config.get<boolean>('inlayHints.parameterNames', true),
				slowPath: true
			},
			semanticTokens: {
				enabled: config.get<boolean>('semanticTokens.enabled', true)
			},
			semanticCache: {
				enabled: true,
				shadowCompare: {
					enabled: false
				}
			},
			overloadResolver: {
				enabled: false,
				shadowCompare: {
					enabled: false
				}
			},
			diagnostics: readDiagnosticsRuntimeSettings(),
			metrics: {
				enabled: !isTestMode
			},
			indexing: {
				workerCount: 16,
				queueCapacity: 4096
			}
		};
	};

	const intellisionPathPromptDismissedKey = 'nsf.intellisionPathPromptDismissed';
	const normalizeIncludePaths = (paths: string[]): string[] => {
		const resolved = paths
			.map((item) => item.trim())
			.filter((item) => item.length > 0)
			.map((item) => path.resolve(item));
		const deduped: string[] = [];
		const seen = new Set<string>();
		for (const item of resolved) {
			const key = item.toLowerCase();
			if (seen.has(key)) {
				continue;
			}
			seen.add(key);
			deduped.push(item);
		}
		const kept: string[] = [];
		for (const candidate of deduped) {
			const candidateKey = candidate.toLowerCase();
			let nested = false;
			for (const parent of deduped) {
				if (parent === candidate) {
					continue;
				}
				const parentKey = parent.toLowerCase();
				const relative = path.relative(parentKey, candidateKey);
				if (!relative || (!relative.startsWith('..') && !path.isAbsolute(relative))) {
					nested = true;
					break;
				}
			}
			if (!nested) {
				kept.push(candidate);
			}
		}
		return kept;
	};
	const promptIntellisionPathIfMissing = async (): Promise<void> => {
		const config = workspace.getConfiguration('nsf');
		const configured = normalizeIncludePaths(config.get<string[]>('intellisionPath', []));
		if (configured.length > 0) {
			await context.workspaceState.update(intellisionPathPromptDismissedKey, false);
			return;
		}
		const dismissed = context.workspaceState.get<boolean>(intellisionPathPromptDismissedKey, false);
		if (dismissed) {
			return;
		}
		const openSettingsAction = '打开设置';
		const dismissAction = '稍后';
		const pick = await window.showWarningMessage(
			'NSF: 未配置 nsf.intellisionPath。请填写 Shader 根目录，否则索引与 include-context 功能不可用。',
			openSettingsAction,
			dismissAction
		);
		if (pick === openSettingsAction) {
			await commands.executeCommand('workbench.action.openSettings', 'nsf.intellisionPath');
			return;
		}
		if (pick === dismissAction) {
			await context.workspaceState.update(intellisionPathPromptDismissedKey, true);
		}
	};

	const nsfDocumentSelector = [
		{ scheme: 'file', language: 'nsf' },
		{ scheme: 'file', language: 'hlsl' }
	];

	const createClientOptions = (): LanguageClientOptions => ({
		outputChannel: clientOutputChannel,
		traceOutputChannel: clientOutputChannel,
		revealOutputChannelOn: RevealOutputChannelOn.Error,
		documentSelector: nsfDocumentSelector,
		initializationOptions: buildRuntimeSettings(),
		synchronize: {
			fileEvents: workspace.createFileSystemWatcher('**/*.{nsf,hlsl,fx,usf,ush}')
		},
		middleware: {
			provideDefinition: async (document, position, token, next) => {
				logDefinitionTrace(
					`request uri=${document.uri.toString()} lang=${document.languageId} line=${position.line} char=${position.character}`
				);
				try {
					const result = await next(document, position, token);
					const count = Array.isArray(result) ? result.length : (result ? 1 : 0);
					logDefinitionTrace(`response locationCount=${count}`);
					if (count === 0 && isDefinitionTraceEnabled()) {
						window.setStatusBarMessage('NSF: 未找到定义，查看输出面板 NSF LSP Trace', 5000);
					}
					return result;
				} catch (error) {
					logDefinitionTrace(`error ${(error as Error).message ?? String(error)}`);
					throw error;
				}
			}
		}
	});

	const sendInlayHintRequest = async (
		documentUri: string,
		range: Range,
		token?: CancellationToken
	): Promise<any[]> => {
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
			if (token?.isCancellationRequested || isTransientClientRpcError(error)) {
				return [];
			}
			throw error;
		}
	};
	const fetchIndexingState = async (force = false): Promise<ServerIndexingState | undefined> => {
		if (!client || !client.initializeResult) {
			return undefined;
		}
		if (!force && pendingIndexingStateRequest) {
			return pendingIndexingStateRequest;
		}
		const task = (async () => {
			try {
				beginRpcActivity('nsf/getIndexingState');
				const payload = await client!.sendRequest<unknown>('nsf/getIndexingState');
				const parsed = normalizeIndexingState(payload);
				if (parsed) {
					lastIndexingState = parsed;
					refreshStatusBar();
				}
				return parsed;
			} catch (error) {
				if (isTransientClientRpcError(error)) {
					return undefined;
				}
				appendClientTrace(`fetch nsf/getIndexingState failed ${(error as Error).message ?? String(error)}`);
				logClient(`fetch nsf/getIndexingState failed ${(error as Error).message ?? String(error)}`);
				pushRecentClientError('nsf/getIndexingState', error);
				return undefined;
			} finally {
				endRpcActivity();
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
	const sleep = (ms: number): Promise<void> => new Promise((resolve) => setTimeout(resolve, ms));
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
		if (!client || !client.initializeResult) {
			return undefined;
		}
		const beforeState = lastIndexingState ?? (await fetchIndexingState(true));
		const previousEpoch = typeof beforeState?.epoch === 'number' ? beforeState.epoch : undefined;
		beginRpcActivity(LSP_METHOD_KEYS.rebuildIndex);
		try {
			await client.sendRequest<void>(LSP_METHOD_KEYS.rebuildIndex, {
				reason,
				clearDiskCache
			});
		} catch (error) {
			appendClientTrace(`send nsf/rebuildIndex failed ${(error as Error).message ?? String(error)}`);
			logClient(`send nsf/rebuildIndex failed ${(error as Error).message ?? String(error)}`);
			pushRecentClientError('nsf/rebuildIndex', error);
			throw error;
		} finally {
			endRpcActivity();
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
	const inlayHintsChanged = new EventEmitter<void>();
	const inlayPrefetchLines = 80;
	let inlayRefreshTimer: NodeJS.Timeout | undefined;
	const inlayDeferredRefreshTimers = new Set<NodeJS.Timeout>();
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
	const includeValidDecoration = window.createTextEditorDecorationType({
		textDecoration: 'underline',
		rangeBehavior: DecorationRangeBehavior.ClosedClosed
	});
	context.subscriptions.push(includeValidDecoration);
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
	let includeUnderlineRefreshTimer: NodeJS.Timeout | undefined;
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
	context.subscriptions.push(inlayHintsChanged);
	context.subscriptions.push({
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
	context.subscriptions.push(
		languages.registerInlayHintsProvider(nsfDocumentSelector, {
			onDidChangeInlayHints: inlayHintsChanged.event,
			provideInlayHints: async (document, range, token) => {
				await ensureClientStarted(false);
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
					const expandedStartLine = Math.max(0, visibleStart.line - inlayPrefetchLines);
					const expandedEndLine = Math.min(document.lineCount - 1, visibleEnd.line + inlayPrefetchLines);
					const expandedStart = new Position(expandedStartLine, 0);
					const expandedEndChar = document.lineAt(expandedEndLine).text.length;
					const expandedEnd = new Position(expandedEndLine, expandedEndChar);
					const start = range.start.isAfter(expandedStart) ? range.start : expandedStart;
					const end = range.end.isBefore(expandedEnd) ? range.end : expandedEnd;
					if (end.isAfter(start)) {
						effectiveRange = new Range(start, end);
					} else {
						effectiveRange = range;
					}
				}
				const stateSnapshot = lastIndexingState ?? (await fetchIndexingState(false));
				if (!isIndexingStateStable(stateSnapshot)) {
					return [];
				}
				beginRpcActivity(LSP_METHOD_KEYS.inlayHint);
				let response: any[] | undefined;
				try {
					response = await sendInlayHintRequest(document.uri.toString(), effectiveRange, token);
				} catch (error) {
					if (token.isCancellationRequested) {
						return [];
					}
					throw error;
				} finally {
					endRpcActivity();
				}
				if (token.isCancellationRequested) {
					return [];
				}
				if (!Array.isArray(response)) {
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
		})
	);

	let lastSentSingleFileSettingsKey = '';
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
		const runtimeSettings = buildRuntimeSettings(includePathsOverride);
		const key = JSON.stringify(runtimeSettings);
		if (key === lastSentSingleFileSettingsKey) {
			return;
		}
		lastSentSingleFileSettingsKey = key;
		beginRpcActivity(LSP_METHOD_KEYS.didChangeConfiguration);
		try {
			await client.sendNotification(LSP_METHOD_KEYS.didChangeConfiguration, {
				settings: {
					nsf: runtimeSettings
				}
			});
		} finally {
			endRpcActivity();
		}
	};

	const ensureClientStarted = async (forceRestart: boolean): Promise<void> => {
		if (clientStarting) {
			return clientStarting;
		}
		clientStarting = (async () => {
			const serverPath = resolveServerPath();
			if (!serverPath) {
				clientStateLabel = 'error';
				refreshStatusBar();
				window.showErrorMessage('NSF: 未找到 C++ 服务端，可在设置中配置 nsf.serverPath（支持工作区相对路径）');
				clientOutputChannel.show(true);
				return;
			}
			if (forceRestart && client) {
				try {
					await client.stop();
				} catch (error) {
					appendClientTrace(`language client stop failed ${(error as Error).message ?? String(error)}`);
					logClient(`language client stop failed ${(error as Error).message ?? String(error)}`);
					pushRecentClientError('client.stop', error);
				}
				client = undefined;
			}
			if (client) {
				return;
			}

			clientStateLabel = 'starting';
			activeIndexingTokens.clear();
			indexingMessage = '';
			lastIndexingState = undefined;
			pendingIndexingStateRequest = undefined;
			pendingInitialInlayRefreshAfterIndex = false;
			sawIndexingEventSinceReady = false;
			stopGitStormPolling();
			refreshStatusBar();

			const args: string[] = [];
			if (isTestMode && process.env.NSF_LSP_DEBUG === '1') {
				args.push('--debug-wait');
			}
			const serverOptions: ServerOptions = { command: serverPath, args, transport: TransportKind.stdio };
			client = new LanguageClient(
				'nsfLanguageServer',
				'NSF Language Server',
				serverOptions,
				createClientOptions()
			);
			client.onDidChangeState((e) => {
				logClient(`state ${e.oldState} -> ${e.newState}`);
				if (e.newState === State.Starting) {
					clientStateLabel = 'starting';
					refreshStatusBar();
				} else if (e.newState === State.Running) {
					if (clientStateLabel !== 'ready') {
						clientStateLabel = 'starting';
						refreshStatusBar();
					}
				} else if (e.newState === State.Stopped) {
					clientStateLabel = 'stopped';
					activeIndexingTokens.clear();
					indexingMessage = '';
					lastIndexingState = undefined;
					pendingIndexingStateRequest = undefined;
					pendingInitialInlayRefreshAfterIndex = false;
					pendingInlayRefreshAfterIndexActivity = false;
					sawIndexingEventSinceReady = false;
					stopGitStormPolling();
					refreshStatusBar();
				}
			});
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
				if (state === 'begin' || state === 'end') {
					sawIndexingEventSinceReady = true;
				}
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
				indexingMessage = [label, symbol, progress].filter((s) => typeof s === 'string' && s.length > 0).join(' ');
				indexingUnit = unit ? path.basename(unit) : '';
				void fetchIndexingState(false).then((snapshot) => {
					if (snapshot && hasServerIndexingWork(snapshot)) {
						pendingInlayRefreshAfterIndexActivity = true;
					}
					if (snapshot && isIndexingStateStable(snapshot)) {
						tryCompleteInitialInlayRefresh([40, 160]);
					}
				});
				refreshStatusBar();
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
				refreshStatusBar();
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
			const handleMetricsNotification = (payload: MetricsPayload): void => {
				latestMetricsPayload = payload;
				const inlay = payload.methods?.[LSP_METHOD_KEYS.inlayHint];
				const hover = payload.methods?.[LSP_METHOD_KEYS.hover];
				const diag = payload.diagnostics;
				const signatureHelp = payload.signatureHelp;
				const diagQueue = payload.diagnosticsQueue;
				const fastAst = payload.fastAst;
				const includeGraphCache = payload.includeGraphCache;
				const fullAst = payload.fullAst;
				const inlayPart = inlay
					? `${METRICS_SUMMARY_LABELS.inlay} ${METRICS_SUMMARY_LABELS.avg}=${(inlay.avgMs ?? 0).toFixed(1)}ms ${METRICS_SUMMARY_LABELS.cancel}=${Math.trunc(inlay.cancelled ?? 0)}`
					: '';
				const hoverPart = hover
					? `${METRICS_SUMMARY_LABELS.hover} ${METRICS_SUMMARY_LABELS.avg}=${(hover.avgMs ?? 0).toFixed(1)}ms`
					: '';
				const diagPart = diag
					? `${METRICS_SUMMARY_LABELS.diag} ${METRICS_SUMMARY_LABELS.avg}=${(diag.avgMs ?? 0).toFixed(1)}ms ${METRICS_SUMMARY_LABELS.timeout}=${Math.trunc(diag.timedOut ?? 0)} ${METRICS_SUMMARY_LABELS.stale}=${Math.trunc((diag.staleDroppedBeforeBuild ?? 0) + (diag.staleDroppedBeforePublish ?? 0))}`
					: '';
				const diagReasonTop = diag
					? summarizeTopReasonsFromMap(diag.indeterminateReasons, DIAGNOSTICS_INDETERMINATE_REASON_KEYS)
					: '';
				const signatureHelpReasonTop = signatureHelp
					? summarizeTopReasonsFromMap(
							signatureHelp.indeterminateReasons,
							SIGNATURE_HELP_INDETERMINATE_REASON_KEYS
						)
					: '';
				const overloadPart =
					(signatureHelp?.overloadResolver?.attempts ?? 0) > 0
						? `${METRICS_SUMMARY_LABELS.ovl} ${METRICS_SUMMARY_LABELS.a}=${Math.trunc(signatureHelp?.overloadResolver?.attempts ?? 0)} ${METRICS_SUMMARY_LABELS.rz}=${Math.trunc(signatureHelp?.overloadResolver?.resolved ?? 0)} ${METRICS_SUMMARY_LABELS.amb}=${Math.trunc(signatureHelp?.overloadResolver?.ambiguous ?? 0)} ${METRICS_SUMMARY_LABELS.nv}=${Math.trunc(signatureHelp?.overloadResolver?.noViable ?? 0)}`
						: '';
				const indeterminatePart =
					(diag?.indeterminateTotal ?? 0) > 0 || (signatureHelp?.indeterminateTotal ?? 0) > 0
						? `${METRICS_SUMMARY_LABELS.indet} ${METRICS_SUMMARY_LABELS.d}=${Math.trunc(diag?.indeterminateTotal ?? 0)} ${METRICS_SUMMARY_LABELS.s}=${Math.trunc(signatureHelp?.indeterminateTotal ?? 0)} ${METRICS_SUMMARY_LABELS.diagTop}=${diagReasonTop || METRICS_SUMMARY_EMPTY_REASON} ${METRICS_SUMMARY_LABELS.sigTop}=${signatureHelpReasonTop || METRICS_SUMMARY_EMPTY_REASON}`
						: '';
				const diagQueuePart = diagQueue
					? `${METRICS_SUMMARY_LABELS.dQueue} ${METRICS_SUMMARY_LABELS.p}=${Math.trunc(diagQueue.pendingTotal ?? 0)} ${METRICS_SUMMARY_LABELS.r}=${Math.trunc(diagQueue.readyTotal ?? 0)}`
					: '';
				const fastAstPart =
					fastAst && (fastAst.lookups ?? 0) > 0
						? `${METRICS_SUMMARY_LABELS.fastAst} ${METRICS_SUMMARY_LABELS.hit}=${(((fastAst.cacheHits ?? 0) * 100) / Math.max(1, fastAst.lookups ?? 0)).toFixed(0)}%`
						: '';
				const includeGraphPart =
					includeGraphCache && (includeGraphCache.lookups ?? 0) > 0
						? `${METRICS_SUMMARY_LABELS.includeGraph} ${METRICS_SUMMARY_LABELS.hit}=${(((includeGraphCache.cacheHits ?? 0) * 100) / Math.max(1, includeGraphCache.lookups ?? 0)).toFixed(0)}%`
						: '';
				const fullAstPart =
					fullAst && (fullAst.lookups ?? 0) > 0
						? `${METRICS_SUMMARY_LABELS.fullAst} ${METRICS_SUMMARY_LABELS.hit}=${(((fullAst.cacheHits ?? 0) * 100) / Math.max(1, fullAst.lookups ?? 0)).toFixed(0)}% ${METRICS_SUMMARY_LABELS.docs}=${Math.trunc(fullAst.documentsCached ?? 0)}`
						: '';
				latestMetricsSummary = [inlayPart, hoverPart, diagPart, indeterminatePart, overloadPart, diagQueuePart, fastAstPart, includeGraphPart, fullAstPart]
					.filter((item) => item.length > 0)
					.join(METRICS_SUMMARY_PART_DELIMITER);
				logClient(`metrics ${JSON.stringify(payload)}`);
				refreshStatusBar();
			};
			try {
				client.start();
			} catch (error) {
				clientStateLabel = 'error';
				refreshStatusBar();
				logClient(`language client start failed ${(error as Error).message ?? String(error)}`);
				pushRecentClientError('client.start', error);
				clientOutputChannel.show(true);
				throw error;
			}
			appendClientTrace(`language client started serverPath=${serverPath}`);
			logClient(`language client started serverPath=${serverPath}`);
			try {
				await client.onReady();
				appendClientTrace('language client ready');
				logClient('language client ready');
				clientStateLabel = 'ready';
				refreshStatusBar();
				client?.onNotification('nsf/indexing', handleIndexingNotification);
				client?.onNotification('nsf/indexingStateChanged', handleIndexingStateChangedNotification);
				client?.onNotification('nsf/inlayHintsChanged', handleInlayHintsChangedNotification);
				client?.onNotification('nsf/metrics', handleMetricsNotification);
				pendingInitialInlayRefreshAfterIndex = true;
				pendingInlayRefreshAfterIndexActivity = true;
				sawIndexingEventSinceReady = false;
				void refreshIndexingStateAndMaybeTriggerInlay();
				void notifyServerActiveUnit();
				const active = window.activeTextEditor?.document;
				if (active && active.uri.scheme === 'file') {
					void updateRuntimeConfiguration(active.uri.fsPath);
				}
			} catch (error) {
				appendClientTrace(`language client ready failed ${(error as Error).message ?? String(error)}`);
				logClient(`language client ready failed ${(error as Error).message ?? String(error)}`);
				pushRecentClientError('client.onReady', error);
				clientOutputChannel.show(true);
				clientStateLabel = 'error';
				refreshStatusBar();
				throw error;
			}
			if (isDefinitionTraceEnabled()) {
				clientOutputChannel.show(true);
			}
		})().finally(() => {
			clientStarting = undefined;
		});
		return clientStarting;
	};

	context.subscriptions.push(workspace.onDidOpenTextDocument((document) => {
		if (document.uri.scheme !== 'file') {
			return;
		}
		if (document.languageId !== 'hlsl' && document.languageId !== 'nsf') {
			return;
		}
		void ensureClientStarted(false)
			.then(() => {
				updateRuntimeConfiguration(document.uri.fsPath);
				scheduleIncludeUnderlineRefresh();
				if (pendingInitialInlayRefreshAfterIndex) {
					void refreshIndexingStateAndMaybeTriggerInlay();
				}
			})
			.catch((error) => {
				appendClientTrace(`ensure client on open failed ${(error as Error).message ?? String(error)}`);
				logClient(`ensure client on open failed ${(error as Error).message ?? String(error)}`);
				pushRecentClientError('onDidOpenTextDocument', error);
			});
	}));

	context.subscriptions.push(window.onDidChangeActiveTextEditor((editor) => {
		const document = editor?.document;
		if (!document) {
			return;
		}
		updateEffectiveUnitFromDocument(document);
		if (document.uri.scheme !== 'file') {
			return;
		}
		if (document.languageId !== 'hlsl' && document.languageId !== 'nsf') {
			return;
		}
		scheduleInlayHintsRefresh(editor);
		scheduleIncludeUnderlineRefresh();
		void ensureClientStarted(false)
			.then(() => {
				updateRuntimeConfiguration(document.uri.fsPath);
				if (pendingInitialInlayRefreshAfterIndex) {
					void refreshIndexingStateAndMaybeTriggerInlay();
				}
			})
			.catch((error) => {
				appendClientTrace(`ensure client on active editor failed ${(error as Error).message ?? String(error)}`);
				logClient(`ensure client on active editor failed ${(error as Error).message ?? String(error)}`);
				pushRecentClientError('onDidChangeActiveTextEditor', error);
			});
	}));

	context.subscriptions.push(window.onDidChangeTextEditorVisibleRanges((e) => {
		scheduleInlayHintsRefresh(e.textEditor);
		scheduleIncludeUnderlineRefresh();
	}));
	context.subscriptions.push(window.onDidChangeVisibleTextEditors(() => {
		scheduleIncludeUnderlineRefresh();
	}));
	context.subscriptions.push(workspace.onDidChangeTextDocument((e) => {
		if (e.document.uri.scheme !== 'file') {
			return;
		}
		if (e.document.languageId !== 'hlsl' && e.document.languageId !== 'nsf') {
			return;
		}
		scheduleIncludeUnderlineRefresh();
	}));
	context.subscriptions.push(workspace.onDidSaveTextDocument((document) => {
		if (document.uri.scheme !== 'file') {
			return;
		}
		if (document.languageId !== 'hlsl' && document.languageId !== 'nsf') {
			return;
		}
		scheduleIncludeUnderlineRefresh();
	}));
	if (!isTestMode) {
		const gitStormWatcher = workspace.createFileSystemWatcher('**/*.{nsf,hlsl,fx,usf,ush}');
		const gitStormWindowMs = 350;
		const gitStormThreshold = 20;
		const gitStormUniqueThreshold = 12;
		const gitStormEvents: Array<{ at: number; key: string }> = [];
		const handleGitStormFsEvent = (uri: Uri): void => {
			if (!client || !client.initializeResult) {
				return;
			}
			const now = Date.now();
			const key = uri.toString().toLowerCase();
			gitStormEvents.push({ at: now, key });
			while (gitStormEvents.length > 0 && now - gitStormEvents[0].at > gitStormWindowMs) {
				gitStormEvents.shift();
			}
			if (gitStormEvents.length < gitStormThreshold) {
				return;
			}
			const unique = new Set(gitStormEvents.map((entry) => entry.key)).size;
			if (unique < gitStormUniqueThreshold) {
				return;
			}
			if (now - lastGitStormKickAtMs < 2500) {
				return;
			}
			lastGitStormKickAtMs = now;
			gitStormEvents.length = 0;
			beginRpcActivity('nsf/kickIndexing');
			void (async () => {
				try {
					await client.sendNotification('nsf/kickIndexing', { reason: 'gitStorm' });
					startGitStormPolling();
				} catch (error) {
					appendClientTrace(`send nsf/kickIndexing failed ${(error as Error).message ?? String(error)}`);
					logClient(`send nsf/kickIndexing failed ${(error as Error).message ?? String(error)}`);
					pushRecentClientError('nsf/kickIndexing', error);
				} finally {
					endRpcActivity();
				}
			})();
		};
		context.subscriptions.push(gitStormWatcher);
		context.subscriptions.push(gitStormWatcher.onDidChange(handleGitStormFsEvent));
		context.subscriptions.push(gitStormWatcher.onDidCreate(handleGitStormFsEvent));
		context.subscriptions.push(gitStormWatcher.onDidDelete(handleGitStormFsEvent));
	}

	context.subscriptions.push(commands.registerCommand('nsf.selectUnit', async () => {
		const activeDocument = window.activeTextEditor?.document;
		const items: Array<{ label: string; description?: string; detail?: string; action: string; uri?: Uri }> = [];

		if (pinnedUnitUri) {
			items.push({
				label: '清除固定工作单位',
				description: pinnedUnitUri.scheme === 'file' ? path.basename(pinnedUnitUri.fsPath) : pinnedUnitUri.toString(),
				action: 'clearPin'
			});
		}

		if (effectiveUnitUri) {
			items.push({
				label: '打开当前工作单位文件',
				description: effectiveUnitUri.scheme === 'file' ? path.basename(effectiveUnitUri.fsPath) : effectiveUnitUri.toString(),
				action: 'openCurrent'
			});
		}

		if (activeDocument && isNsfUnitDocument(activeDocument)) {
			items.push({
				label: '固定为当前 NSF 文件',
				description: path.basename(activeDocument.uri.fsPath),
				action: 'pin',
				uri: activeDocument.uri
			});
		}

		const nsfFiles = await workspace.findFiles('**/*.nsf', '**/{.git,node_modules,server_cpp/build*,**/build/**}', 200);
		for (const uri of nsfFiles) {
			const folder = workspace.getWorkspaceFolder(uri);
			const rel = folder ? path.relative(folder.uri.fsPath, uri.fsPath) : uri.fsPath;
			items.push({
				label: path.basename(uri.fsPath),
				description: folder ? folder.name : undefined,
				detail: rel,
				action: 'pin',
				uri
			});
		}

		const picked = await window.showQuickPick(items, {
			placeHolder: '选择 NSF 工作单位（选择文件会固定到状态栏）',
			matchOnDescription: true,
			matchOnDetail: true
		});
		if (!picked) {
			return;
		}
		if (picked.action === 'clearPin') {
			await setPinnedUnit(undefined);
			return;
		}
		if (picked.action === 'openCurrent') {
			if (!effectiveUnitUri) {
				return;
			}
			const doc = await workspace.openTextDocument(effectiveUnitUri);
			await window.showTextDocument(doc, { preview: false });
			return;
		}
		if (picked.action === 'pin' && picked.uri) {
			await setPinnedUnit(picked.uri);
		}
	}));

	context.subscriptions.push(commands.registerCommand('nsf.checkStatus', async () => {
		clientOutputChannel.show(true);
		const configured = workspace.getConfiguration('nsf').get<string>('serverPath', '').trim();
		const resolved = resolveServerPath();
		logClient(`workspaceFolders=${workspace.workspaceFolders?.length ?? 0}`);
		logClient(`configuredServerPath=${configured || '(empty)'}`);
		logClient(`resolvedServerPath=${resolved ?? '(not found)'}`);
		logClient(`clientCreated=${client ? 'yes' : 'no'}`);
		logClient(`clientReady=${client?.initializeResult ? 'yes' : 'no'}`);
		logClient(`latestMetrics=${latestMetricsSummary || '(empty)'}`);
		logClient(
			`indexingState=${JSON.stringify({
				state: lastIndexingState?.state ?? '(unknown)',
				reason: lastIndexingState?.reason ?? '',
				queuedTasks: lastIndexingState?.pending?.queuedTasks ?? 0,
				runningWorkers: lastIndexingState?.pending?.runningWorkers ?? 0,
				phase: lastIndexingState?.progress?.phase ?? '',
				visited: lastIndexingState?.progress?.visited ?? 0,
				total: lastIndexingState?.progress?.total ?? 0
			})}`
		);
		logClient(
			`inlayTriggerState=${JSON.stringify({
				initialInlayRefreshTriggerCount,
				indexSettledInlayRefreshTriggerCount,
				pendingInitialInlayRefreshAfterIndex,
				pendingInlayRefreshAfterIndexActivity
			})}`
		);
		if (recentClientErrors.length > 0) {
			logClient(`recentErrors=${recentClientErrors.length}`);
			for (const item of recentClientErrors) {
				logClient(`[error@${new Date(item.at).toISOString()}][${item.source}] ${item.message}`);
				if (item.stack && item.stack !== item.message) {
					clientOutputChannel.appendLine(item.stack);
				}
			}
		}
		try {
			await ensureClientStarted(false);
		} catch (error) {
			pushRecentClientError('nsf.checkStatus.ensureClientStarted', error);
			window.showErrorMessage(`NSF: 状态检查失败：${(error as Error).message ?? String(error)}`);
		}
	}));

	context.subscriptions.push(commands.registerCommand('nsf.restartServer', async () => {
		clientOutputChannel.show(true);
		logClient('manual restart requested');
		await ensureClientStarted(true);
	}));

	context.subscriptions.push(commands.registerCommand('nsf.rebuildIndexClearCache', async () => {
		const continueLabel = '继续';
		if (!isTestMode) {
			const picked = await window.showWarningMessage(
				'NSF: 这会清除当前工作区索引缓存并执行一次完整重建。',
				{ modal: true },
				continueLabel
			);
			if (picked !== continueLabel) {
				return;
			}
		}
		clientOutputChannel.show(true);
		logClient('manual clear-cache rebuild requested');
		try {
			await ensureClientStarted(false);
			if (!client?.initializeResult) {
				throw new Error('Language client is not ready.');
			}
			const targetReason = 'manualClearCache';
			await window.withProgress(
				{
					location: ProgressLocation.Notification,
					title: 'NSF: 正在清缓存并完整重建索引',
					cancellable: false
				},
				async (progress) => {
					progress.report({ message: '正在请求服务端清缓存...' });
					const state = await requestIndexRebuild(targetReason, true);
					if (!state) {
						throw new Error('Timed out waiting for the clear-cache rebuild to finish.');
					}
					progress.report({ message: '索引已稳定。' });
				}
			);
			scheduleInlayHintsRefreshWave([0, 120]);
			window.showInformationMessage('NSF: 已清缓存并完成完整索引重建。');
		} catch (error) {
			pushRecentClientError('nsf.rebuildIndexClearCache', error);
			const message = (error as Error).message ?? String(error);
			const configuredServerPath = workspace.getConfiguration('nsf').get<string>('serverPath', '').trim();
			if (message.includes('Method not implemented') && configuredServerPath.length > 0) {
				window.showErrorMessage(
					'NSF: 当前配置的 serverPath 指向的语言服务不支持清缓存重建索引。请重建该二进制或清空 nsf.serverPath。'
				);
			} else {
				window.showErrorMessage(`NSF: 清缓存重建索引失败：${message}`);
			}
			throw error;
		}
	}));

	context.subscriptions.push(
		commands.registerCommand('nsf._getInternalStatus', async () => {
			return {
				clientState: clientStateLabel,
				indexingActive: activeIndexingTokens.size,
				lastIndexingEvent,
				indexingState: lastIndexingState,
				initialInlayRefreshTriggerCount,
				indexSettledInlayRefreshTriggerCount,
				pendingInitialInlayRefreshAfterIndex,
				pendingInlayRefreshAfterIndexActivity
			};
		})
	);
	context.subscriptions.push(
		commands.registerCommand('nsf._resetInternalStatus', async () => {
			lastIndexingEvent = undefined;
			lastIndexingState = undefined;
			activeIndexingTokens.clear();
			indexingMessage = '';
			initialInlayRefreshTriggerCount = 0;
			indexSettledInlayRefreshTriggerCount = 0;
			refreshStatusBar();
		})
	);
	context.subscriptions.push(
		commands.registerCommand('nsf._clearActiveUnitForTests', async () => {
			pinnedUnitUri = undefined;
			lastActiveNsfUri = undefined;
			effectiveUnitUri = undefined;
			lastSentUnitUri = '';
			await context.workspaceState.update(pinnedUnitStorageKey, undefined);
			refreshUnitStatusBar();
			if (client?.initializeResult) {
				beginRpcActivity(LSP_METHOD_KEYS.setActiveUnit);
				try {
					await client.sendNotification(LSP_METHOD_KEYS.setActiveUnit, {});
				} finally {
					endRpcActivity();
				}
			}
		})
	);
	context.subscriptions.push(
		commands.registerCommand(
			'nsf._spamInlayRequests',
			async (payload: {
				uri: string;
				startLine?: number;
				startCharacter?: number;
				endLine?: number;
				endCharacter?: number;
				count?: number;
			}) => {
				await ensureClientStarted(false);
				if (!client || !client.initializeResult || typeof payload?.uri !== 'string' || payload.uri.length === 0) {
					return { completed: 0, cancelled: 0, failed: 0 };
				}
				const count = Math.max(1, Math.min(120, payload.count ?? 20));
				const range = new Range(
					new Position(Math.max(0, payload.startLine ?? 0), Math.max(0, payload.startCharacter ?? 0)),
					new Position(Math.max(0, payload.endLine ?? 2000), Math.max(0, payload.endCharacter ?? 0))
				);
				const tasks: Array<Promise<any[]>> = [];
				for (let index = 0; index < count; index++) {
					tasks.push(sendInlayHintRequest(payload.uri, range));
				}
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
			}
		)
	);
	context.subscriptions.push(
		commands.registerCommand('nsf._getLatestMetrics', async () => {
			return {
				summary: latestMetricsSummary,
				payload: latestMetricsPayload
			};
		})
	);

	const watchedFileChanges = new Map<string, number>();
	let watchedFileFlushTimer: NodeJS.Timeout | undefined;
	const queueWatchedFileChange = (uri: Uri, type: number): void => {
		if (uri.scheme !== 'file') {
			return;
		}
		scheduleIncludeUnderlineRefresh();
		watchedFileChanges.set(uri.toString(), type);
		if (watchedFileFlushTimer) {
			return;
		}
		watchedFileFlushTimer = setTimeout(() => {
			watchedFileFlushTimer = undefined;
			const activeClient = client;
			if (!activeClient || !activeClient.initializeResult || watchedFileChanges.size === 0) {
				watchedFileChanges.clear();
				return;
			}
			const changes = Array.from(watchedFileChanges.entries()).map(([changeUri, changeType]) => ({
				uri: changeUri,
				type: changeType
			}));
			watchedFileChanges.clear();
			try {
				const maybePromise = (activeClient as any).sendNotification('workspace/didChangeWatchedFiles', {
					changes
				});
				if (maybePromise && typeof maybePromise.catch === 'function') {
					void maybePromise.catch(() => undefined);
				}
			} catch {
				return;
			}
		}, 40);
	};
	const fileWatcher = workspace.createFileSystemWatcher('**/*.{nsf,hlsl,fx,usf,ush}');
	context.subscriptions.push(fileWatcher);
	context.subscriptions.push(
		fileWatcher.onDidCreate((uri) => queueWatchedFileChange(uri, 1)),
		fileWatcher.onDidChange((uri) => queueWatchedFileChange(uri, 2)),
		fileWatcher.onDidDelete((uri) => queueWatchedFileChange(uri, 3))
	);
	context.subscriptions.push({
		dispose: () => {
			if (watchedFileFlushTimer) {
				clearTimeout(watchedFileFlushTimer);
				watchedFileFlushTimer = undefined;
			}
			watchedFileChanges.clear();
		}
	});

	context.subscriptions.push(workspace.onDidChangeConfiguration((e) => {
		if (e.affectsConfiguration('nsf.serverPath')) {
			void ensureClientStarted(true);
		}
		if (
			e.affectsConfiguration('nsf.intellisionPath') ||
			e.affectsConfiguration('nsf.include.validUnderline') ||
			e.affectsConfiguration('nsf.shaderFileExtensions') ||
			e.affectsConfiguration('nsf.defines') ||
			e.affectsConfiguration('nsf.inlayHints.enabled') ||
			e.affectsConfiguration('nsf.inlayHints.parameterNames') ||
			e.affectsConfiguration('nsf.semanticTokens.enabled') ||
			e.affectsConfiguration('nsf.diagnostics.mode') ||
			e.affectsConfiguration('nsf.serverPath')
		) {
			const active = window.activeTextEditor?.document;
			if (active && active.uri.scheme === 'file') {
				void updateRuntimeConfiguration(active.uri.fsPath);
			}
			if (
				e.affectsConfiguration('nsf.inlayHints.enabled') ||
				e.affectsConfiguration('nsf.inlayHints.parameterNames')
			) {
				inlayHintsChanged.fire();
			}
			if (
				e.affectsConfiguration('nsf.intellisionPath') ||
				e.affectsConfiguration('nsf.include.validUnderline') ||
				e.affectsConfiguration('nsf.shaderFileExtensions')
			) {
				scheduleIncludeUnderlineRefresh();
			}
			if (e.affectsConfiguration('nsf.intellisionPath')) {
				void promptIntellisionPathIfMissing();
			}
		}
	}));

	void promptIntellisionPathIfMissing();
	scheduleIncludeUnderlineRefresh();
	void ensureClientStarted(false).catch((error) => {
		appendClientTrace(`initial ensure client failed ${(error as Error).message ?? String(error)}`);
		logClient(`initial ensure client failed ${(error as Error).message ?? String(error)}`);
		pushRecentClientError('initialEnsureClientStarted', error);
	});
}

export function deactivate(): Thenable<void> | undefined {
	if (!client) {
		return undefined;
	}
	return client.stop();
}
