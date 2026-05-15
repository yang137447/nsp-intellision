/* --------------------------------------------------------------------------------------------
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the MIT License. See License.txt in the project root for license information.
 * ------------------------------------------------------------------------------------------ */

import * as fs from 'fs';
import * as path from 'path';
import { AsyncLocalStorage } from 'async_hooks';
import {
	workspace,
	ExtensionContext,
	window,
	commands,
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
	createClearActiveUnitHandler,
	type InteractiveRuntimeDebugResponse,
	type LastCompletionDebugResponse,
	type WorkspaceIndexSymbolDebugResponse,
	createRuntimeDebugHandler,
	createSetActiveUnitHandler,
	createSpamRequestHandlers,
	registerInternalCommands
} from './client_internal_commands';
import { createClientRuntimeHost } from './client_runtime_host';
import { registerClientEditorEvents } from './client_editor_events';
import { registerClientRuntimeEvents } from './client_runtime_events';
import { createActiveUnitController } from './client_active_unit';
import { createEditorFeedbackController } from './client_editor_feedback';
import { createStatusCommandHandlers } from './client_status_commands';
import {
	buildRuntimeSettings,
	normalizeIncludePaths,
	promptIntellisionPathIfMissing,
	seedPreprocessorMacrosSettingIfMissing,
	type PreprocessorMacroPresetResponse
} from './client_config_sync';
import {
	formatIndexingProgress,
	hasServerIndexingWork,
	isIndexingStateStable,
	normalizeIndexingState,
	type ServerIndexingState
} from './client_indexing_status';
import { createMetricsTracker, type MetricsPayload } from './client_metrics';
import { buildMainStatusBarPresentation } from './client_status_ui';
import { registerUserCommands } from './client_user_commands';
import { registerWatchedFileForwarding } from './client_watched_files';
import { createRealWorkspaceReplayRecorder } from './client_real_workspace_replay_recording';
import {
	CompletionRequestCoordinator,
	type CompletionCoordinatorAction,
	type CompletionCoordinatorDecision
} from './client_completion_request_coordinator';
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
	const testMode = (process.env.NSF_TEST_MODE ?? '').trim();
	const isTestMode = testMode.length > 0;
	const isPerfTestMode = testMode === 'perf';
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
	let indexingMessage = '';
	let indexingUnit = '';
	let lastIndexingEvent: { kind: string; state: string; phase?: string; symbol?: string; token?: number } | undefined;
	let lastIndexingState: ServerIndexingState | undefined;
	let pendingIndexingStateRequest: Promise<ServerIndexingState | undefined> | undefined;
	let gitStormPollingTimer: NodeJS.Timeout | undefined;
	let activeRpcCount = 0;
	let lastRpcMethod = '';
	type ClientMetric = { samples: number; totalMs: number; maxMs: number };
	type ProviderTimingKind = 'completion' | 'signatureHelp';
	type ProviderTimingDraft = {
		kind: ProviderTimingKind;
		sequence: number;
		startedAt: number;
		activeSameKindProviderCountAtStart: number;
		documentUri: string;
		document: TextDocument;
		documentVersion: number;
		documentIsDirty: boolean;
		documentVersionAtNextStart?: number;
		documentIsDirtyAtNextStart?: boolean;
		documentVersionAtLspStart?: number;
		documentIsDirtyAtLspStart?: boolean;
		documentVersionAtProviderReturn?: number;
		documentIsDirtyAtProviderReturn?: boolean;
		line?: number;
		character?: number;
		triggerKind?: number;
		triggerCharacter?: string;
		isRetrigger?: boolean;
		prefixLength?: number;
		completionCoordinatorAction?: CompletionCoordinatorAction;
		completionCoordinatorSource?: CompletionCoordinatorDecision['source'];
		completionCoordinatorKey?: string;
		completionCoordinatorPrefixLength?: number;
		completionDebugRequestId?: string;
		nextStartedAt?: number;
		nextCompletedAt?: number;
		activeSameKindNextCountAtStart?: number;
		lspRequestStartedAt?: number;
		lspRequestCompletedAt?: number;
		lspRequestCount: number;
		code2ProtocolMs: number;
		lspRequestMs: number;
		protocol2CodeMs: number;
		error?: string;
	};
	type ProviderTimingSnapshot = {
		kind: ProviderTimingKind;
		sequence: number;
		totalMs: number;
		awaitSyncMs: number;
		code2ProtocolMs: number;
		lspRequestMs: number;
		protocol2CodeMs: number;
		providerReturnMs: number;
		documentUri: string;
		documentVersion: number;
		documentIsDirty: boolean;
		documentVersionAtNextStart?: number;
		documentIsDirtyAtNextStart?: boolean;
		documentVersionAtLspStart?: number;
		documentIsDirtyAtLspStart?: boolean;
		documentVersionAtProviderReturn?: number;
		documentIsDirtyAtProviderReturn?: boolean;
		startedAtMs: number;
		completedAtMs: number;
		activeSameKindProviderCountAtStart: number;
		activeSameKindNextCountAtStart?: number;
		nextStartedAtMs?: number;
		nextCompletedAtMs?: number;
		nextWaitMs?: number;
		nextExecutionMs?: number;
		lspRequestStartedAtMs?: number;
		lspRequestCompletedAtMs?: number;
		lspRequestCount: number;
		lspStartDelayMs?: number;
		lspCompletionToProviderReturnMs?: number;
		line?: number;
		character?: number;
		triggerKind?: number;
		triggerCharacter?: string;
		isRetrigger?: boolean;
		prefixLength?: number;
		completionCoordinatorAction?: CompletionCoordinatorAction;
		completionCoordinatorSource?: CompletionCoordinatorDecision['source'];
		completionCoordinatorKey?: string;
		completionCoordinatorPrefixLength?: number;
		completionDebugRequestId?: string;
		itemCount?: number;
		signatureCount?: number;
		error?: string;
	};
	type ProviderClientMetrics = {
		requests: number;
		awaitSync: ClientMetric;
		next: ClientMetric;
		code2Protocol: ClientMetric;
		lspRequest: ClientMetric;
		protocol2Code: ClientMetric;
		providerReturn: ClientMetric;
		sequence: number;
		lastTiming?: ProviderTimingSnapshot;
		recentTimings: ProviderTimingSnapshot[];
	};
	const clientMetric = (): ClientMetric => ({ samples: 0, totalMs: 0, maxMs: 0 });
	const createProviderClientMetrics = (): ProviderClientMetrics => ({
		requests: 0,
		awaitSync: clientMetric(),
		next: clientMetric(),
		code2Protocol: clientMetric(),
		lspRequest: clientMetric(),
		protocol2Code: clientMetric(),
		providerReturn: clientMetric(),
		sequence: 0,
		lastTiming: undefined,
		recentTimings: []
	});
	const completionClientMetrics = {
		...createProviderClientMetrics()
	};
	const completionRequestCoordinator = new CompletionRequestCoordinator();
	context.subscriptions.push({
		dispose: () => completionRequestCoordinator.dispose()
	});
	const hoverClientMetrics = {
		requests: clientMetric(),
		rpc: clientMetric(),
		code2Protocol: clientMetric(),
		protocolRequest: clientMetric(),
		protocol2Code: clientMetric()
	};
	const signatureHelpClientMetrics = {
		...createProviderClientMetrics()
	};
	const providerTimingStack: ProviderTimingDraft[] = [];
	const activeProviderTimingStack: ProviderTimingDraft[] = [];
	const activeProviderTimingStorage = new AsyncLocalStorage<ProviderTimingDraft>();
	const runInProviderTimingContext = activeProviderTimingStorage.run.bind(activeProviderTimingStorage) as
		<T>(store: ProviderTimingDraft, callback: () => T) => T;
	const providerTimingHookClients = new WeakSet<object>();
	const recordClientMetric = (metric: ClientMetric, durationMs: number): void => {
		metric.samples++;
		metric.totalMs += durationMs;
		metric.maxMs = Math.max(metric.maxMs, durationMs);
	};
	const avgClientMetric = (metric: { samples: number; totalMs: number }): number =>
		metric.samples > 0 ? metric.totalMs / metric.samples : 0;
	const currentProviderTiming = (kind: ProviderTimingKind): ProviderTimingDraft | undefined => {
		const draft = activeProviderTimingStorage.getStore();
		return draft?.kind === kind ? draft : undefined;
	};
	const runWithActiveProviderTiming = async <T>(
		draft: ProviderTimingDraft,
		run: () => Thenable<T> | Promise<T> | T
	): Promise<T> => {
		const nextStartedAt = Date.now();
		draft.nextStartedAt = draft.nextStartedAt ?? nextStartedAt;
		draft.activeSameKindNextCountAtStart = activeProviderTimingStack.filter((entry) => entry.kind === draft.kind).length;
		draft.documentVersionAtNextStart = draft.document.version;
		draft.documentIsDirtyAtNextStart = draft.document.isDirty;
		activeProviderTimingStack.push(draft);
		try {
			return await runInProviderTimingContext(draft, async () => await run());
		} finally {
			draft.nextCompletedAt = Date.now();
			const stackIndex = activeProviderTimingStack.lastIndexOf(draft);
			if (stackIndex >= 0) {
				activeProviderTimingStack.splice(stackIndex, 1);
			}
		}
	};
	const providerMetricsForKind = (kind: ProviderTimingKind): ProviderClientMetrics =>
		kind === 'completion' ? completionClientMetrics : signatureHelpClientMetrics;
	const completionPrefixLength = (document: TextDocument, position?: Position): number | undefined => {
		if (!position) {
			return undefined;
		}
		const lineText = document.lineAt(position.line).text.slice(0, position.character);
		const match = lineText.match(/[A-Za-z0-9_]*$/);
		return match ? match[0].length : 0;
	};
	const beginProviderTiming = (
		kind: ProviderTimingKind,
		document: TextDocument,
		position?: Position,
		providerContext?: unknown
	): ProviderTimingDraft => {
		const metrics = providerMetricsForKind(kind);
		metrics.requests++;
		metrics.sequence++;
		recordClientMetric(metrics.awaitSync, 0);
		const context = providerContext as {
			triggerKind?: unknown;
			triggerCharacter?: unknown;
			isRetrigger?: unknown;
		} | undefined;
		const draft: ProviderTimingDraft = {
			kind,
			sequence: metrics.sequence,
			startedAt: Date.now(),
			activeSameKindProviderCountAtStart: providerTimingStack.filter((entry) => entry.kind === kind).length,
			documentUri: document.uri.toString(),
			document,
			documentVersion: document.version,
			documentIsDirty: document.isDirty,
			line: position?.line,
			character: position?.character,
			triggerKind: typeof context?.triggerKind === 'number' ? context.triggerKind : undefined,
			triggerCharacter: typeof context?.triggerCharacter === 'string' ? context.triggerCharacter : undefined,
			isRetrigger: typeof context?.isRetrigger === 'boolean' ? context.isRetrigger : undefined,
			prefixLength: kind === 'completion' ? completionPrefixLength(document, position) : undefined,
			completionDebugRequestId:
				kind === 'completion' ? `completion:${metrics.sequence}:${Date.now()}` : undefined,
			lspRequestCount: 0,
			code2ProtocolMs: 0,
			lspRequestMs: 0,
			protocol2CodeMs: 0
		};
		providerTimingStack.push(draft);
		return draft;
	};
	const finishProviderTiming = (
		draft: ProviderTimingDraft,
		result: unknown,
		error?: unknown
	): void => {
		const metrics = providerMetricsForKind(draft.kind);
		const completedAt = Date.now();
		const totalMs = completedAt - draft.startedAt;
		draft.documentVersionAtProviderReturn = draft.document.version;
		draft.documentIsDirtyAtProviderReturn = draft.document.isDirty;
		const nextWaitMs = draft.nextStartedAt === undefined ? undefined : draft.nextStartedAt - draft.startedAt;
		const nextExecutionMs =
			draft.nextStartedAt === undefined || draft.nextCompletedAt === undefined
				? undefined
				: draft.nextCompletedAt - draft.nextStartedAt;
		const lspStartDelayMs =
			draft.lspRequestStartedAt === undefined ? undefined : draft.lspRequestStartedAt - draft.startedAt;
		const lspCompletionToProviderReturnMs =
			draft.lspRequestCompletedAt === undefined ? undefined : completedAt - draft.lspRequestCompletedAt;
		const itemCount =
			draft.kind === 'completion'
				? Array.isArray(result)
					? result.length
					: Array.isArray((result as { items?: unknown[] } | undefined)?.items)
						? ((result as { items: unknown[] }).items.length)
						: undefined
				: undefined;
		const signatureCount =
			draft.kind === 'signatureHelp'
				? Array.isArray((result as { signatures?: unknown[] } | undefined)?.signatures)
					? ((result as { signatures: unknown[] }).signatures.length)
					: undefined
				: undefined;
		const errorText =
			error === undefined ? draft.error : (error instanceof Error ? error.message : String(error));
		recordClientMetric(metrics.next, totalMs);
		recordClientMetric(metrics.providerReturn, totalMs);
		if (draft.code2ProtocolMs > 0) {
			recordClientMetric(metrics.code2Protocol, draft.code2ProtocolMs);
		}
		if (draft.lspRequestMs > 0) {
			recordClientMetric(metrics.lspRequest, draft.lspRequestMs);
		}
		if (draft.protocol2CodeMs > 0) {
			recordClientMetric(metrics.protocol2Code, draft.protocol2CodeMs);
		}
		metrics.lastTiming = {
			kind: draft.kind,
			sequence: draft.sequence,
			totalMs,
			awaitSyncMs: 0,
			code2ProtocolMs: draft.code2ProtocolMs,
			lspRequestMs: draft.lspRequestMs,
			protocol2CodeMs: draft.protocol2CodeMs,
			providerReturnMs: totalMs,
			documentUri: draft.documentUri,
			documentVersion: draft.documentVersion,
			documentIsDirty: draft.documentIsDirty,
			documentVersionAtNextStart: draft.documentVersionAtNextStart,
			documentIsDirtyAtNextStart: draft.documentIsDirtyAtNextStart,
			documentVersionAtLspStart: draft.documentVersionAtLspStart,
			documentIsDirtyAtLspStart: draft.documentIsDirtyAtLspStart,
			documentVersionAtProviderReturn: draft.documentVersionAtProviderReturn,
			documentIsDirtyAtProviderReturn: draft.documentIsDirtyAtProviderReturn,
			startedAtMs: draft.startedAt,
			completedAtMs: completedAt,
			activeSameKindProviderCountAtStart: draft.activeSameKindProviderCountAtStart,
			activeSameKindNextCountAtStart: draft.activeSameKindNextCountAtStart,
			nextStartedAtMs: draft.nextStartedAt,
			nextCompletedAtMs: draft.nextCompletedAt,
			nextWaitMs,
			nextExecutionMs,
			lspRequestStartedAtMs: draft.lspRequestStartedAt,
			lspRequestCompletedAtMs: draft.lspRequestCompletedAt,
			lspRequestCount: draft.lspRequestCount,
			lspStartDelayMs,
			lspCompletionToProviderReturnMs,
			line: draft.line,
			character: draft.character,
			triggerKind: draft.triggerKind,
			triggerCharacter: draft.triggerCharacter,
			isRetrigger: draft.isRetrigger,
			prefixLength: draft.prefixLength,
			completionCoordinatorAction: draft.completionCoordinatorAction,
			completionCoordinatorSource: draft.completionCoordinatorSource,
			completionCoordinatorKey: draft.completionCoordinatorKey,
			completionCoordinatorPrefixLength: draft.completionCoordinatorPrefixLength,
			completionDebugRequestId: draft.completionDebugRequestId,
			itemCount,
			signatureCount,
			error: errorText
		};
		metrics.recentTimings.push(metrics.lastTiming);
		if (metrics.recentTimings.length > 200) {
			metrics.recentTimings.splice(0, metrics.recentTimings.length - 200);
		}
		const stackIndex = providerTimingStack.lastIndexOf(draft);
		if (stackIndex >= 0) {
			providerTimingStack.splice(stackIndex, 1);
		}
	};
	const requestMethodName = (requestType: unknown): string => {
		if (typeof requestType === 'string') {
			return requestType;
		}
		const method = (requestType as { method?: unknown } | undefined)?.method;
		return typeof method === 'string' ? method : '';
	};
	const timingKindForMethod = (method: string): ProviderTimingKind | undefined => {
		if (method === LSP_METHOD_KEYS.completion) {
			return 'completion';
		}
		if (method === LSP_METHOD_KEYS.signatureHelp) {
			return 'signatureHelp';
		}
		return undefined;
	};
	const installProviderTimingHooks = (readyClient: LanguageClient): void => {
		const anyClient = readyClient as any;
		if (providerTimingHookClients.has(anyClient)) {
			return;
		}
		providerTimingHookClients.add(anyClient);
		const wrapConverter = (
			target: any,
			methodName: string,
			kind: ProviderTimingKind,
			field: 'code2ProtocolMs' | 'protocol2CodeMs'
		): void => {
			if (!target || typeof target[methodName] !== 'function') {
				return;
			}
			const original = target[methodName].bind(target);
			target[methodName] = (...args: unknown[]) => {
				const draft = currentProviderTiming(kind);
				if (!draft) {
					return original(...args);
				}
				const startedAt = Date.now();
				try {
					return original(...args);
				} finally {
					draft[field] += Date.now() - startedAt;
				}
			};
		};
		wrapConverter(anyClient.code2ProtocolConverter, 'asCompletionParams', 'completion', 'code2ProtocolMs');
		wrapConverter(anyClient.protocol2CodeConverter, 'asCompletionResult', 'completion', 'protocol2CodeMs');
		wrapConverter(anyClient.code2ProtocolConverter, 'asSignatureHelpParams', 'signatureHelp', 'code2ProtocolMs');
		wrapConverter(anyClient.protocol2CodeConverter, 'asSignatureHelp', 'signatureHelp', 'protocol2CodeMs');

		const originalSendRequest = anyClient.sendRequest.bind(readyClient);
		anyClient.sendRequest = (...args: unknown[]) => {
			const kind = timingKindForMethod(requestMethodName(args[0]));
			const draft = kind ? currentProviderTiming(kind) : undefined;
			if (!draft) {
				return originalSendRequest(...args);
			}
			const startedAt = Date.now();
			if (kind === 'completion' && draft.completionDebugRequestId) {
				const params = args[1];
				if (params && typeof params === 'object' && !Array.isArray(params)) {
					args[1] = {
						...(params as Record<string, unknown>),
						nsfDebugRequestId: draft.completionDebugRequestId,
						nsfDebugClientSendStartedAtMs: startedAt
					};
				}
			}
			draft.lspRequestCount++;
			draft.lspRequestStartedAt = draft.lspRequestStartedAt ?? startedAt;
			draft.documentVersionAtLspStart = draft.document.version;
			draft.documentIsDirtyAtLspStart = draft.document.isDirty;
			const completeLspRequest = (): void => {
				draft.lspRequestMs += Date.now() - startedAt;
				draft.lspRequestCompletedAt = Date.now();
			};
			try {
				const result = originalSendRequest(...args);
				if (result && typeof result.then === 'function') {
					return result.then(
						(value: unknown) => {
							completeLspRequest();
							return value;
						},
						(error: unknown) => {
							completeLspRequest();
							draft.error = error instanceof Error ? error.message : String(error);
							throw error;
						}
					);
				}
				completeLspRequest();
				return result;
			} catch (error) {
				completeLspRequest();
				draft.error = error instanceof Error ? error.message : String(error);
				throw error;
			}
		};
	};
	let pendingInitialInlayRefreshAfterIndex = false;
	let pendingInlayRefreshAfterIndexActivity = false;
	let sawIndexingEventSinceReady = false;
	let initialInlayRefreshTriggerCount = 0;
	let indexSettledInlayRefreshTriggerCount = 0;
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

	const sendSetActiveUnitNotification = async (payload: { uri?: string }): Promise<void> => {
		if (!client) {
			return;
		}
		await client.sendNotification(LSP_METHOD_KEYS.setActiveUnit, payload);
	};

	const activeUnitController = createActiveUnitController({
		context,
		unitStatusBarItem,
		hasReadyClient: () => Boolean(client?.initializeResult),
		beginRpcActivity,
		endRpcActivity,
		appendClientTrace,
		logClient,
		pushRecentClientError,
		sendSetActiveUnitNotification,
		setActiveUnitMethod: LSP_METHOD_KEYS.setActiveUnit
	});
	const updateEffectiveUnitFromDocument = activeUnitController.updateEffectiveUnitFromDocument;
	const notifyServerActiveUnit = activeUnitController.notifyServerActiveUnit;
	const setPinnedUnit = activeUnitController.setPinnedUnit;
	const metricsTracker = createMetricsTracker({
		logClient,
		onSummaryChanged: () => refreshStatusBar()
	});

	const refreshStatusBar = (): void => {
		const presentation = buildMainStatusBarPresentation({
			clientStateLabel,
			activeIndexingCount: activeIndexingTokens.size,
			lastIndexingState,
			indexingMessage,
			indexingUnit,
			activeRpcCount,
			lastRpcMethod,
			latestMetricsSummary: metricsTracker.getLatestSummary(),
			lastIndexingEvent
		});
		statusBarItem.text = presentation.text;
		statusBarItem.tooltip = presentation.tooltip;
		statusBarItem.show();
	};

	refreshStatusBar();
	let editorFeedback:
		| ReturnType<typeof createEditorFeedbackController>
		| undefined;
	const syncFeedbackState = (): void => {
		if (!editorFeedback) {
			return;
		}
		const snapshot = editorFeedback.getSnapshot();
		lastIndexingState = snapshot.lastIndexingState;
		lastIndexingEvent = snapshot.lastIndexingEvent;
		indexingMessage = snapshot.indexingMessage;
		indexingUnit = snapshot.indexingUnit;
		pendingInitialInlayRefreshAfterIndex = snapshot.pendingInitialInlayRefreshAfterIndex;
		pendingInlayRefreshAfterIndexActivity = snapshot.pendingInlayRefreshAfterIndexActivity;
		initialInlayRefreshTriggerCount = snapshot.initialInlayRefreshTriggerCount;
		indexSettledInlayRefreshTriggerCount = snapshot.indexSettledInlayRefreshTriggerCount;
		refreshStatusBar();
	};
	activeUnitController.initializeFromDocument(window.activeTextEditor?.document);

	const nsfDocumentSelector = [
		{ scheme: 'file', language: 'nsf' },
		{ scheme: 'file', language: 'hlsl' }
	];

	const createClientOptions = (): LanguageClientOptions => ({
		outputChannel: clientOutputChannel,
		traceOutputChannel: clientOutputChannel,
		revealOutputChannelOn: RevealOutputChannelOn.Error,
		documentSelector: nsfDocumentSelector,
		initializationOptions: buildRuntimeSettings(isTestMode, isPerfTestMode),
		synchronize: {
			fileEvents: workspace.createFileSystemWatcher('**/*.{nsf,hlsl}')
		},
		middleware: {
			provideCompletionItem: async (document, position, context, token, next) => {
				const timing = beginProviderTiming('completion', document, position, context);
				const markCoordinatorDecision = (decision: CompletionCoordinatorDecision): void => {
					timing.completionCoordinatorAction = decision.action;
					timing.completionCoordinatorSource = decision.source;
					timing.completionCoordinatorKey = decision.key;
					timing.completionCoordinatorPrefixLength = decision.prefixLength;
				};
				try {
					const result = await completionRequestCoordinator.coordinate(
						{ document, position, context, token },
						(executionToken) => runWithActiveProviderTiming(timing, () => next(document, position, context, executionToken)),
						markCoordinatorDecision
					);
					finishProviderTiming(timing, result);
					return result;
				} catch (error) {
					finishProviderTiming(timing, undefined, error);
					throw error;
				}
			},
			provideHover: async (document, position, token, next) => {
				const startedAt = Date.now();
				recordClientMetric(hoverClientMetrics.code2Protocol, 0);
				recordClientMetric(hoverClientMetrics.protocolRequest, 0);
				const result = await next(document, position, token);
				const durationMs = Date.now() - startedAt;
				recordClientMetric(hoverClientMetrics.protocol2Code, 0);
				recordClientMetric(hoverClientMetrics.requests, durationMs);
				recordClientMetric(hoverClientMetrics.rpc, durationMs);
				return result;
			},
			provideSignatureHelp: async (document, position, context, token, next) => {
				const timing = beginProviderTiming('signatureHelp', document, position, context);
				try {
					const result = await runWithActiveProviderTiming(timing, () =>
						next(document, position, context, token)
					);
					finishProviderTiming(timing, result);
					return result;
				} catch (error) {
					finishProviderTiming(timing, undefined, error);
					throw error;
				}
			},
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

	editorFeedback = createEditorFeedbackController({
		context,
		isTestMode,
		isPerfTestMode,
		getClient: () => client,
		ensureClientStarted: (forceRestart) => ensureClientStarted(forceRestart),
		beginRpcActivity,
		endRpcActivity,
		appendClientTrace,
		logClient,
		pushRecentClientError,
		isTransientClientRpcError,
		onStateChanged: () => {
			syncFeedbackState();
		}
	});
	syncFeedbackState();
	const sendInlayHintRequest = editorFeedback.sendInlayHintRequest;
	const sendDocumentSymbolRequest = editorFeedback.sendDocumentSymbolRequest;
	const sendWorkspaceSymbolRequest = editorFeedback.sendWorkspaceSymbolRequest;
	const collectSpamRequestResults = editorFeedback.collectSpamRequestResults;
	const fetchIndexingState = editorFeedback.fetchIndexingState;
	const requestIndexRebuild = editorFeedback.requestIndexRebuild;
	const refreshIndexingStateAndMaybeTriggerInlay = editorFeedback.refreshIndexingStateAndMaybeTriggerInlay;
	const startGitStormPolling = editorFeedback.startGitStormPolling;
	const stopGitStormPolling = editorFeedback.stopGitStormPolling;
	const scheduleIncludeUnderlineRefresh = editorFeedback.scheduleIncludeUnderlineRefresh;
	const scheduleInlayHintsRefresh = editorFeedback.scheduleInlayHintsRefresh;
	const scheduleInlayHintsRefreshWave = editorFeedback.scheduleInlayHintsRefreshWave;
	const updateRuntimeConfiguration = editorFeedback.updateRuntimeConfiguration;
	const handleIndexingNotification = editorFeedback.handleIndexingNotification;
	const handleIndexingStateChangedNotification = editorFeedback.handleIndexingStateChangedNotification;
	const handleInlayHintsChangedNotification = editorFeedback.handleInlayHintsChangedNotification;
	const fireInlayHintsChanged = editorFeedback.fireInlayHintsChanged;
	const handleMetricsNotification = (payload: MetricsPayload): void => {
		metricsTracker.handleNotification(payload);
	};
	const runtimeHost = createClientRuntimeHost({
		context,
		clientOutputChannel,
		isTestMode,
		getClient: () => client,
		setClient: (nextClient) => {
			client = nextClient;
		},
		getClientStarting: () => clientStarting,
		setClientStarting: (task) => {
			clientStarting = task;
		},
		createClientOptions,
		logClient,
		appendClientTrace,
		pushRecentClientError,
		onServerMissing: () => {
			clientStateLabel = 'error';
			refreshStatusBar();
			window.showErrorMessage('NSF: 未找到 C++ 服务端，可在设置中配置 nsf.serverPath（支持工作区相对路径）');
		},
		onWillStart: () => {
			clientStateLabel = 'starting';
			editorFeedback?.handleClientStopped();
			refreshStatusBar();
		},
		onDidChangeState: (e) => {
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
				editorFeedback?.handleClientStopped();
				refreshStatusBar();
			}
		},
		onReady: async (readyClient) => {
			appendClientTrace('language client ready');
			logClient('language client ready');
			installProviderTimingHooks(readyClient);
			clientStateLabel = 'ready';
			refreshStatusBar();
			readyClient.onNotification('nsf/indexing', handleIndexingNotification);
			readyClient.onNotification('nsf/indexingStateChanged', handleIndexingStateChangedNotification);
			readyClient.onNotification('nsf/inlayHintsChanged', handleInlayHintsChangedNotification);
			readyClient.onNotification('nsf/metrics', handleMetricsNotification);
			try {
				await seedPreprocessorMacrosSettingIfMissing({
					context,
					isTestMode,
					fetchPreset: () =>
						readyClient.sendRequest<PreprocessorMacroPresetResponse>(
							LSP_METHOD_KEYS.getPreprocessorMacroPreset,
							{}
						),
					logClient
				});
			} catch (error) {
				appendClientTrace(`seed preprocessor macros failed ${(error as Error).message ?? String(error)}`);
				logClient(`seed preprocessor macros failed ${(error as Error).message ?? String(error)}`);
				pushRecentClientError('seedPreprocessorMacros', error);
			}
			editorFeedback?.handleClientReady();
			void notifyServerActiveUnit();
			const active = window.activeTextEditor?.document;
			if (active && active.uri.scheme === 'file') {
				void updateRuntimeConfiguration(active.uri.fsPath);
			}
			if (isDefinitionTraceEnabled()) {
				clientOutputChannel.show(true);
			}
		},
		onStartFailed: () => {
			clientStateLabel = 'error';
			refreshStatusBar();
		},
		onReadyFailed: () => {
			clientStateLabel = 'error';
			refreshStatusBar();
		}
	});
	const resolveServerPath = runtimeHost.resolveServerPath;
	const ensureClientStarted = runtimeHost.ensureClientStarted;

	registerClientEditorEvents(context, {
		ensureClientStarted,
		updateRuntimeConfiguration,
		scheduleIncludeUnderlineRefresh,
		scheduleInlayHintsRefresh,
		refreshIndexingStateAndMaybeTriggerInlay,
		shouldRefreshInlayAfterIndex: () => pendingInitialInlayRefreshAfterIndex,
		updateEffectiveUnitFromDocument,
		appendClientTrace,
		logClient,
		pushRecentClientError
	});
	registerClientRuntimeEvents(context, {
		isTestMode,
		hasReadyClient: () => Boolean(client?.initializeResult),
		beginRpcActivity,
		endRpcActivity,
		sendKickIndexingNotification: async () => {
			await client!.sendNotification('nsf/kickIndexing', { reason: 'gitStorm' });
		},
		startGitStormPolling,
		appendClientTrace,
		logClient,
		pushRecentClientError,
		ensureClientStarted,
		updateRuntimeConfiguration,
		fireInlayHintsChanged,
		scheduleIncludeUnderlineRefresh,
		promptIntellisionPathIfMissing: () => promptIntellisionPathIfMissing(context, isTestMode)
	});

	const statusCommandHandlers = createStatusCommandHandlers({
		isTestMode,
		clientOutputChannel,
		logClient,
		pushRecentClientError,
		resolveServerPath,
		getClient: () => client,
		ensureClientStarted,
		getLatestMetricsSummary: () => metricsTracker.getLatestSummary(),
		getLastIndexingState: () => lastIndexingState,
		getInlayTriggerState: () => ({
			initialInlayRefreshTriggerCount,
			indexSettledInlayRefreshTriggerCount,
			pendingInitialInlayRefreshAfterIndex,
			pendingInlayRefreshAfterIndexActivity
		}),
		getRecentClientErrors: () => recentClientErrors,
		requestIndexRebuild,
		scheduleInlayHintsRefreshWave
	});

	registerUserCommands(context, {
		selectUnit: activeUnitController.selectUnitCommand,
		checkStatus: statusCommandHandlers.checkStatusCommand,
		restartServer: statusCommandHandlers.restartServerCommand,
		rebuildIndexClearCache: statusCommandHandlers.rebuildIndexClearCacheCommand
	});
	const spamHandlers = createSpamRequestHandlers({
		ensureClientStarted,
		hasReadyClient: () => Boolean(client?.initializeResult),
		sendInlayHintRequest,
		sendDocumentSymbolRequest,
		sendWorkspaceSymbolRequest,
		collectSpamRequestResults
	});
const runtimeDebugHandler = createRuntimeDebugHandler({
	ensureClientStarted,
	hasReadyClient: () => Boolean(client?.initializeResult),
	beginRpcActivity,
	endRpcActivity,
	sendRuntimeDebugRequest: (payload) => client!.sendRequest<any>('nsf/_debugDocumentRuntime', payload ?? {})
});
const replayRecorder = createRealWorkspaceReplayRecorder();
context.subscriptions.push(replayRecorder);
registerInternalCommands(context, {
		getInternalStatus: () => ({
			clientState: clientStateLabel,
			activeRpcCount,
			lastRpcMethod,
			indexingActive: activeIndexingTokens.size,
			lastIndexingEvent,
			indexingState: lastIndexingState,
			initialInlayRefreshTriggerCount,
			indexSettledInlayRefreshTriggerCount,
			pendingInitialInlayRefreshAfterIndex,
			pendingInlayRefreshAfterIndexActivity,
			completionRequestCount: completionClientMetrics.requests,
			completionAwaitSyncSamples: completionClientMetrics.awaitSync.samples,
			completionNextSamples: completionClientMetrics.next.samples,
			completionProviderReturnSamples: completionClientMetrics.providerReturn.samples,
			completionProviderReturnAvgMs: avgClientMetric(completionClientMetrics.providerReturn),
			completionProviderReturnMaxMs: completionClientMetrics.providerReturn.maxMs,
			completionCode2ProtocolSamples: completionClientMetrics.code2Protocol.samples,
			completionCode2ProtocolAvgMs: avgClientMetric(completionClientMetrics.code2Protocol),
			completionCode2ProtocolMaxMs: completionClientMetrics.code2Protocol.maxMs,
			completionLspRequestSamples: completionClientMetrics.lspRequest.samples,
			completionLspRequestAvgMs: avgClientMetric(completionClientMetrics.lspRequest),
			completionLspRequestMaxMs: completionClientMetrics.lspRequest.maxMs,
			completionProtocol2CodeSamples: completionClientMetrics.protocol2Code.samples,
			completionProtocol2CodeAvgMs: avgClientMetric(completionClientMetrics.protocol2Code),
			completionProtocol2CodeMaxMs: completionClientMetrics.protocol2Code.maxMs,
			completionLastProviderTiming: completionClientMetrics.lastTiming,
			...completionRequestCoordinator.getSnapshot(),
			completionTriggerCharacters: ['.', '_', ...'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'.split('')],
			hoverRequestCount: hoverClientMetrics.requests.samples,
			hoverRequestAvgMs: avgClientMetric(hoverClientMetrics.requests),
			hoverRequestMaxMs: hoverClientMetrics.requests.maxMs,
			hoverRpcRequestCount: hoverClientMetrics.rpc.samples,
			hoverRpcRequestAvgMs: avgClientMetric(hoverClientMetrics.rpc),
			hoverRpcRequestMaxMs: hoverClientMetrics.rpc.maxMs,
			hoverCode2ProtocolSamples: hoverClientMetrics.code2Protocol.samples,
			hoverCode2ProtocolAvgMs: avgClientMetric(hoverClientMetrics.code2Protocol),
			hoverCode2ProtocolMaxMs: hoverClientMetrics.code2Protocol.maxMs,
			hoverProtocolRequestSamples: hoverClientMetrics.protocolRequest.samples,
			hoverProtocolRequestAvgMs: avgClientMetric(hoverClientMetrics.protocolRequest),
			hoverProtocolRequestMaxMs: hoverClientMetrics.protocolRequest.maxMs,
			hoverProtocol2CodeSamples: hoverClientMetrics.protocol2Code.samples,
			hoverProtocol2CodeAvgMs: avgClientMetric(hoverClientMetrics.protocol2Code),
			hoverProtocol2CodeMaxMs: hoverClientMetrics.protocol2Code.maxMs,
			hoverExtensionHostOverheadAvgMs: Math.max(
				0,
				avgClientMetric(hoverClientMetrics.requests) - avgClientMetric(hoverClientMetrics.rpc)
			),
			hoverExtensionHostOverheadMaxMs: Math.max(
				0,
				hoverClientMetrics.requests.maxMs - hoverClientMetrics.rpc.maxMs
			),
			signatureHelpRequestCount: signatureHelpClientMetrics.requests,
			signatureHelpAwaitSyncSamples: signatureHelpClientMetrics.awaitSync.samples,
			signatureHelpNextSamples: signatureHelpClientMetrics.next.samples,
			signatureHelpProviderReturnSamples: signatureHelpClientMetrics.providerReturn.samples,
			signatureHelpProviderReturnAvgMs: avgClientMetric(signatureHelpClientMetrics.providerReturn),
			signatureHelpProviderReturnMaxMs: signatureHelpClientMetrics.providerReturn.maxMs,
			signatureHelpCode2ProtocolSamples: signatureHelpClientMetrics.code2Protocol.samples,
			signatureHelpCode2ProtocolAvgMs: avgClientMetric(signatureHelpClientMetrics.code2Protocol),
			signatureHelpCode2ProtocolMaxMs: signatureHelpClientMetrics.code2Protocol.maxMs,
			signatureHelpLspRequestSamples: signatureHelpClientMetrics.lspRequest.samples,
			signatureHelpLspRequestAvgMs: avgClientMetric(signatureHelpClientMetrics.lspRequest),
			signatureHelpLspRequestMaxMs: signatureHelpClientMetrics.lspRequest.maxMs,
			signatureHelpProtocol2CodeSamples: signatureHelpClientMetrics.protocol2Code.samples,
			signatureHelpProtocol2CodeAvgMs: avgClientMetric(signatureHelpClientMetrics.protocol2Code),
			signatureHelpProtocol2CodeMaxMs: signatureHelpClientMetrics.protocol2Code.maxMs,
			signatureHelpLastProviderTiming: signatureHelpClientMetrics.lastTiming,
			...editorFeedback?.getSnapshot()
		}),
		getProviderTimingStatus: () => ({
			completionRecentProviderTimings: completionClientMetrics.recentTimings,
			signatureHelpRecentProviderTimings: signatureHelpClientMetrics.recentTimings
		}),
		resetInternalStatus: () => {
			lastIndexingEvent = undefined;
			lastIndexingState = undefined;
			activeIndexingTokens.clear();
			indexingMessage = '';
			initialInlayRefreshTriggerCount = 0;
			indexSettledInlayRefreshTriggerCount = 0;
			Object.assign(completionClientMetrics, createProviderClientMetrics());
			completionRequestCoordinator.reset();
			hoverClientMetrics.requests = clientMetric();
			hoverClientMetrics.rpc = clientMetric();
			hoverClientMetrics.code2Protocol = clientMetric();
			hoverClientMetrics.protocolRequest = clientMetric();
			hoverClientMetrics.protocol2Code = clientMetric();
			Object.assign(signatureHelpClientMetrics, createProviderClientMetrics());
			refreshStatusBar();
		},
		clearActiveUnitForTests: createClearActiveUnitHandler({
			clearPinnedUnitState: activeUnitController.clearPinnedUnitState,
			hasReadyClient: () => Boolean(client?.initializeResult),
			beginRpcActivity,
			endRpcActivity,
			sendClearActiveUnitNotification: async () => {
				await client!.sendNotification(LSP_METHOD_KEYS.setActiveUnit, {});
			}
		}),
		setActiveUnitForTests: createSetActiveUnitHandler({
			ensureClientStarted,
			setPinnedUnit,
			sendSetActiveUnitNotification
		}),
		spamInlayRequests: spamHandlers.spamInlayRequests,
		spamDocumentSymbolRequests: spamHandlers.spamDocumentSymbolRequests,
		spamWorkspaceSymbolRequests: spamHandlers.spamWorkspaceSymbolRequests,
		getLatestMetrics: () => metricsTracker.getLatestSnapshot(),
		getMetricsHistory: (sinceRevision) => metricsTracker.getHistory(sinceRevision),
		getDocumentRuntimeDebug: runtimeDebugHandler,
		getInteractiveRuntimeDebug: async (payload) => {
			await ensureClientStarted(false);
			if (!client) {
				throw new Error('Language client is not ready yet');
			}
			return client.sendRequest<InteractiveRuntimeDebugResponse>(
				'nsf/_getInteractiveRuntimeDebug',
				payload ?? {}
			);
		},
		getWorkspaceIndexSymbolDebug: async (payload) => {
			await ensureClientStarted(false);
			if (!client) {
				throw new Error('Language client is not ready yet');
			}
			return client.sendRequest<WorkspaceIndexSymbolDebugResponse>(
				'nsf/_debugWorkspaceIndexSymbols',
				payload ?? {}
			);
		},
		sendServerRequest: async (method, params) => {
			await ensureClientStarted(false);
			if (!client) {
				throw new Error('Language client is not ready yet');
			}
			return client.sendRequest<any>(method, params ?? {});
		},
		getLastCompletionDebug: async () => {
			await ensureClientStarted(false);
			if (!client) {
				throw new Error('Language client is not ready yet');
			}
			return client.sendRequest<LastCompletionDebugResponse>('nsf/_getLastCompletionDebug', {});
		},
		startReplayRecording: (payload) => replayRecorder.start(payload ?? {}),
		stopReplayRecording: () => replayRecorder.stop()
	});

	registerWatchedFileForwarding(context, {
		scheduleIncludeUnderlineRefresh,
		hasReadyClient: () => Boolean(client?.initializeResult),
		sendWatchedFilesNotification: (changes) => client?.sendNotification('workspace/didChangeWatchedFiles', { changes })
	});

	void promptIntellisionPathIfMissing(context, isTestMode);
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
