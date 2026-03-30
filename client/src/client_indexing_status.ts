export type ServerIndexingState = {
	epoch?: number;
	state?: string;
	reason?: string;
	updatedAtMs?: number;
	pending?: {
		queuedTasks?: number;
		runningWorkers?: number;
		dirtyFiles?: number;
		probeRemainingBudget?: number;
	};
	progress?: {
		phase?: string;
		visited?: number;
		total?: number;
	};
	limits?: {
		workerCount?: number;
		queueCapacity?: number;
	};
};

export function normalizeIndexingState(payload: unknown): ServerIndexingState | undefined {
	if (!payload || typeof payload !== 'object') {
		return undefined;
	}
	return payload as ServerIndexingState;
}

export function isIndexingStateStable(state: ServerIndexingState | undefined): boolean {
	if (!state) {
		return false;
	}
	if (state.state !== 'Idle') {
		return false;
	}
	const queued = state.pending?.queuedTasks ?? 0;
	const running = state.pending?.runningWorkers ?? 0;
	return queued === 0 && running === 0;
}

export function hasServerIndexingWork(state: ServerIndexingState | undefined): boolean {
	if (!state) {
		return false;
	}
	if (state.state && state.state !== 'Idle') {
		return true;
	}
	const queued = state.pending?.queuedTasks ?? 0;
	const running = state.pending?.runningWorkers ?? 0;
	return queued > 0 || running > 0;
}

export function formatIndexingProgress(state: ServerIndexingState | undefined): string {
	if (!state) {
		return '';
	}
	const visited = state.progress?.visited;
	const total = state.progress?.total;
	if (typeof visited !== 'number') {
		return '';
	}
	if (typeof total === 'number' && total > 0) {
		return `(${visited}/${total})`;
	}
	return `(${visited}/?)`;
}
