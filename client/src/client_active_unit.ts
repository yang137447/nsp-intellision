import * as fs from 'fs';
import * as path from 'path';

import {
	type ExtensionContext,
	type StatusBarItem,
	type TextDocument,
	Uri,
	window,
	workspace
} from 'vscode';

type ActiveUnitOptions = {
	context: ExtensionContext;
	unitStatusBarItem: StatusBarItem;
	beginRpcActivity: (method: string) => void;
	endRpcActivity: () => void;
	appendClientTrace: (message: string) => void;
	logClient: (message: string) => void;
	pushRecentClientError: (source: string, error: unknown) => void;
	sendSetActiveUnitNotification: (payload: { uri?: string }) => Thenable<void> | Promise<void>;
	setActiveUnitMethod: string;
};

export type ActiveUnitController = {
	initializeFromDocument: (document: TextDocument | undefined) => void;
	updateEffectiveUnitFromDocument: (document: TextDocument | undefined) => void;
	notifyServerActiveUnit: () => Promise<void>;
	setPinnedUnit: (uri: Uri | undefined) => Promise<void>;
	clearPinnedUnitState: () => Promise<void>;
	selectUnitCommand: () => Promise<void>;
};

export function createActiveUnitController(options: ActiveUnitOptions): ActiveUnitController {
	const pinnedUnitStorageKey = 'nsf.pinnedUnitUri';
	const lastActiveUnitStorageKey = 'nsf.lastActiveUnitUri';
	let pinnedUnitUri: Uri | undefined;
	let lastActiveNsfUri: Uri | undefined;
	let effectiveUnitUri: Uri | undefined;
	let lastSentUnitUri = '';

	const tryParseUri = (value: unknown): Uri | undefined => {
		if (typeof value !== 'string' || value.length === 0) {
			return undefined;
		}
		try {
			return Uri.parse(value);
		} catch {
			return undefined;
		}
	};

	pinnedUnitUri = tryParseUri(options.context.workspaceState.get<string>(pinnedUnitStorageKey));
	lastActiveNsfUri = tryParseUri(options.context.workspaceState.get<string>(lastActiveUnitStorageKey));

	const isNsfUnitDocument = (document: TextDocument): boolean => {
		if (document.uri.scheme !== 'file') {
			return false;
		}
		return path.extname(document.uri.fsPath).toLowerCase() === '.nsf';
	};

	const isSameWorkspaceFolder = (a: Uri, b: Uri): boolean => {
		const wa = workspace.getWorkspaceFolder(a);
		const wb = workspace.getWorkspaceFolder(b);
		if (!wa || !wb) {
			return false;
		}
		return wa.uri.toString() === wb.uri.toString();
	};

	const findSiblingNsfUnit = (document: TextDocument): Uri | undefined => {
		if (document.uri.scheme !== 'file') {
			return undefined;
		}
		try {
			const dir = path.dirname(document.uri.fsPath);
			const entries = fs.readdirSync(dir, { withFileTypes: true });
			const nsf = entries
				.filter((entry) => entry.isFile() && entry.name.toLowerCase().endsWith('.nsf'))
				.map((entry) => entry.name)
				.sort((lhs, rhs) => lhs.localeCompare(rhs))[0];
			if (!nsf) {
				return undefined;
			}
			return Uri.file(path.join(dir, nsf));
		} catch {
			return undefined;
		}
	};

	const rememberLastActiveUnit = (uri: Uri | undefined): void => {
		lastActiveNsfUri = uri;
		void options.context.workspaceState.update(
			lastActiveUnitStorageKey,
			uri ? uri.toString() : undefined
		);
	};

	const resolveEffectiveUnit = (document: TextDocument | undefined): Uri | undefined => {
		if (pinnedUnitUri) {
			return pinnedUnitUri;
		}
		if (document && isNsfUnitDocument(document)) {
			return document.uri;
		}
		if (document && lastActiveNsfUri && isSameWorkspaceFolder(document.uri, lastActiveNsfUri)) {
			return lastActiveNsfUri;
		}
		if (document) {
			return findSiblingNsfUnit(document);
		}
		return lastActiveNsfUri;
	};

	const refreshUnitStatusBar = (): void => {
		const name = effectiveUnitUri?.scheme === 'file' ? path.basename(effectiveUnitUri.fsPath) : '';
		const prefix = pinnedUnitUri ? '$(pin) ' : '';
		options.unitStatusBarItem.text = name ? `${prefix}NSF: ${name}` : `${prefix}NSF: (no unit)`;
		options.unitStatusBarItem.tooltip = pinnedUnitUri
			? `固定工作单位：${name || pinnedUnitUri.toString()}`
			: name
				? `当前工作单位：${name}`
				: '选择 NSF 工作单位';
		options.unitStatusBarItem.show();
	};

	const notifyServerActiveUnit = async (): Promise<void> => {
		if (!effectiveUnitUri) {
			return;
		}
		const uri = effectiveUnitUri.toString();
		if (uri === lastSentUnitUri) {
			return;
		}
		lastSentUnitUri = uri;
		try {
			options.beginRpcActivity(options.setActiveUnitMethod);
			await options.sendSetActiveUnitNotification({ uri });
		} catch (error) {
			options.appendClientTrace(`send nsf/setActiveUnit failed ${(error as Error).message ?? String(error)}`);
			options.logClient(`send nsf/setActiveUnit failed ${(error as Error).message ?? String(error)}`);
			options.pushRecentClientError('nsf/setActiveUnit', error);
		} finally {
			options.endRpcActivity();
		}
	};

	const updateEffectiveUnitFromDocument = (document: TextDocument | undefined): void => {
		if (document && isNsfUnitDocument(document)) {
			rememberLastActiveUnit(document.uri);
		}
		effectiveUnitUri = resolveEffectiveUnit(document);
		refreshUnitStatusBar();
		void notifyServerActiveUnit();
	};

	const setPinnedUnit = async (uri: Uri | undefined): Promise<void> => {
		pinnedUnitUri = uri;
		if (uri) {
			await options.context.workspaceState.update(pinnedUnitStorageKey, uri.toString());
			rememberLastActiveUnit(uri);
		} else {
			await options.context.workspaceState.update(pinnedUnitStorageKey, undefined);
		}
		updateEffectiveUnitFromDocument(window.activeTextEditor?.document);
	};

	const clearPinnedUnitState = async (): Promise<void> => {
		pinnedUnitUri = undefined;
		rememberLastActiveUnit(undefined);
		effectiveUnitUri = undefined;
		lastSentUnitUri = '';
		await options.context.workspaceState.update(pinnedUnitStorageKey, undefined);
		await options.context.workspaceState.update(lastActiveUnitStorageKey, undefined);
		refreshUnitStatusBar();
	};

	const selectUnitCommand = async (): Promise<void> => {
		const activeDocument = window.activeTextEditor?.document;
		const items: Array<{ label: string; description?: string; detail?: string; action: string; uri?: Uri }> = [];

		if (pinnedUnitUri) {
			items.push({
				label: '清除固定工作单位',
				description: pinnedUnitUri.scheme === 'file' ? path.basename(pinnedUnitUri.fsPath) : pinnedUnitUri.toString(),
				action: 'clearPin'
			});
		}

		if (effectiveUnitUri) {
			items.push({
				label: '打开当前工作单位文件',
				description: effectiveUnitUri.scheme === 'file' ? path.basename(effectiveUnitUri.fsPath) : effectiveUnitUri.toString(),
				action: 'openCurrent'
			});
		}

		if (activeDocument && isNsfUnitDocument(activeDocument)) {
			items.push({
				label: '固定为当前 NSF 文件',
				description: path.basename(activeDocument.uri.fsPath),
				action: 'pin',
				uri: activeDocument.uri
			});
		}

		const nsfFiles = await workspace.findFiles('**/*.nsf', '**/{.git,node_modules,server_cpp/build*,**/build/**}', 200);
		for (const uri of nsfFiles) {
			const folder = workspace.getWorkspaceFolder(uri);
			const rel = folder ? path.relative(folder.uri.fsPath, uri.fsPath) : uri.fsPath;
			items.push({
				label: path.basename(uri.fsPath),
				description: folder ? folder.name : undefined,
				detail: rel,
				action: 'pin',
				uri
			});
		}

		const picked = await window.showQuickPick(items, {
			placeHolder: '选择 NSF 工作单位（选择文件会固定到状态栏）',
			matchOnDescription: true,
			matchOnDetail: true
		});
		if (!picked) {
			return;
		}
		if (picked.action === 'clearPin') {
			await setPinnedUnit(undefined);
			return;
		}
		if (picked.action === 'openCurrent') {
			if (!effectiveUnitUri) {
				return;
			}
			const doc = await workspace.openTextDocument(effectiveUnitUri);
			await window.showTextDocument(doc, { preview: false });
			return;
		}
		if (picked.action === 'pin' && picked.uri) {
			await setPinnedUnit(picked.uri);
		}
	};

	return {
		initializeFromDocument: (document: TextDocument | undefined) => {
			updateEffectiveUnitFromDocument(document);
		},
		updateEffectiveUnitFromDocument,
		notifyServerActiveUnit,
		setPinnedUnit,
		clearPinnedUnitState,
		selectUnitCommand
	};
}
