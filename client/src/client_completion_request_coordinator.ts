import {
	CancellationToken,
	CancellationTokenSource,
	CompletionContext,
	CompletionItem,
	CompletionList,
	CompletionTriggerKind,
	Position,
	TextDocument
} from 'vscode';

export type CompletionCoordinatorAction =
	| 'executed'
	| 'coalescedBeforeLsp'
	| 'staleResolvedWhileInFlight'
	| 'cancelledBeforeLsp'
	| 'cancelledWhileInFlight'
	| 'bypassedExplicit'
	| 'bypassedMember'
	| 'bypassedRetrigger'
	| 'bypassedUnknown';

export type CompletionCoordinatorSnapshot = {
	completionCoordinatorReceivedCount: number;
	completionCoordinatorExecutedCount: number;
	completionCoordinatorCoalescedBeforeLspCount: number;
	completionCoordinatorStaleDroppedAfterLspCount: number;
	completionCoordinatorCancelledBeforeLspCount: number;
	completionCoordinatorCancelledWhileInFlightCount: number;
	completionCoordinatorSupersededInFlightCount: number;
	completionCoordinatorNeutralResolvedWhileInFlightCount: number;
	completionCoordinatorLatestRetainedWhileInFlightCount: number;
	completionCoordinatorDetachedLspCount: number;
	completionCoordinatorDetachedLspErrorCount: number;
	completionCoordinatorBypassedExplicitCount: number;
	completionCoordinatorBypassedMemberCount: number;
	completionCoordinatorBypassedRetriggerCount: number;
	completionCoordinatorBypassedUnknownCount: number;
};

export type CompletionCoordinatorDecision = {
	action: CompletionCoordinatorAction;
	source: 'identifierPrefixAutoTrigger' | 'explicit' | 'member' | 'retrigger' | 'unknown';
	key?: string;
	prefixLength?: number;
	identifierStartCharacter?: number;
};

type CompletionResult = CompletionList | CompletionItem[] | undefined | null;

type CompletionExecution = (token: CancellationToken) => Thenable<CompletionResult> | Promise<CompletionResult> | CompletionResult;

type CompletionRequest = {
	document: TextDocument;
	position: Position;
	context: CompletionContext;
	token: CancellationToken;
};

type CompletionClassification = CompletionCoordinatorDecision & {
	key: string;
	prefixLength: number;
};

type PendingCompletionRequest = {
	key: string;
	generation: number;
	timer: NodeJS.Timeout;
	resolve: (value: CompletionResult) => void;
	reject: (reason: unknown) => void;
	markDecision: (decision: CompletionCoordinatorDecision) => void;
	execute: CompletionExecution;
	token: CancellationToken;
	prefixLength: number;
	identifierStartCharacter?: number;
	settled: boolean;
};

type VisibleCompletionRequest = {
	key: string;
	generation: number;
	resolve: (value: CompletionResult) => void;
	reject: (reason: unknown) => void;
	markDecision: (decision: CompletionCoordinatorDecision) => void;
	cancellationSource: CancellationTokenSource;
	cancellationLink?: { dispose(): unknown };
	prefixLength: number;
	identifierStartCharacter?: number;
	stale: boolean;
	settled: boolean;
};

type CompletionKeyState = {
	pendingByGeneration: Map<number, PendingCompletionRequest>;
	visibleByGeneration: Map<number, VisibleCompletionRequest>;
};

type RecentIdentifierRequest = {
	seenAtMs: number;
	prefixLength: number;
};

const identifierPrefixDebounceMs = 25;
const burstContinuationMs = 250;
const identifierPrefixPattern = /[A-Za-z0-9_]+$/;

function emptySnapshot(): CompletionCoordinatorSnapshot {
	return {
		completionCoordinatorReceivedCount: 0,
		completionCoordinatorExecutedCount: 0,
		completionCoordinatorCoalescedBeforeLspCount: 0,
		completionCoordinatorStaleDroppedAfterLspCount: 0,
		completionCoordinatorCancelledBeforeLspCount: 0,
		completionCoordinatorCancelledWhileInFlightCount: 0,
		completionCoordinatorSupersededInFlightCount: 0,
		completionCoordinatorNeutralResolvedWhileInFlightCount: 0,
		completionCoordinatorLatestRetainedWhileInFlightCount: 0,
		completionCoordinatorDetachedLspCount: 0,
		completionCoordinatorDetachedLspErrorCount: 0,
		completionCoordinatorBypassedExplicitCount: 0,
		completionCoordinatorBypassedMemberCount: 0,
		completionCoordinatorBypassedRetriggerCount: 0,
		completionCoordinatorBypassedUnknownCount: 0
	};
}

