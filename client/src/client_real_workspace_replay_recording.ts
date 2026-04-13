import * as path from 'path';
import * as vscode from 'vscode';

type ReplayRecordingTarget = {
	workspaceFolderSuffix: string;
	relativePath: string;
	anchorText: string;
	occurrence?: number;
	characterOffset?: number;
};

type RawReplayEvent =
	| { kind: 'selection'; target: ReplayRecordingTarget }
	| { kind: 'insert'; text: string }
	| { kind: 'delete'; deletedLength: number };

type ReplayRecordingStep =
	| { kind: 'typeText'; label: string; payload: { text: string } }
	| { kind: 'deleteLeft'; label: string; payload: { count: number } }
	| { kind: 'placeCursor'; label: string; target: ReplayRecordingTarget };

export type ReplayRecordingDraft = {
	id: string;
	workspaceHint: string;
	steps: ReplayRecordingStep[];
};

type ReplayRecordingState = {
	active: boolean;
	meta: Record<string, unknown>;
	rawEvents: RawReplayEvent[];
};

function buildSelectionTarget(document: vscode.TextDocument, position: vscode.Position): ReplayRecordingTarget | undefined {
	const lineIndex = Math.min(Math.max(position.line, 0), Math.max(document.lineCount - 1, 0));
	const lineText = document.lineAt(lineIndex).text;
	const folder = vscode.workspace.getWorkspaceFolder(document.uri);
	const workspaceFolderSuffix =
		folder?.uri.fsPath.replace(/\\/g, '/') ?? document.uri.fsPath.replace(/\\/g, '/');
	const relativePath = folder
		? path.relative(folder.uri.fsPath, document.uri.fsPath).replace(/\\/g, '/')
		: document.uri.fsPath.replace(/\\/g, '/');
	return {
		workspaceFolderSuffix,
		relativePath,
		anchorText: lineText,
		occurrence: 1,
		characterOffset: position.character
	};
}

function normalizeEvent(event: RawReplayEvent): ReplayRecordingStep | null {
	if (event.kind === 'insert') {
		return { kind: 'typeText', label: 'recorded type', payload: { text: event.text } };
	}
	if (event.kind === 'delete') {
		return { kind: 'deleteLeft', label: 'recorded delete', payload: { count: event.deletedLength } };
	}
	if (event.kind === 'selection') {
		return { kind: 'placeCursor', label: 'recorded cursor', target: event.target };
	}
	return null;
}

export function createRealWorkspaceReplayRecorder() {
	const state: ReplayRecordingState = { active: false, meta: {}, rawEvents: [] };

	const pushEvent = (event: RawReplayEvent): void => {
		if (!state.active) {
			return;
		}
		state.rawEvents.push(event);
	};

	const disposables = [
		vscode.window.onDidChangeTextEditorSelection((event) => {
			const selection = event.selections[0];
			if (!selection) {
				return;
			}
			const target = buildSelectionTarget(event.textEditor.document, selection.active);
			if (target) {
				pushEvent({ kind: 'selection', target });
			}
		}),
		vscode.workspace.onDidChangeTextDocument((event) => {
			for (const change of event.contentChanges) {
				if (change.text.length > 0 && change.rangeLength === 0) {
					pushEvent({ kind: 'insert', text: change.text });
				} else if (change.text.length === 0 && change.rangeLength > 0) {
					pushEvent({ kind: 'delete', deletedLength: change.rangeLength });
				} else if (change.text.length > 0 && change.rangeLength > 0) {
					pushEvent({ kind: 'delete', deletedLength: change.rangeLength });
					pushEvent({ kind: 'insert', text: change.text });
				}
			}
		})
	];

	return {
		start(meta?: Record<string, unknown>) {
			state.active = true;
			state.meta = { ...(meta ?? {}) };
			state.rawEvents = [];
		},
		stop(): ReplayRecordingDraft {
			state.active = false;
			const steps = state.rawEvents
				.map(normalizeEvent)
				.filter((step): step is ReplayRecordingStep => step !== null);
			const draft: ReplayRecordingDraft = {
				id: String(state.meta.id ?? 'recorded-replay'),
				workspaceHint: String(state.meta.workspaceHint ?? ''),
				steps
			};
			state.meta = {};
			state.rawEvents = [];
			return draft;
		},
		dispose() {
			vscode.Disposable.from(...disposables).dispose();
		}
	};
}
