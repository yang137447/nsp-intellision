import {
	ProgressLocation,
	window,
	workspace,
	type OutputChannel
} from 'vscode';

import type { LanguageClient } from 'vscode-languageclient/node';

import type { ServerIndexingState } from './client_indexing_status';

type RecentClientError = {
	at: number;
	source: string;
	message: string;
	stack: string;
};

type StatusCommandOptions = {
	isTestMode: boolean;
	clientOutputChannel: OutputChannel;
	logClient: (message: string) => void;
	pushRecentClientError: (source: string, error: unknown) => void;
	resolveServerPath: () => string | undefined;
	getClient: () => LanguageClient | undefined;
	ensureClientStarted: (forceRestart: boolean) => Promise<void>;
	getLatestMetricsSummary: () => string;
	getLastIndexingState: () => ServerIndexingState | undefined;
	getInlayTriggerState: () => {
		initialInlayRefreshTriggerCount: number;
		indexSettledInlayRefreshTriggerCount: number;
		pendingInitialInlayRefreshAfterIndex: boolean;
		pendingInlayRefreshAfterIndexActivity: boolean;
	};
	getRecentClientErrors: () => RecentClientError[];
	requestIndexRebuild: (reason: string, clearDiskCache: boolean) => Promise<ServerIndexingState | undefined>;
	scheduleInlayHintsRefreshWave: (delaysMs: number[]) => boolean;
};

export type StatusCommandHandlers = {
	checkStatusCommand: () => Promise<void>;
	restartServerCommand: () => Promise<void>;
	rebuildIndexClearCacheCommand: () => Promise<void>;
};

export function createStatusCommandHandlers(options: StatusCommandOptions): StatusCommandHandlers {
	const checkStatusCommand = async (): Promise<void> => {
		options.clientOutputChannel.show(true);
		const configured = workspace.getConfiguration('nsf').get<string>('serverPath', '').trim();
		const resolved = options.resolveServerPath();
		const lastIndexingState = options.getLastIndexingState();
		const inlayTriggerState = options.getInlayTriggerState();
		options.logClient(`workspaceFolders=${workspace.workspaceFolders?.length ?? 0}`);
		options.logClient(`configuredServerPath=${configured || '(empty)'}`);
		options.logClient(`resolvedServerPath=${resolved ?? '(not found)'}`);
		options.logClient(`clientCreated=${options.getClient() ? 'yes' : 'no'}`);
		options.logClient(`clientReady=${options.getClient()?.initializeResult ? 'yes' : 'no'}`);
		options.logClient(`latestMetrics=${options.getLatestMetricsSummary() || '(empty)'}`);
		options.logClient(
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
		options.logClient(`inlayTriggerState=${JSON.stringify(inlayTriggerState)}`);
		const recentClientErrors = options.getRecentClientErrors();
		if (recentClientErrors.length > 0) {
			options.logClient(`recentErrors=${recentClientErrors.length}`);
			for (const item of recentClientErrors) {
				options.logClient(`[error@${new Date(item.at).toISOString()}][${item.source}] ${item.message}`);
				if (item.stack && item.stack !== item.message) {
					options.clientOutputChannel.appendLine(item.stack);
				}
			}
		}
		try {
			await options.ensureClientStarted(false);
		} catch (error) {
			options.pushRecentClientError('nsf.checkStatus.ensureClientStarted', error);
			window.showErrorMessage(`NSF: 状态检查失败：${(error as Error).message ?? String(error)}`);
		}
	};

	const restartServerCommand = async (): Promise<void> => {
		options.clientOutputChannel.show(true);
		options.logClient('manual restart requested');
		await options.ensureClientStarted(true);
	};

	const rebuildIndexClearCacheCommand = async (): Promise<void> => {
		const continueLabel = '继续';
		if (!options.isTestMode) {
			const picked = await window.showWarningMessage(
				'NSF: 这会清除当前工作区索引缓存并执行一次完整重建。',
				{ modal: true },
				continueLabel
			);
			if (picked !== continueLabel) {
				return;
			}
		}
		options.clientOutputChannel.show(true);
		options.logClient('manual clear-cache rebuild requested');
		try {
			await options.ensureClientStarted(false);
			if (!options.getClient()?.initializeResult) {
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
					const state = await options.requestIndexRebuild(targetReason, true);
					if (!state) {
						throw new Error('Timed out waiting for the clear-cache rebuild to finish.');
					}
					progress.report({ message: '索引已稳定。' });
				}
			);
			options.scheduleInlayHintsRefreshWave([0, 120]);
			window.showInformationMessage('NSF: 已清缓存并完成完整索引重建。');
		} catch (error) {
			options.pushRecentClientError('nsf.rebuildIndexClearCache', error);
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
	};

	return {
		checkStatusCommand,
		restartServerCommand,
		rebuildIndexClearCacheCommand
	};
}
