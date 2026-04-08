import {
	commands,
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

const isIdentifierSuggestCharacter = (text: string): boolean =>
	text.length === 1 && /[A-Za-z0-9_]/.test(text);

const isParameterHintsTriggerCharacter = (text: string): boolean =>
	text === '(' || text === ',';

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
		if (event.contentChanges.length !== 1) {
			return;
		}
		const activeEditor = window.activeTextEditor;
		if (!activeEditor || activeEditor.document.uri.toString() !== event.document.uri.toString()) {
			return;
		}
		const changedText = event.contentChanges[0].text;
		let commandId = '';
		let errorSource = '';
		if (isIdentifierSuggestCharacter(changedText)) {
			commandId = 'editor.action.triggerSuggest';
			errorSource = 'identifierSuggestFallback';
		} else if (isParameterHintsTriggerCharacter(changedText)) {
			commandId = 'editor.action.triggerParameterHints';
			errorSource = 'parameterHintsFallback';
		}
		if (!commandId) {
			return;
		}
		setTimeout(() => {
			const currentEditor = window.activeTextEditor;
			if (!currentEditor || currentEditor.document.uri.toString() !== event.document.uri.toString()) {
				return;
			}
			void Promise.resolve(commands.executeCommand('workbench.action.focusActiveEditorGroup'))
				.then(() => commands.executeCommand(commandId))
				.catch((error) => {
				options.appendClientTrace(`editor fallback ${commandId} failed ${(error as Error).message ?? String(error)}`);
				options.logClient(`editor fallback ${commandId} failed ${(error as Error).message ?? String(error)}`);
				options.pushRecentClientError(errorSource, error);
				});
		}, 75);
	}));

	context.subscriptions.push(workspace.onDidSaveTextDocument((document) => {
		if (!isTrackedShaderDocument(document)) {
			return;
		}
		options.scheduleIncludeUnderlineRefresh();
	}));
}
