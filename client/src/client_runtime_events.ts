import { type ExtensionContext, type Uri, window, workspace } from 'vscode';

type RuntimeEventsOptions = {
	isTestMode: boolean;
	hasReadyClient: () => boolean;
	beginRpcActivity: (method: string) => void;
	endRpcActivity: () => void;
	sendKickIndexingNotification: () => Thenable<void> | Promise<void>;
	startGitStormPolling: () => void;
	appendClientTrace: (message: string) => void;
	logClient: (message: string) => void;
	pushRecentClientError: (source: string, error: unknown) => void;
	ensureClientStarted: (forceRestart: boolean) => Promise<void>;
	updateRuntimeConfiguration: (filePath?: string) => Promise<void>;
	fireInlayHintsChanged: () => void;
	scheduleIncludeUnderlineRefresh: () => void;
	promptIntellisionPathIfMissing: () => Promise<void> | void;
};

export function registerClientRuntimeEvents(
	context: ExtensionContext,
	options: RuntimeEventsOptions
): void {
	if (!options.isTestMode) {
		const gitStormWatcher = workspace.createFileSystemWatcher('**/*.{nsf,hlsl,fx,usf,ush}');
		const gitStormWindowMs = 350;
		const gitStormThreshold = 20;
		const gitStormUniqueThreshold = 12;
		const gitStormEvents: Array<{ at: number; key: string }> = [];
		let lastGitStormKickAtMs = 0;
		const handleGitStormFsEvent = (uri: Uri): void => {
			if (!options.hasReadyClient()) {
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
			options.beginRpcActivity('nsf/kickIndexing');
			void (async () => {
				try {
					await options.sendKickIndexingNotification();
					options.startGitStormPolling();
				} catch (error) {
					options.appendClientTrace(`send nsf/kickIndexing failed ${(error as Error).message ?? String(error)}`);
					options.logClient(`send nsf/kickIndexing failed ${(error as Error).message ?? String(error)}`);
					options.pushRecentClientError('nsf/kickIndexing', error);
				} finally {
					options.endRpcActivity();
				}
			})();
		};
		context.subscriptions.push(gitStormWatcher);
		context.subscriptions.push(gitStormWatcher.onDidChange(handleGitStormFsEvent));
		context.subscriptions.push(gitStormWatcher.onDidCreate(handleGitStormFsEvent));
		context.subscriptions.push(gitStormWatcher.onDidDelete(handleGitStormFsEvent));
	}

	context.subscriptions.push(workspace.onDidChangeConfiguration((event) => {
		if (event.affectsConfiguration('nsf.serverPath')) {
			void options.ensureClientStarted(true);
		}
		if (
			event.affectsConfiguration('nsf.intellisionPath') ||
			event.affectsConfiguration('nsf.include.validUnderline') ||
			event.affectsConfiguration('nsf.shaderFileExtensions') ||
			event.affectsConfiguration('nsf.defines') ||
			event.affectsConfiguration('nsf.inlayHints.enabled') ||
			event.affectsConfiguration('nsf.inlayHints.parameterNames') ||
			event.affectsConfiguration('nsf.semanticTokens.enabled') ||
			event.affectsConfiguration('nsf.diagnostics.mode') ||
			event.affectsConfiguration('nsf.serverPath')
		) {
			const active = window.activeTextEditor?.document;
			if (active && active.uri.scheme === 'file') {
				void options.updateRuntimeConfiguration(active.uri.fsPath);
			}
			if (
				event.affectsConfiguration('nsf.inlayHints.enabled') ||
				event.affectsConfiguration('nsf.inlayHints.parameterNames')
			) {
				options.fireInlayHintsChanged();
			}
			if (
				event.affectsConfiguration('nsf.intellisionPath') ||
				event.affectsConfiguration('nsf.include.validUnderline') ||
				event.affectsConfiguration('nsf.shaderFileExtensions')
			) {
				options.scheduleIncludeUnderlineRefresh();
			}
			if (event.affectsConfiguration('nsf.intellisionPath')) {
				void options.promptIntellisionPathIfMissing();
			}
		}
	}));
}