function isIdentifierTriggerCharacter(value: string | undefined): boolean {
	return typeof value === 'string' && /^[A-Za-z0-9_]$/.test(value);
}

function completionPrefixInfo(
	document: TextDocument,
	position: Position
): { prefixLength: number; identifierStartCharacter?: number; previousCharacter?: string } {
	const lineText = document.lineAt(position.line).text.slice(0, position.character);
	const match = lineText.match(identifierPrefixPattern);
	const prefixLength = match ? match[0].length : 0;
	const identifierStartCharacter = prefixLength > 0 ? position.character - prefixLength : undefined;
	const previousCharacter =
		identifierStartCharacter !== undefined && identifierStartCharacter > 0
			? lineText[identifierStartCharacter - 1]
			: undefined;
	return { prefixLength, identifierStartCharacter, previousCharacter };
}

function isEligibleIdentifierClassification(
	decision: CompletionCoordinatorDecision
): decision is CompletionClassification {
	return (
		decision.source === 'identifierPrefixAutoTrigger' &&
		typeof decision.key === 'string' &&
		typeof decision.prefixLength === 'number'
	);
}

function canForwardSupersede(newPrefixLength: number, existingPrefixLength: number): boolean {
	return newPrefixLength >= existingPrefixLength;
}

export class CompletionRequestCoordinator {
	private readonly stateByKey = new Map<string, CompletionKeyState>();
	private readonly recentByKey = new Map<string, RecentIdentifierRequest>();
	private metrics = emptySnapshot();
	private nextGeneration = 1;

	getSnapshot(): CompletionCoordinatorSnapshot {
		return { ...this.metrics };
	}

	reset(): void {
		this.disposeActiveRequests();
		this.recentByKey.clear();
		this.metrics = emptySnapshot();
		this.nextGeneration = 1;
	}

	dispose(): void {
		this.disposeActiveRequests();
		this.recentByKey.clear();
	}

	async coordinate(
		request: CompletionRequest,
		execute: CompletionExecution,
		markDecision: (decision: CompletionCoordinatorDecision) => void
	): Promise<CompletionResult> {
		this.metrics.completionCoordinatorReceivedCount++;
		const classification = this.classify(request);
		this.rememberRecentIdentifierRequest(classification);
		this.resolveVisibleStaleRequestsForNewRequest(classification);
		if (!isEligibleIdentifierClassification(classification)) {
			this.recordBypass(classification.action);
			this.metrics.completionCoordinatorExecutedCount++;
			markDecision(classification);
			return execute(request.token);
		}

		const state = this.getOrCreateState(classification.key);
		this.coalesceSafePendingRequests(state, classification);
		const hadVisibleInFlight = this.hasVisibleInFlight(state);
		if (hadVisibleInFlight) {
			this.metrics.completionCoordinatorLatestRetainedWhileInFlightCount++;
		}

		return new Promise<CompletionResult>((resolve, reject) => {
			const generation = this.nextGeneration++;
			const pending: PendingCompletionRequest = {
				key: classification.key,
				generation,
				timer: setTimeout(() => {
					this.startPendingRequest(classification.key, generation, classification);
				}, identifierPrefixDebounceMs),
				resolve,
				reject,
				markDecision,
				execute,
				token: request.token,
				prefixLength: classification.prefixLength,
				identifierStartCharacter: classification.identifierStartCharacter,
				settled: false
			};
			state.pendingByGeneration.set(generation, pending);
		});
	}

