import { commands, ExtensionContext } from 'vscode';

export type UserCommandDeps = {
	selectUnit: () => Promise<void>;
	checkStatus: () => Promise<void>;
	restartServer: () => Promise<void>;
	rebuildIndexClearCache: () => Promise<void>;
};

export function registerUserCommands(
	context: ExtensionContext,
	deps: UserCommandDeps
): void {
	context.subscriptions.push(commands.registerCommand('nsf.selectUnit', async () => deps.selectUnit()));
	context.subscriptions.push(commands.registerCommand('nsf.checkStatus', async () => deps.checkStatus()));
	context.subscriptions.push(commands.registerCommand('nsf.restartServer', async () => deps.restartServer()));
	context.subscriptions.push(
		commands.registerCommand('nsf.rebuildIndexClearCache', async () => deps.rebuildIndexClearCache())
	);
}
