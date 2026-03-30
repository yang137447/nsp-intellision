import { ExtensionContext, Uri, workspace } from 'vscode';

export type WatchedFileDeps = {
	scheduleIncludeUnderlineRefresh: () => void;
	hasReadyClient: () => boolean;
	sendWatchedFilesNotification: (changes: Array<{ uri: string; type: number }>) => Promise<void> | void;
};

export function registerWatchedFileForwarding(
	context: ExtensionContext,
	deps: WatchedFileDeps
): void {
	const watchedFileChanges = new Map<string, number>();
	let watchedFileFlushTimer: NodeJS.Timeout | undefined;

	const queueWatchedFileChange = (uri: Uri, type: number): void => {
		if (uri.scheme !== 'file') {
			return;
		}
		deps.scheduleIncludeUnderlineRefresh();
		watchedFileChanges.set(uri.toString(), type);
		if (watchedFileFlushTimer) {
			return;
		}
		watchedFileFlushTimer = setTimeout(() => {
			watchedFileFlushTimer = undefined;
			if (!deps.hasReadyClient() || watchedFileChanges.size === 0) {
				watchedFileChanges.clear();
				return;
			}
			const changes = Array.from(watchedFileChanges.entries()).map(([changeUri, changeType]) => ({
				uri: changeUri,
				type: changeType
			}));
			watchedFileChanges.clear();
			try {
				const maybePromise = deps.sendWatchedFilesNotification(changes);
				if (maybePromise && typeof (maybePromise as Promise<void>).catch === 'function') {
					void (maybePromise as Promise<void>).catch(() => undefined);
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
}
