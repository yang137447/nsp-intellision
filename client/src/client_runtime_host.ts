import * as fs from 'fs';
import * as path from 'path';

import { workspace, window, type ExtensionContext, type OutputChannel } from 'vscode';
import {
	LanguageClient,
	type LanguageClientOptions,
	type ServerOptions,
	State,
	TransportKind
} from 'vscode-languageclient/node';

type ClientStateChangeEvent = {
	oldState: State;
	newState: State;
};

type RuntimeHostOptions = {
	context: ExtensionContext;
	clientOutputChannel: OutputChannel;
	isTestMode: boolean;
	getClient: () => LanguageClient | undefined;
	setClient: (client: LanguageClient | undefined) => void;
	getClientStarting: () => Promise<void> | undefined;
	setClientStarting: (task: Promise<void> | undefined) => void;
	createClientOptions: () => LanguageClientOptions;
	logClient: (message: string) => void;
	appendClientTrace: (message: string) => void;
	pushRecentClientError: (source: string, error: unknown) => void;
	onServerMissing: () => void;
	onWillStart: () => void;
	onDidChangeState: (event: ClientStateChangeEvent) => void;
	onReady: (client: LanguageClient, serverPath: string) => Promise<void> | void;
	onReadyFailed: (error: unknown) => void;
	onStartFailed: (error: unknown) => void;
};

export type ClientRuntimeHost = {
	resolveServerPath: () => string | undefined;
	ensureClientStarted: (forceRestart: boolean) => Promise<void>;
};

export function createClientRuntimeHost(options: RuntimeHostOptions): ClientRuntimeHost {
	const resolveConfiguredServerPath = (configuredPath: string): string => {
		if (path.isAbsolute(configuredPath)) {
			return configuredPath;
		}
		const folder = workspace.workspaceFolders?.[0];
		if (!folder) {
			return options.context.asAbsolutePath(configuredPath);
		}
		return path.join(folder.uri.fsPath, configuredPath);
	};

	const resolveServerPath = (): string | undefined => {
		const configured = workspace.getConfiguration('nsf').get<string>('serverPath', '').trim();
		const candidates: string[] = [];
		if (configured.length > 0) {
			candidates.push(resolveConfiguredServerPath(configured));
		}
		candidates.push(options.context.asAbsolutePath(path.join('server_cpp', 'build', 'nsf_lsp.exe')));
		candidates.push(options.context.asAbsolutePath(path.join('server_cpp', 'build', 'nsf_lsp')));

		for (const candidate of candidates) {
			if (candidate.length > 0 && fs.existsSync(candidate)) {
				return candidate;
			}
		}
		return undefined;
	};

	const ensureClientStarted = async (forceRestart: boolean): Promise<void> => {
		const existingTask = options.getClientStarting();
		if (existingTask) {
			return existingTask;
		}
		let task: Promise<void> | undefined;
		task = (async () => {
			const serverPath = resolveServerPath();
			if (!serverPath) {
				options.onServerMissing();
				options.clientOutputChannel.show(true);
				return;
			}

			const currentClient = options.getClient();
			if (forceRestart && currentClient) {
				try {
					await currentClient.stop();
				} catch (error) {
					options.appendClientTrace(`language client stop failed ${(error as Error).message ?? String(error)}`);
					options.logClient(`language client stop failed ${(error as Error).message ?? String(error)}`);
					options.pushRecentClientError('client.stop', error);
				}
				options.setClient(undefined);
			}
			if (options.getClient()) {
				return;
			}

			options.onWillStart();

			const args: string[] = [];
			if (options.isTestMode && process.env.NSF_LSP_DEBUG === '1') {
				args.push('--debug-wait');
			}
			const serverOptions: ServerOptions = { command: serverPath, args, transport: TransportKind.stdio };
			const client = new LanguageClient(
				'nsfLanguageServer',
				'NSF Language Server',
				serverOptions,
				options.createClientOptions()
			);
			options.setClient(client);
			client.onDidChangeState((event) => {
				options.onDidChangeState({ oldState: event.oldState, newState: event.newState });
			});
			try {
				client.start();
			} catch (error) {
				options.onStartFailed(error);
				options.logClient(`language client start failed ${(error as Error).message ?? String(error)}`);
				options.pushRecentClientError('client.start', error);
				options.clientOutputChannel.show(true);
				throw error;
			}
			options.appendClientTrace(`language client started serverPath=${serverPath}`);
			options.logClient(`language client started serverPath=${serverPath}`);
			try {
				await client.onReady();
				await options.onReady(client, serverPath);
			} catch (error) {
				options.appendClientTrace(`language client ready failed ${(error as Error).message ?? String(error)}`);
				options.logClient(`language client ready failed ${(error as Error).message ?? String(error)}`);
				options.pushRecentClientError('client.onReady', error);
				options.clientOutputChannel.show(true);
				options.onReadyFailed(error);
				throw error;
			}
		})().finally(() => {
			if (options.getClientStarting() === task) {
				options.setClientStarting(undefined);
			}
		});
		options.setClientStarting(task);
		return task;
	};

	return {
		resolveServerPath,
		ensureClientStarted
	};
}
