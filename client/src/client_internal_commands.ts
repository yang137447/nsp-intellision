import { commands, ExtensionContext, Position, Range, Uri } from 'vscode';

type SpamRequestResult = {
	completed: number;
	cancelled: number;
	failed: number;
};

export type InternalStatusSnapshot = {
	clientState: string;
	activeRpcCount: number;
	lastRpcMethod: string;
	indexingActive: number;
	lastIndexingEvent: unknown;
	indexingState: unknown;
	initialInlayRefreshTriggerCount: number;
	indexSettledInlayRefreshTriggerCount: number;
	pendingInitialInlayRefreshAfterIndex: boolean;
	pendingInlayRefreshAfterIndexActivity: boolean;
};

export type LatestMetricsSnapshot = {
	summary: string;
	payload?: unknown;
	revision: number;
	receivedAtMs: number;
};

export type MetricsHistoryEntry = {
	summary: string;
	payload?: unknown;
	revision: number;
	receivedAtMs: number;
};

export type RuntimeDebugResponse = {
	documents: unknown[];
};

export type InternalCommandDeps = {
	getInternalStatus: () => InternalStatusSnapshot;
	resetInternalStatus: () => void;
	clearActiveUnitForTests: () => Promise<void>;
	setActiveUnitForTests: (uriText?: string) => Promise<void>;
	spamInlayRequests: (payload: {
		uri: string;
		startLine?: number;
		startCharacter?: number;
		endLine?: number;
		endCharacter?: number;
		count?: number;
	}) => Promise<SpamRequestResult>;
	spamDocumentSymbolRequests: (payload: {
		uri: string;
		count?: number;
	}) => Promise<SpamRequestResult>;
	spamWorkspaceSymbolRequests: (payload: {
		query: string;
		count?: number;
	}) => Promise<SpamRequestResult>;
	getLatestMetrics: () => LatestMetricsSnapshot;
	getMetricsHistory: (sinceRevision?: number) => MetricsHistoryEntry[];
	getDocumentRuntimeDebug: (payload?: { uris?: string[] }) => Promise<RuntimeDebugResponse>;
};

export function registerInternalCommands(
	context: ExtensionContext,
	deps: InternalCommandDeps
): void {
	context.subscriptions.push(
		commands.registerCommand('nsf._getInternalStatus', async () => deps.getInternalStatus())
	);
	context.subscriptions.push(
		commands.registerCommand('nsf._resetInternalStatus', async () => deps.resetInternalStatus())
	);
	context.subscriptions.push(
		commands.registerCommand('nsf._clearActiveUnitForTests', async () => deps.clearActiveUnitForTests())
	);
	context.subscriptions.push(
		commands.registerCommand('nsf._setActiveUnitForTests', async (uriText?: string) =>
			deps.setActiveUnitForTests(uriText)
		)
	);
	context.subscriptions.push(
		commands.registerCommand(
			'nsf._spamInlayRequests',
			async (payload: {
				uri: string;
				startLine?: number;
				startCharacter?: number;
				endLine?: number;
				endCharacter?: number;
				count?: number;
			}) => deps.spamInlayRequests(payload)
		)
	);
	context.subscriptions.push(
		commands.registerCommand(
			'nsf._spamDocumentSymbolRequests',
			async (payload: {
				uri: string;
				count?: number;
			}) => deps.spamDocumentSymbolRequests(payload)
		)
	);
	context.subscriptions.push(
		commands.registerCommand(
			'nsf._spamWorkspaceSymbolRequests',
			async (payload: {
				query: string;
				count?: number;
			}) => deps.spamWorkspaceSymbolRequests(payload)
		)
	);
	context.subscriptions.push(
		commands.registerCommand('nsf._getLatestMetrics', async () => deps.getLatestMetrics())
	);
	context.subscriptions.push(
		commands.registerCommand(
			'nsf._getMetricsHistory',
			async (payload?: { sinceRevision?: number }) =>
				deps.getMetricsHistory(Math.max(0, payload?.sinceRevision ?? 0))
		)
	);
	context.subscriptions.push(
		commands.registerCommand(
			'nsf._getDocumentRuntimeDebug',
			async (payload?: { uris?: string[] }) => deps.getDocumentRuntimeDebug(payload)
		)
	);
}

export type SpamRequestDeps = {
	ensureClientStarted: (forceRestart: boolean) => Promise<void>;
	hasReadyClient: () => boolean;
	sendInlayHintRequest: (uri: string, range: Range) => Promise<any[]>;
	sendDocumentSymbolRequest: (uri: string) => Promise<any[]>;
	sendWorkspaceSymbolRequest: (query: string) => Promise<any[]>;
	collectSpamRequestResults: <T>(tasks: Array<Promise<T>>) => Promise<SpamRequestResult>;
};

