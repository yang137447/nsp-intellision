import {
	type ExtensionContext,
	type TextDocument,
	type TextEditor,
	window,
	workspace
} from 'vscode';

type EditorEventsOptions = {
	ensureClientStarted: (forceRestart: boolean) => Promise<void>;
	updateRuntimeConfiguration: (filePath?: string) => Promise<void>;
	scheduleIncludeUnderlineRefresh: () => void;
	scheduleInlayHintsRefresh: (editor?: TextEditor) => void;
	refreshIndexingStateAndMaybeTriggerInlay: () => Promise<void>;
	shouldRefreshInlayAfterIndex: () => boolean;
	updateEffectiveUnitFromDocument: (document: TextDocument) => void;
	appendClientTrace: (message: string) => void;
	logClient: (message: string) => void;
	pushRecentClientError: (source: string, error: unknown) => void;
};

const isTrackedShaderDocument = (document: TextDocument | undefined): document is TextDocument => {
	if (!document) {
		return false;
	}
	if (document.uri.scheme !== 'file') {
		return false;
	}
	return document.languageId === 'hlsl' || document.languageId === 'nsf';
};

export function registerClientEditorEvents(context: ExtensionContext, options: EditorEventsOptions): void {
	context.subscriptions.push(workspace.onDidOpenTextDocument((document) => {
		if (!isTrackedShaderDocument(document)) {
			return;
		}
		void options.ensureClientStarted(false)
			.then(() => {
				void options.updateRuntimeConfiguration(document.uri.fsPath);
				options.scheduleIncludeUnderlineRefresh();
				if (options.shouldRefreshInlayAfterIndex()) {
					void options.refreshIndexingStateAndMaybeTriggerInlay();
				}
			})
			.catch((error) => {
				options.appendClientTrace(`ensure client on open failed ${(error as Error).message ?? String(error)}`);
				options.logClient(`ensure client on open failed ${(error as Error).message ?? String(error)}`);
				options.pushRecentClientError('onDidOpenTextDocument', error);
			});
	}));

	context.subscriptions.push(window.onDidChangeActiveTextEditor((editor) => {
		const document = editor?.document;
		if (!document) {
			return;
		}
		options.updateEffectiveUnitFromDocument(document);
		if (!isTrackedShaderDocument(document)) {
			return;
		}
		options.scheduleInlayHintsRefresh(editor);
		options.scheduleIncludeUnderlineRefresh();
		void options.ensureClientStarted(false)
			.then(() => {
				void options.updateRuntimeConfiguration(document.uri.fsPath);
				if (options.shouldRefreshInlayAfterIndex()) {
					void options.refreshIndexingStateAndMaybeTriggerInlay();
				}
			})
			.catch((error) => {
				options.appendClientTrace(`ensure client on active editor failed ${(error as Error).message ?? String(error)}`);
				options.logClient(`ensure client on active editor failed ${(error as Error).message ?? String(error)}`);
				options.pushRecentClientError('onDidChangeActiveTextEditor', error);
			});
	}));

	context.subscriptions.push(window.onDidChangeTextEditorVisibleRanges((event) => {
		options.scheduleInlayHintsRefresh(event.textEditor);
		options.scheduleIncludeUnderlineRefresh();
	}));

	context.subscriptions.push(window.onDidChangeVisibleTextEditors(() => {
		options.scheduleIncludeUnderlineRefresh();
	}));

	context.subscriptions.push(workspace.onDidChangeTextDocument((event) => {
		if (!isTrackedShaderDocument(event.document)) {
			return;
		}
		options.scheduleIncludeUnderlineRefresh();
	}));

	context.subscriptions.push(workspace.onDidSaveTextDocument((document) => {
		if (!isTrackedShaderDocument(document)) {
			return;
		}
		options.scheduleIncludeUnderlineRefresh();
	}));
}