	private classify(request: CompletionRequest): CompletionCoordinatorDecision {
		const { prefixLength, identifierStartCharacter, previousCharacter } = completionPrefixInfo(
			request.document,
			request.position
		);
		const key =
			prefixLength > 0 && identifierStartCharacter !== undefined
				? `${request.document.uri.toString()}:${request.position.line}:${identifierStartCharacter}`
				: undefined;

		const isRetrigger = (request.context as { isRetrigger?: unknown }).isRetrigger === true;
		if (isRetrigger || request.context.triggerKind === CompletionTriggerKind.TriggerForIncompleteCompletions) {
			return { action: 'bypassedRetrigger', source: 'retrigger', key, prefixLength, identifierStartCharacter };
		}
		if (request.context.triggerCharacter === '.' || previousCharacter === '.') {
			return { action: 'bypassedMember', source: 'member', key, prefixLength, identifierStartCharacter };
		}
		if (prefixLength <= 0 || identifierStartCharacter === undefined || !key) {
			return { action: 'bypassedUnknown', source: 'unknown', prefixLength };
		}

		const now = Date.now();
		const recent = this.recentByKey.get(key);
		const isRecentIdentifierBurst =
			recent !== undefined &&
			now - recent.seenAtMs <= burstContinuationMs &&
			prefixLength >= recent.prefixLength;
		const isTriggerCharacterAutoTrigger =
			request.context.triggerKind === CompletionTriggerKind.TriggerCharacter &&
			isIdentifierTriggerCharacter(request.context.triggerCharacter);
		const isAmbiguousInvokeContinuation =
			request.context.triggerKind === CompletionTriggerKind.Invoke &&
			!request.context.triggerCharacter &&
			isRecentIdentifierBurst;

		if (isTriggerCharacterAutoTrigger || isAmbiguousInvokeContinuation) {
			return {
				action: 'executed',
				source: 'identifierPrefixAutoTrigger',
				key,
				prefixLength,
				identifierStartCharacter
			};
		}
		if (request.context.triggerKind === CompletionTriggerKind.Invoke && !request.context.triggerCharacter) {
			return { action: 'bypassedExplicit', source: 'explicit', key, prefixLength, identifierStartCharacter };
		}
		return { action: 'bypassedUnknown', source: 'unknown', key, prefixLength, identifierStartCharacter };
	}

	private rememberRecentIdentifierRequest(decision: CompletionCoordinatorDecision): void {
		if (!decision.key || decision.prefixLength === undefined || decision.source === 'member') {
			return;
		}
		const now = Date.now();
		this.recentByKey.set(decision.key, {
			seenAtMs: now,
			prefixLength: decision.prefixLength
		});
		for (const [key, recent] of this.recentByKey) {
			if (now - recent.seenAtMs > burstContinuationMs * 4) {
				this.recentByKey.delete(key);
			}
		}
	}

	private getOrCreateState(key: string): CompletionKeyState {
		let state = this.stateByKey.get(key);
		if (!state) {
			state = {
				pendingByGeneration: new Map<number, PendingCompletionRequest>(),
				visibleByGeneration: new Map<number, VisibleCompletionRequest>()
			};
			this.stateByKey.set(key, state);
		}
		return state;
	}

	private hasVisibleInFlight(state: CompletionKeyState): boolean {
		for (const visible of state.visibleByGeneration.values()) {
			if (!visible.settled || visible.stale) {
				return true;
			}
		}
		return false;
	}

	private coalesceSafePendingRequests(
		state: CompletionKeyState,
		classification: CompletionClassification
	): void {
		for (const pending of [...state.pendingByGeneration.values()]) {
			if (!canForwardSupersede(classification.prefixLength, pending.prefixLength)) {
				continue;
			}
			clearTimeout(pending.timer);
			state.pendingByGeneration.delete(pending.generation);
			this.metrics.completionCoordinatorCoalescedBeforeLspCount++;
			this.settlePending(pending, 'coalescedBeforeLsp');
		}
	}

	private startPendingRequest(
		key: string,
		generation: number,
		classification: CompletionClassification
	): void {
		const state = this.stateByKey.get(key);
		const pending = state?.pendingByGeneration.get(generation);
		if (!state || !pending) {
			return;
		}
		state.pendingByGeneration.delete(generation);
		if (pending.token.isCancellationRequested) {
			this.metrics.completionCoordinatorCancelledBeforeLspCount++;
			this.settlePending(pending, 'cancelledBeforeLsp');
			this.cleanupState(key);
			return;
		}

		const visible: VisibleCompletionRequest = {
			key,
			generation,
			resolve: pending.resolve,
			reject: pending.reject,
			markDecision: pending.markDecision,
			cancellationSource: new CancellationTokenSource(),
			prefixLength: pending.prefixLength,
			identifierStartCharacter: pending.identifierStartCharacter,
			stale: false,
			settled: false
		};
		if (typeof pending.token.onCancellationRequested === 'function') {
			visible.cancellationLink = pending.token.onCancellationRequested(() => {
				visible.cancellationSource.cancel();
			});
		}
		state.visibleByGeneration.set(generation, visible);
		this.metrics.completionCoordinatorExecutedCount++;
		pending.markDecision({
			...classification,
			action: 'executed'
		});

		Promise.resolve()
			.then(() => pending.execute(visible.cancellationSource.token))
			.then(
				(result) => this.completeVisibleRequest(key, generation, result),
				(error) => this.failVisibleRequest(key, generation, error)
			);
		this.cleanupState(key);
	}

