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
	createClearActiveUnitHandler,
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
	promptIntellisionPathIfMissing
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

	const activeUnitController = createActiveUnitController({
		context,
		unitStatusBarItem,
		beginRpcActivity,
		endRpcActivity,
		appendClientTrace,
		logClient,
		pushRecentClientError,
		sendSetActiveUnitNotification: async (payload) => {
			if (!client) {
				return;
			}
			await client.sendNotification(LSP_METHOD_KEYS.setActiveUnit, payload);
		},
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
			clientStateLabel = 'ready';
			refreshStatusBar();
			readyClient.onNotification('nsf/indexing', handleIndexingNotification);
			readyClient.onNotification('nsf/indexingStateChanged', handleIndexingStateChangedNotification);
			readyClient.onNotification('nsf/inlayHintsChanged', handleInlayHintsChangedNotification);
			readyClient.onNotification('nsf/metrics', handleMetricsNotification);
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
			pendingInlayRefreshAfterIndexActivity
		}),
		resetInternalStatus: () => {
			lastIndexingEvent = undefined;
			lastIndexingState = undefined;
			activeIndexingTokens.clear();
			indexingMessage = '';
			initialInlayRefreshTriggerCount = 0;
			indexSettledInlayRefreshTriggerCount = 0;
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
			setPinnedUnit
		}),
		spamInlayRequests: spamHandlers.spamInlayRequests,
		spamDocumentSymbolRequests: spamHandlers.spamDocumentSymbolRequests,
		spamWorkspaceSymbolRequests: spamHandlers.spamWorkspaceSymbolRequests,
		getLatestMetrics: () => metricsTracker.getLatestSnapshot(),
		getMetricsHistory: (sinceRevision) => metricsTracker.getHistory(sinceRevision),
		getDocumentRuntimeDebug: runtimeDebugHandler
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
