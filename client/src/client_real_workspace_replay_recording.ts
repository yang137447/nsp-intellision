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
	lastNormalizedTarget?: ReplayRecordingTarget;
	lastRawEventKind?: RawReplayEvent['kind'];
	suppressSelectionAfterEdit: boolean;
};

function buildWorkspaceFolderSuffix(folder: vscode.WorkspaceFolder): string {
	const normalizedPath = folder.uri.fsPath.replace(/\\/g, '/');
	const parts = normalizedPath.split('/').filter((segment) => segment.length > 0);
	const worktreesIndex = parts.lastIndexOf('.worktrees');
	if (worktreesIndex > 0) {
		return parts[worktreesIndex - 1];
	}
	return parts.length > 0 ? parts[parts.length - 1] : folder.name;
}

function buildSelectionTarget(document: vscode.TextDocument, position: vscode.Position): ReplayRecordingTarget | undefined {
	const folder = vscode.workspace.getWorkspaceFolder(document.uri);
	if (!folder) {
		return undefined;
	}
	const relativePathRaw = path.relative(folder.uri.fsPath, document.uri.fsPath);
	const relativePath = relativePathRaw.replace(/\\/g, '/');
	if (relativePath.startsWith('..')) {
		return undefined;
	}
	const lineIndex = Math.min(Math.max(position.line, 0), Math.max(document.lineCount - 1, 0));
	const lineText = document.lineAt(lineIndex).text;
	return {
		workspaceFolderSuffix: buildWorkspaceFolderSuffix(folder),
		relativePath,
		anchorText: lineText,
		occurrence: 1,
		characterOffset: position.character
	};
}

function areTargetsEqual(a: ReplayRecordingTarget, b: ReplayRecordingTarget): boolean {
	return (
		a.workspaceFolderSuffix === b.workspaceFolderSuffix &&
		a.relativePath === b.relativePath &&
		a.anchorText === b.anchorText &&
		(a.characterOffset ?? 0) === (b.characterOffset ?? 0) &&
		(a.occurrence ?? 1) === (b.occurrence ?? 1)
	);
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
	const state: ReplayRecordingState = {
		active: false,
		meta: {},
		rawEvents: [],
		lastNormalizedTarget: undefined,
		lastRawEventKind: undefined,
		suppressSelectionAfterEdit: false
	};

	const pushEvent = (event: RawReplayEvent): void => {
		if (!state.active) {
			return;
		}
		state.rawEvents.push(event);
		state.lastRawEventKind = event.kind;
		if (event.kind === 'insert' || event.kind === 'delete') {
			state.suppressSelectionAfterEdit = true;
		} else if (event.kind === 'selection') {
			state.suppressSelectionAfterEdit = false;
		}
	};

	const recordSelectionTarget = (target: ReplayRecordingTarget): void => {
		if (!state.active) {
			return;
		}
		if (state.suppressSelectionAfterEdit) {
			state.suppressSelectionAfterEdit = false;
			return;
		}
		const sameTarget =
			state.lastNormalizedTarget !== undefined && areTargetsEqual(target, state.lastNormalizedTarget);
		if (sameTarget) {
			if (state.lastRawEventKind === 'insert' || state.lastRawEventKind === 'delete') {
				return;
			}
			if (state.lastRawEventKind === 'selection') {
				return;
			}
		}
		state.lastNormalizedTarget = target;
		pushEvent({ kind: 'selection', target });
	};

	const captureInitialSelection = (): void => {
		const windowEditor = vscode.window.activeTextEditor;
		if (!windowEditor) {
			return;
		}
		const selectionTarget = buildSelectionTarget(windowEditor.document, windowEditor.selection.active);
		if (selectionTarget) {
			recordSelectionTarget(selectionTarget);
		}
	};

	const disposables = [
		vscode.window.onDidChangeTextEditorSelection((event) => {
			const selection = event.selections[0];
			if (!selection) {
				return;
			}
			const target = buildSelectionTarget(event.textEditor.document, selection.active);
			if (target) {
				recordSelectionTarget(target);
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
			state.lastNormalizedTarget = undefined;
			state.lastRawEventKind = undefined;
			state.suppressSelectionAfterEdit = false;
			captureInitialSelection();
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
			state.lastNormalizedTarget = undefined;
			state.lastRawEventKind = undefined;
			state.suppressSelectionAfterEdit = false;
			return draft;
		},
		dispose() {
			vscode.Disposable.from(...disposables).dispose();
		}
	};
}