	private completeVisibleRequest(key: string, generation: number, result: CompletionResult): void {
		const state = this.stateByKey.get(key);
		const visible = state?.visibleByGeneration.get(generation);
		if (!state || !visible) {
			return;
		}
		if (visible.stale || visible.settled) {
			this.metrics.completionCoordinatorDetachedLspCount++;
		} else {
			visible.settled = true;
			visible.resolve(result);
		}
		this.disposeVisibleResources(visible);
		state.visibleByGeneration.delete(generation);
		this.cleanupState(key);
	}

	private failVisibleRequest(key: string, generation: number, error: unknown): void {
		const state = this.stateByKey.get(key);
		const visible = state?.visibleByGeneration.get(generation);
		if (!state || !visible) {
			return;
		}
		if (visible.stale || visible.settled) {
			this.metrics.completionCoordinatorDetachedLspErrorCount++;
		} else {
			visible.settled = true;
			visible.reject(error);
		}
		this.disposeVisibleResources(visible);
		state.visibleByGeneration.delete(generation);
		this.cleanupState(key);
	}

	private settlePending(pending: PendingCompletionRequest, action: CompletionCoordinatorAction): void {
		if (pending.settled) {
			return;
		}
		pending.settled = true;
		pending.markDecision({
			action,
			source: 'identifierPrefixAutoTrigger',
			key: pending.key,
			prefixLength: pending.prefixLength,
			identifierStartCharacter: pending.identifierStartCharacter
		});
		pending.resolve(undefined);
	}

	private settleVisible(visible: VisibleCompletionRequest, action: CompletionCoordinatorAction): void {
		if (visible.settled) {
			return;
		}
		visible.stale = true;
		visible.settled = true;
		visible.cancellationSource.cancel();
		visible.markDecision({
			action,
			source: 'identifierPrefixAutoTrigger',
			key: visible.key,
			prefixLength: visible.prefixLength,
			identifierStartCharacter: visible.identifierStartCharacter
		});
		visible.resolve(undefined);
	}

	private resolveVisibleStaleRequestsForNewRequest(classification: CompletionCoordinatorDecision): void {
		if (!classification.key || classification.prefixLength === undefined) {
			return;
		}
		const superseding = {
			...classification,
			key: classification.key,
			prefixLength: classification.prefixLength
		};
		for (const state of this.stateByKey.values()) {
			for (const visible of state.visibleByGeneration.values()) {
				if (visible.settled || !this.canSupersedeVisibleRequest(superseding, visible)) {
					continue;
				}
				this.metrics.completionCoordinatorSupersededInFlightCount++;
				this.metrics.completionCoordinatorNeutralResolvedWhileInFlightCount++;
				this.metrics.completionCoordinatorStaleDroppedAfterLspCount++;
				this.settleVisible(visible, 'staleResolvedWhileInFlight');
			}
		}
	}

	private canSupersedeVisibleRequest(
		classification: CompletionCoordinatorDecision & { key: string; prefixLength: number },
		visible: VisibleCompletionRequest
	): boolean {
		if (classification.key !== visible.key) {
			return true;
		}
		return canForwardSupersede(classification.prefixLength, visible.prefixLength);
	}

	private disposeVisibleResources(visible: VisibleCompletionRequest): void {
		visible.cancellationLink?.dispose();
		visible.cancellationSource.dispose();
	}

	private cleanupState(key: string): void {
		const state = this.stateByKey.get(key);
		if (!state) {
			return;
		}
		if (state.pendingByGeneration.size === 0 && state.visibleByGeneration.size === 0) {
			this.stateByKey.delete(key);
		}
	}

	private recordBypass(action: CompletionCoordinatorAction): void {
		if (action === 'bypassedExplicit') {
			this.metrics.completionCoordinatorBypassedExplicitCount++;
		} else if (action === 'bypassedMember') {
			this.metrics.completionCoordinatorBypassedMemberCount++;
		} else if (action === 'bypassedRetrigger') {
			this.metrics.completionCoordinatorBypassedRetriggerCount++;
		} else if (action === 'bypassedUnknown') {
			this.metrics.completionCoordinatorBypassedUnknownCount++;
		}
	}

	private disposeActiveRequests(): void {
		for (const [key, state] of this.stateByKey) {
			for (const pending of state.pendingByGeneration.values()) {
				clearTimeout(pending.timer);
				this.settlePending(pending, 'cancelledBeforeLsp');
			}
			for (const visible of state.visibleByGeneration.values()) {
				if (!visible.settled) {
					this.metrics.completionCoordinatorCancelledWhileInFlightCount++;
					this.settleVisible(visible, 'cancelledWhileInFlight');
				}
				this.disposeVisibleResources(visible);
			}
			state.pendingByGeneration.clear();
			state.visibleByGeneration.clear();
			this.cleanupState(key);
		}
	}
}