export function createSpamRequestHandlers(deps: SpamRequestDeps) {
	return {
		spamInlayRequests: async (payload: {
			uri: string;
			startLine?: number;
			startCharacter?: number;
			endLine?: number;
			endCharacter?: number;
			count?: number;
		}): Promise<SpamRequestResult> => {
			await deps.ensureClientStarted(false);
			if (!deps.hasReadyClient() || typeof payload?.uri !== 'string' || payload.uri.length === 0) {
				return { completed: 0, cancelled: 0, failed: 0 };
			}
			const count = Math.max(1, Math.min(120, payload.count ?? 20));
			const range = new Range(
				new Position(Math.max(0, payload.startLine ?? 0), Math.max(0, payload.startCharacter ?? 0)),
				new Position(Math.max(0, payload.endLine ?? 2000), Math.max(0, payload.endCharacter ?? 0))
			);
			const tasks: Array<Promise<any[]>> = [];
			for (let index = 0; index < count; index++) {
				tasks.push(deps.sendInlayHintRequest(payload.uri, range));
			}
			return deps.collectSpamRequestResults(tasks);
		},
		spamDocumentSymbolRequests: async (payload: {
			uri: string;
			count?: number;
		}): Promise<SpamRequestResult> => {
			await deps.ensureClientStarted(false);
			if (!deps.hasReadyClient() || typeof payload?.uri !== 'string' || payload.uri.length === 0) {
				return { completed: 0, cancelled: 0, failed: 0 };
			}
			const count = Math.max(1, Math.min(120, payload.count ?? 20));
			const tasks: Array<Promise<any[]>> = [];
			for (let index = 0; index < count; index++) {
				tasks.push(deps.sendDocumentSymbolRequest(payload.uri));
			}
			return deps.collectSpamRequestResults(tasks);
		},
		spamWorkspaceSymbolRequests: async (payload: {
			query: string;
			count?: number;
		}): Promise<SpamRequestResult> => {
			await deps.ensureClientStarted(false);
			if (!deps.hasReadyClient() || typeof payload?.query !== 'string') {
				return { completed: 0, cancelled: 0, failed: 0 };
			}
			const count = Math.max(1, Math.min(120, payload.count ?? 20));
			const tasks: Array<Promise<any[]>> = [];
			for (let index = 0; index < count; index++) {
				tasks.push(deps.sendWorkspaceSymbolRequest(payload.query));
			}
			return deps.collectSpamRequestResults(tasks);
		}
	};
}

export type RuntimeDebugDeps = {
	ensureClientStarted: (forceRestart: boolean) => Promise<void>;
	hasReadyClient: () => boolean;
	beginRpcActivity: (method: string) => void;
	endRpcActivity: () => void;
	sendRuntimeDebugRequest: (payload?: { uris?: string[] }) => Promise<any>;
};

export function createRuntimeDebugHandler(deps: RuntimeDebugDeps) {
	return async (payload?: { uris?: string[] }): Promise<RuntimeDebugResponse> => {
		await deps.ensureClientStarted(false);
		if (!deps.hasReadyClient()) {
			return { documents: [] };
		}
		deps.beginRpcActivity('nsf/_debugDocumentRuntime');
		try {
			const response = await deps.sendRuntimeDebugRequest(payload);
			return response ?? { documents: [] };
		} finally {
			deps.endRpcActivity();
		}
	};
}

export type ClearActiveUnitDeps = {
	clearPinnedUnitState: () => Promise<void>;
	hasReadyClient: () => boolean;
	beginRpcActivity: (method: string) => void;
	endRpcActivity: () => void;
	sendClearActiveUnitNotification: () => Promise<void>;
};

export function createClearActiveUnitHandler(deps: ClearActiveUnitDeps) {
	return async (): Promise<void> => {
		await deps.clearPinnedUnitState();
		if (!deps.hasReadyClient()) {
			return;
		}
		deps.beginRpcActivity('nsf/setActiveUnit');
		try {
			await deps.sendClearActiveUnitNotification();
		} finally {
			deps.endRpcActivity();
		}
	};
}

export type SetActiveUnitDeps = {
	ensureClientStarted: (forceRestart: boolean) => Promise<void>;
	setPinnedUnit: (uri: Uri | undefined) => Promise<void>;
};

export function createSetActiveUnitHandler(deps: SetActiveUnitDeps) {
	return async (uriText?: string): Promise<void> => {
		if (typeof uriText !== 'string' || uriText.trim().length === 0) {
			return;
		}
		await deps.ensureClientStarted(false);
		await deps.setPinnedUnit(Uri.parse(uriText));
	};
}
