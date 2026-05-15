import * as fs from 'fs';
import * as path from 'path';

type NumericStats = {
	count: number;
	avgMs?: number;
	p50Ms?: number;
	p95Ms?: number;
	maxMs?: number;
};

type ProbeLatencyRow = {
	label: string;
	category?: string;
	line?: number;
	uiCoverageTriggerSource?: CompletionUiCoverageTriggerSource;
	durationMs?: number;
	triggerTypingMs?: number;
	uiCommandMs?: number;
	uiTotalMs?: number;
	executeProviderMs?: number;
	clientProviderTotalMs?: number;
	clientLspRequestMs?: number;
	clientCode2ProtocolMs?: number;
	clientProtocol2CodeMs?: number;
	directServerMs?: number;
	nativeTriggerRequests?: number;
	uiTriggerRequests?: number;
	explicitInvokeOverlapRequests?: number;
	uiLatestVisibleProviderReturnMs?: number;
	uiLatestVisibleProviderExecutionMs?: number;
	uiLatestVisibleLspRequestMs?: number;
	uiLatestVisibleNextWaitMs?: number;
	uiLatestVisibleNextExecutionMs?: number;
	uiLatestVisibleLspStartDelayMs?: number;
	uiLatestVisibleLspCompletionToProviderReturnMs?: number;
	uiLatestVisibleActiveProviderOverlapAtStart?: number;
	uiLatestVisibleActiveNextOverlapAtStart?: number;
	uiLatestVisibleServerHandlerMs?: number;
	uiLatestVisibleClientResidualLspMs?: number;
	uiLatestVisibleClientToServerReceivedMs?: number;
	uiLatestVisibleServerResponseToClientResolveMs?: number;
	uiLatestVisibleServerDidChangeOverlapMs?: number;
	uiLatestVisibleServerDidChangeOverlapCount?: number;
	uiLatestVisibleServerLastDidChangeDurationMs?: number;
	uiLatestVisibleServerLastDidChangeGapMs?: number;
	uiLatestVisibleHasServerDebug?: boolean;
	postLatestVisibleCleanupMs?: number;
	uiCoverageRequests?: number;
	uiRequestBurstCount?: number;
	firstProviderRequestAtMs?: number;
	lastProviderRequestCompletedAtMs?: number;
	uiQueueQuietMs?: number;
	uiQueueQuietTimedOut?: boolean;
	providerRequests?: number;
	totalRequests?: number;
	duplicatedRequestPath?: boolean;
	itemCount?: number;
	signatureCount?: number;
	coalescingSimulation?: CompletionCoalescingProbeSimulation;
	coordinatorActual?: CompletionCoordinatorProbeActual;
	uiExecutedAttribution?: CompletionUiExecutedProbeAttribution;
};

type CompletionUiCoverageTriggerSource = 'nativeOnly' | 'explicitSuggest' | 'unknown';

type CompletionUiCoverageTriggerSourceSummary = {
	triggerSource: CompletionUiCoverageTriggerSource;
	probeCount: number;
	uiQueueQuiet: NumericStats;
	uiLatestVisibleProviderReturn: NumericStats;
	uiLatestVisibleLspStartDelay: NumericStats;
	uiLatestVisibleLspRequest: NumericStats;
	uiLatestVisibleClientResidualLsp: NumericStats;
	uiLatestVisibleClientToServerReceived: NumericStats;
	uiLatestVisibleServerResponseToClientResolve: NumericStats;
	uiLatestVisibleServerDidChangeOverlap: NumericStats;
	uiLatestVisibleServerLastDidChangeDuration: NumericStats;
	uiLatestVisibleServerLastDidChangeGap: NumericStats;
	postLatestVisibleCleanup: NumericStats;
	uiExecutedLspRequest: NumericStats;
	executeProvider: NumericStats;
	directServer: NumericStats;
	explicitInvokeOverlapRequests: number;
	latestExecutedWithServerDebugCount: number;
	slowest: ProbeLatencyRow[];
};

type CompletionCoalescingWindowSimulation = {
	debounceWindowMs: number;
	simulatedReceivedRequests: number;
	simulatedExecutedRequests: number;
	simulatedCoalescedRequests: number;
	simulatedRetainedSequences: number[];
	simulatedDroppedSequences: number[];
	latestPrefixLength?: number;
	wouldReduceBurstBy: number;
	eligibleRequestCount: number;
	bypassedRequestCount: number;
	bypassedExplicitRequestCount: number;
	bypassedMemberRequestCount: number;
	bypassedUnknownRequestCount: number;
};

type CompletionCoalescingProbeSimulation = Omit<CompletionCoalescingWindowSimulation, 'debounceWindowMs'> & {
	label: string;
	category?: string;
	line?: number;
	defaultDebounceWindowMs: number;
	windows: CompletionCoalescingWindowSimulation[];
};

type CompletionCoalescingAggregateWindowSimulation = {
	debounceWindowMs: number;
	simulatedReceivedRequests: number;
	simulatedExecutedRequests: number;
	simulatedCoalescedRequests: number;
	wouldReduceBurstBy: number;
	eligibleRequestCount: number;
	bypassedRequestCount: number;
};

type CompletionCoalescingSimulationSummary = Omit<CompletionCoalescingAggregateWindowSimulation, 'debounceWindowMs'> & {
	defaultDebounceWindowMs: number;
	probeCount: number;
	windows: CompletionCoalescingAggregateWindowSimulation[];
	probes: CompletionCoalescingProbeSimulation[];
};

type CompletionCoordinatorProbeActual = {
	label: string;
	category?: string;
	line?: number;
	receivedRequests: number;
	executedLspRequests: number;
	coalescedBeforeLspRequests: number;
	staleResolvedWhileInFlightRequests: number;
	staleDroppedAfterLspRequests: number;
	cancelledBeforeLspRequests: number;
	cancelledWhileInFlightRequests: number;
	bypassedExplicitRequests: number;
	bypassedMemberRequests: number;
	bypassedRetriggerRequests: number;
	bypassedUnknownRequests: number;
	retainedSequences: number[];
	droppedSequences: number[];
	latestPrefixLength?: number;
};

type CompletionCoordinatorActualSummary = Omit<CompletionCoordinatorProbeActual, 'label' | 'category' | 'line' | 'retainedSequences' | 'droppedSequences' | 'latestPrefixLength'> & {
	probeCount: number;
	probes: CompletionCoordinatorProbeActual[];
};

type CompletionUiExecutedProbeAttribution = {
	label: string;
	category?: string;
	line?: number;
	executedRequestCount: number;
	coalescedBeforeLspRequests: number;
	staleResolvedWhileInFlightRequests: number;
	executedSequences: number[];
	executedLspRequestMs: number[];
	executedTotalMs: number[];
	latestExecutedSequence?: number;
	latestExecutedPrefixLength?: number;
	latestExecutedRelativeStartedAtMs?: number;
	latestExecutedRelativeCompletedAtMs?: number;
	latestExecutedLspRequestMs?: number;
	latestExecutedTotalMs?: number;
	queueQuietMs?: number;
	queueQuietStartedAtMs?: number;
	queueQuietCompletedAtMs?: number;
	waitUntilLatestExecutedCompletedMs?: number;
	postLatestExecutedQuietMs?: number;
	latestExecutedHasServerDebug?: boolean;
	latestExecutedClientAttribution?: CompletionUiExecutedClientAttribution;
	latestExecutedServerAttribution?: CompletionUiExecutedServerAttribution;
};

type CompletionUiExecutedClientAttribution = {
	documentVersionAtStart?: number;
	documentIsDirtyAtStart?: boolean;
	documentVersionAtNextStart?: number;
	documentIsDirtyAtNextStart?: boolean;
	documentVersionAtLspStart?: number;
	documentIsDirtyAtLspStart?: boolean;
	documentVersionAtProviderReturn?: number;
	documentIsDirtyAtProviderReturn?: boolean;
	activeSameKindProviderCountAtStart?: number;
	activeSameKindNextCountAtStart?: number;
	nextWaitMs?: number;
	nextExecutionMs?: number;
	lspStartDelayMs?: number;
	lspRequestMs?: number;
	lspRequestCount?: number;
	lspCompletionToProviderReturnMs?: number;
	relativeNextStartedAtMs?: number;
	relativeNextCompletedAtMs?: number;
	relativeLspRequestStartedAtMs?: number;
	relativeLspRequestCompletedAtMs?: number;
	documentVersionAdvancedBeforeNext?: boolean;
	documentVersionAdvancedBeforeLsp?: boolean;
	documentVersionAdvancedDuringLsp?: boolean;
};

type CompletionUiExecutedServerAttribution = {
	clientDebugRequestId?: string;
	nsfDebugRequestId?: string;
	matchedByDebugRequestId?: boolean;
	serverRequestObserved?: boolean;
	clientSendStartedAtUnixMs?: number;
	serverReceivedAtUnixMs?: number;
	serverWorkerStartedAtUnixMs?: number;
	serverResponseWriteCompletedAtUnixMs?: number;
	clientSendToServerReceivedMs?: number;
	serverReceivedToWorkerStartMs?: number;
	serverResponseWriteToClientResolveMs?: number;
	serverDidChangeCompletedBeforeRequestCount?: number;
	serverDidChangeOverlapClientSendCount?: number;
	serverDidChangeOverlapClientSendMs?: number;
	serverLastDidChangeDurationMs?: number;
	serverLastDidChangeEndToRequestReceivedMs?: number;
	documentFound?: boolean;
	line?: number;
	character?: number;
	clientDocumentVersion?: number;
	clientDocumentIsDirty?: boolean;
	documentVersion?: number;
	requestDocumentVersion?: number;
	completionPrefix?: string;
	path?: string;
	itemCount?: number;
	requestQueueWaitMs?: number;
	requestContextBuildMs?: number;
	handlerTotalMs?: number;
	interactiveCollectMs?: number;
	memberBaseResolveMs?: number;
	memberQueryMs?: number;
	itemAssemblyMs?: number;
	responseWriteMs?: number;
	serverKnownLspMs?: number;
	clientResidualLspMs?: number;
	matchesLatestExecutedPosition?: boolean;
};

type CompletionUiExecutedAttributionSummary = {
	probeCount: number;
	executedRequestCount: number;
	lspRequest: NumericStats;
	providerTotal: NumericStats;
	waitUntilLatestExecutedCompleted: NumericStats;
	postLatestExecutedQuiet: NumericStats;
	clientNextWait: NumericStats;
	clientNextExecution: NumericStats;
	clientLspStartDelay: NumericStats;
	clientLspCompletionToProviderReturn: NumericStats;
	clientActiveProviderOverlapAtStart: NumericStats;
	clientActiveNextOverlapAtStart: NumericStats;
	documentAdvancedBeforeNextCount: number;
	documentAdvancedBeforeLspCount: number;
	documentAdvancedDuringLspCount: number;
	serverQueueWait: NumericStats;
	serverContextBuild: NumericStats;
	serverHandler: NumericStats;
	clientResidualLsp: NumericStats;
	serverDidChangeOverlapClientSend: NumericStats;
	serverLastDidChangeDuration: NumericStats;
	serverLastDidChangeEndToRequestReceived: NumericStats;
	latestExecutedWithServerDebugCount: number;
	serverDebugRequestIdMatchedCount: number;
	serverDebugRequestIdUnmatchedCount: number;
	serverDebugRequestIdFallbackCount: number;
	probes: CompletionUiExecutedProbeAttribution[];
};

type ReplayLatencySummary = {
	probeCounts: {
		total: number;
		completion: number;
		signatureHelp: number;
		diagnostics: number;
	};
	completion?: {
		capture: NumericStats;
		executeProvider: NumericStats;
		clientProviderTotal: NumericStats;
		clientLspRequest: NumericStats;
		directServer: NumericStats;
		duplicatedRequestProbeCount: number;
		uiQueueQuietTimedOutProbeCount: number;
		uiQueueQuiet: NumericStats;
		uiRequestBurst: NumericStats;
		uiLatestVisibleProviderReturn: NumericStats;
		uiLatestVisibleNextWait: NumericStats;
		uiLatestVisibleNextExecution: NumericStats;
		uiLatestVisibleLspStartDelay: NumericStats;
		uiLatestVisibleLspRequest: NumericStats;
		uiLatestVisibleLspCompletionToProviderReturn: NumericStats;
		uiLatestVisibleActiveProviderOverlapAtStart: NumericStats;
		uiLatestVisibleActiveNextOverlapAtStart: NumericStats;
		uiLatestVisibleServerHandler: NumericStats;
		uiLatestVisibleClientResidualLsp: NumericStats;
		uiLatestVisibleClientToServerReceived: NumericStats;
		uiLatestVisibleServerResponseToClientResolve: NumericStats;
		uiLatestVisibleServerDidChangeOverlap: NumericStats;
		uiLatestVisibleServerLastDidChangeDuration: NumericStats;
		uiLatestVisibleServerLastDidChangeGap: NumericStats;
		uiLatestVisibleWithServerDebugCount: number;
		postLatestVisibleCleanup: NumericStats;
		coalescingSimulation?: CompletionCoalescingSimulationSummary;
		coordinatorActual?: CompletionCoordinatorActualSummary;
		uiExecutedLspRequest: NumericStats;
		uiExecutedAttribution?: CompletionUiExecutedAttributionSummary;
		uiCoverageByTriggerSource?: CompletionUiCoverageTriggerSourceSummary[];
		slowest: ProbeLatencyRow[];
	};
	signatureHelp?: {
		capture: NumericStats;
		executeProvider: NumericStats;
		clientProviderTotal: NumericStats;
		clientLspRequest: NumericStats;
		duplicatedRequestProbeCount: number;
		uiQueueQuietTimedOutProbeCount: number;
		uiQueueQuiet: NumericStats;
		uiRequestBurst: NumericStats;
		slowest: ProbeLatencyRow[];
	};
};

type SimulatedCompletionRequest = {
	index: number;
	sequence?: number;
	triggerKind?: number;
	triggerCharacter?: string;
	line?: number;
	character?: number;
	prefixLength?: number;
	relativeStartedAtMs?: number;
	startedAtMs?: number;
	identifierStartCharacter?: number;
	bypassReason?: 'explicit' | 'member' | 'unknown';
};

const completionCoalescingDebounceWindowsMs = [25, 40, 60];
const defaultCompletionCoalescingDebounceWindowMs = 40;

function asRecord(value: unknown): Record<string, any> | undefined {
	return value && typeof value === 'object' ? value as Record<string, any> : undefined;
}

function finiteNumber(value: unknown): number | undefined {
	return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
}

function roundMs(value: number): number {
	return Math.round(value * 10) / 10;
}

function completionUiCoverageTriggerSource(capture: Record<string, any> | undefined): CompletionUiCoverageTriggerSource {
	const raw =
		asRecord(capture?.uiCoverage)?.triggerSource ??
		capture?.uiCoverageTriggerSource;
	if (raw === 'nativeOnly' || raw === 'explicitSuggest') {
		return raw;
	}
	if (asRecord(capture?.uiTrigger)) {
		return 'explicitSuggest';
	}
	return 'nativeOnly';
}

function stats(values: Array<number | undefined>): NumericStats {
	const sorted = values
		.filter((value): value is number => typeof value === 'number' && Number.isFinite(value))
		.sort((a, b) => a - b);
	if (sorted.length === 0) {
		return { count: 0 };
	}
	const percentile = (ratio: number): number => {
		const index = Math.min(sorted.length - 1, Math.max(0, Math.ceil(sorted.length * ratio) - 1));
		return sorted[index];
	};
	return {
		count: sorted.length,
		avgMs: roundMs(sorted.reduce((sum, value) => sum + value, 0) / sorted.length),
		p50Ms: roundMs(percentile(0.5)),
		p95Ms: roundMs(percentile(0.95)),
		maxMs: roundMs(sorted[sorted.length - 1])
	};
}

function requestCount(value: unknown, field: string): number | undefined {
	const raw = asRecord(value)?.[field];
	return typeof raw === 'number' && Number.isFinite(raw) ? raw : undefined;
}

function identifierCharacter(value: string | undefined): boolean {
	return typeof value === 'string' && /^[A-Za-z0-9_]$/.test(value);
}

function sequenceValues(entries: SimulatedCompletionRequest[]): number[] {
	return entries
		.map((entry) => entry.sequence)
		.filter((sequence): sequence is number => typeof sequence === 'number' && Number.isFinite(sequence));
}

function requestSortKey(entry: SimulatedCompletionRequest): number {
	const relativeStartedAtMs = finiteNumber(entry.relativeStartedAtMs);
	if (relativeStartedAtMs !== undefined) {
		return relativeStartedAtMs;
	}
	const startedAtMs = finiteNumber(entry.startedAtMs);
	if (startedAtMs !== undefined) {
		return startedAtMs;
	}
	const sequence = finiteNumber(entry.sequence);
	if (sequence !== undefined) {
		return sequence;
	}
	return entry.index;
}

function requestTime(entry: SimulatedCompletionRequest): number | undefined {
	return finiteNumber(entry.relativeStartedAtMs) ?? finiteNumber(entry.startedAtMs);
}

function normalizeCompletionRequestSequence(value: unknown): SimulatedCompletionRequest[] {
	if (!Array.isArray(value)) {
		return [];
	}
	return value
		.map((raw, index): SimulatedCompletionRequest => {
			const record = asRecord(raw) ?? {};
			const line = finiteNumber(record.line);
			const character = finiteNumber(record.character);
			const prefixLength = finiteNumber(record.prefixLength);
			const identifierStartCharacter =
				line !== undefined && character !== undefined && prefixLength !== undefined && prefixLength > 0
					? character - prefixLength
					: undefined;
			return {
				index,
				sequence: finiteNumber(record.sequence),
				triggerKind: finiteNumber(record.triggerKind),
				triggerCharacter: typeof record.triggerCharacter === 'string' ? record.triggerCharacter : undefined,
				line,
				character,
				prefixLength,
				relativeStartedAtMs: finiteNumber(record.relativeStartedAtMs),
				startedAtMs: finiteNumber(record.startedAtMs),
				identifierStartCharacter
			};
		})
		.sort((lhs, rhs) => requestSortKey(lhs) - requestSortKey(rhs) || lhs.index - rhs.index);
}

function completionRequestGroupKey(entry: SimulatedCompletionRequest): string | undefined {
	if (entry.triggerCharacter === '.') {
		return undefined;
	}
	if (
		entry.line === undefined ||
		entry.character === undefined ||
		entry.prefixLength === undefined ||
		entry.identifierStartCharacter === undefined ||
		entry.prefixLength <= 0 ||
		entry.identifierStartCharacter < 0
	) {
		return undefined;
	}
	return `${entry.line}:${entry.identifierStartCharacter}`;
}

function isMonotonicPrefixGroup(entries: SimulatedCompletionRequest[]): boolean {
	let previousPrefix = -1;
	for (const entry of entries) {
		if (entry.prefixLength === undefined) {
			return false;
		}
		if (previousPrefix >= 0 && entry.prefixLength < previousPrefix) {
			return false;
		}
		previousPrefix = entry.prefixLength;
	}
	return true;
}

function hasPrefixProgression(entries: SimulatedCompletionRequest[]): boolean {
	const prefixes = new Set(entries.map((entry) => entry.prefixLength).filter((value): value is number => value !== undefined));
	return prefixes.size > 1;
}

function hasDuplicatePrefix(entries: SimulatedCompletionRequest[]): boolean {
	const seen = new Set<number>();
	for (const entry of entries) {
		if (entry.prefixLength === undefined) {
			continue;
		}
		if (seen.has(entry.prefixLength)) {
			return true;
		}
		seen.add(entry.prefixLength);
	}
	return false;
}

function classifyCompletionSimulationRequests(
	entries: SimulatedCompletionRequest[],
	capture: Record<string, any> | undefined,
	category: string | undefined
): { eligible: SimulatedCompletionRequest[]; bypassed: SimulatedCompletionRequest[] } {
	const isProbeLevelMember =
		category?.startsWith('completion.member') === true || capture?.triggerCharacter === '.';
	const isProbeLevelExplicit = capture?.triggerKind === 'Invoked' && typeof capture?.triggerCharacter !== 'string';
	if (isProbeLevelMember || isProbeLevelExplicit) {
		const reason = isProbeLevelMember ? 'member' : 'explicit';
		return {
			eligible: [],
			bypassed: entries.map((entry) => ({ ...entry, bypassReason: reason }))
		};
	}

	const grouped = new Map<string, SimulatedCompletionRequest[]>();
	const bypassed: SimulatedCompletionRequest[] = [];
	for (const entry of entries) {
		const key = completionRequestGroupKey(entry);
		if (!key) {
			const reason = entry.triggerCharacter === '.' ? 'member' : entry.triggerKind === 1 ? 'explicit' : 'unknown';
			bypassed.push({ ...entry, bypassReason: reason });
			continue;
		}
		const group = grouped.get(key);
		if (group) {
			group.push(entry);
		} else {
			grouped.set(key, [entry]);
		}
	}

	const eligible: SimulatedCompletionRequest[] = [];
	for (const group of grouped.values()) {
		const sortedGroup = [...group].sort((lhs, rhs) => requestSortKey(lhs) - requestSortKey(rhs) || lhs.index - rhs.index);
		const looksLikeIdentifierBurst =
			sortedGroup.length > 1 &&
			isMonotonicPrefixGroup(sortedGroup) &&
			(hasPrefixProgression(sortedGroup) ||
				hasDuplicatePrefix(sortedGroup) ||
				sortedGroup.some((entry) => identifierCharacter(entry.triggerCharacter)));
		if (looksLikeIdentifierBurst) {
			eligible.push(...sortedGroup);
		} else {
			const reason: NonNullable<SimulatedCompletionRequest['bypassReason']> = sortedGroup.every((entry) => entry.triggerKind === 1 && !entry.triggerCharacter)
				? 'explicit'
				: 'unknown';
			bypassed.push(...sortedGroup.map((entry) => ({ ...entry, bypassReason: reason })));
		}
	}

	return { eligible, bypassed };
}

function splitCompletionBursts(entries: SimulatedCompletionRequest[]): SimulatedCompletionRequest[][] {
	const bursts: SimulatedCompletionRequest[][] = [];
	let current: SimulatedCompletionRequest[] = [];
	let previousPrefix = -1;
	for (const entry of entries) {
		const prefixLength = entry.prefixLength ?? -1;
		if (current.length > 0 && prefixLength < previousPrefix) {
			bursts.push(current);
			current = [];
		}
		current.push(entry);
		previousPrefix = prefixLength;
	}
	if (current.length > 0) {
		bursts.push(current);
	}
	return bursts;
}

function collapseFinalPrefixDuplicates(
	entries: SimulatedCompletionRequest[]
): { retainedCandidates: SimulatedCompletionRequest[]; dropped: SimulatedCompletionRequest[] } {
	if (entries.length <= 1) {
		return { retainedCandidates: entries, dropped: [] };
	}
	const finalPrefix = entries[entries.length - 1].prefixLength;
	if (finalPrefix === undefined) {
		return { retainedCandidates: entries, dropped: [] };
	}
	let finalRunStart = entries.length - 1;
	while (finalRunStart > 0 && entries[finalRunStart - 1].prefixLength === finalPrefix) {
		finalRunStart--;
	}
	if (finalRunStart === entries.length - 1) {
		return { retainedCandidates: entries, dropped: [] };
	}
	const dropped = entries.slice(finalRunStart, entries.length - 1);
	return {
		retainedCandidates: [...entries.slice(0, finalRunStart), entries[entries.length - 1]],
		dropped
	};
}

function retainLatestPerWindow(
	entries: SimulatedCompletionRequest[],
	debounceWindowMs: number
): { retained: SimulatedCompletionRequest[]; dropped: SimulatedCompletionRequest[] } {
	const retained: SimulatedCompletionRequest[] = [];
	const dropped: SimulatedCompletionRequest[] = [];
	let bucket: SimulatedCompletionRequest[] = [];
	let bucketStartedAt: number | undefined;
	const flush = (): void => {
		if (bucket.length === 0) {
			return;
		}
		retained.push(bucket[bucket.length - 1]);
		dropped.push(...bucket.slice(0, -1));
		bucket = [];
		bucketStartedAt = undefined;
	};

	for (const entry of entries) {
		const startedAt = requestTime(entry);
		if (startedAt === undefined) {
			flush();
			retained.push(entry);
			continue;
		}
		if (
			bucket.length === 0 ||
			bucketStartedAt === undefined ||
			startedAt - bucketStartedAt <= debounceWindowMs
		) {
			bucket.push(entry);
			if (bucketStartedAt === undefined) {
				bucketStartedAt = startedAt;
			}
			continue;
		}
		flush();
		bucket.push(entry);
		bucketStartedAt = startedAt;
	}
	flush();
	return { retained, dropped };
}

function simulateCompletionWindow(
	entries: SimulatedCompletionRequest[],
	capture: Record<string, any> | undefined,
	category: string | undefined,
	debounceWindowMs: number
): CompletionCoalescingWindowSimulation {
	const { eligible, bypassed } = classifyCompletionSimulationRequests(entries, capture, category);
	const retained: SimulatedCompletionRequest[] = [...bypassed];
	const dropped: SimulatedCompletionRequest[] = [];

	const groups = new Map<string, SimulatedCompletionRequest[]>();
	for (const entry of eligible) {
		const key = completionRequestGroupKey(entry);
		if (!key) {
			retained.push({ ...entry, bypassReason: 'unknown' });
			continue;
		}
		const group = groups.get(key);
		if (group) {
			group.push(entry);
		} else {
			groups.set(key, [entry]);
		}
	}

	for (const group of groups.values()) {
		const sortedGroup = [...group].sort((lhs, rhs) => requestSortKey(lhs) - requestSortKey(rhs) || lhs.index - rhs.index);
		for (const burst of splitCompletionBursts(sortedGroup)) {
			const duplicateCollapsed = collapseFinalPrefixDuplicates(burst);
			dropped.push(...duplicateCollapsed.dropped);
			const windowResult = retainLatestPerWindow(duplicateCollapsed.retainedCandidates, debounceWindowMs);
			retained.push(...windowResult.retained);
			dropped.push(...windowResult.dropped);
		}
	}

	const retainedSorted = retained.sort((lhs, rhs) => requestSortKey(lhs) - requestSortKey(rhs) || lhs.index - rhs.index);
	const droppedSorted = dropped.sort((lhs, rhs) => requestSortKey(lhs) - requestSortKey(rhs) || lhs.index - rhs.index);
	const prefixLengths = eligible
		.map((entry) => entry.prefixLength)
		.filter((value): value is number => typeof value === 'number' && Number.isFinite(value));
	const latestPrefixLength = prefixLengths[prefixLengths.length - 1];
	return {
		debounceWindowMs,
		simulatedReceivedRequests: entries.length,
		simulatedExecutedRequests: retainedSorted.length,
		simulatedCoalescedRequests: droppedSorted.length,
		simulatedRetainedSequences: sequenceValues(retainedSorted),
		simulatedDroppedSequences: sequenceValues(droppedSorted),
		latestPrefixLength,
		wouldReduceBurstBy: droppedSorted.length,
		eligibleRequestCount: eligible.length,
		bypassedRequestCount: bypassed.length,
		bypassedExplicitRequestCount: bypassed.filter((entry) => entry.bypassReason === 'explicit').length,
		bypassedMemberRequestCount: bypassed.filter((entry) => entry.bypassReason === 'member').length,
		bypassedUnknownRequestCount: bypassed.filter((entry) => entry.bypassReason === 'unknown').length
	};
}

function completionCoalescingSimulation(probe: Record<string, any>, capture: Record<string, any> | undefined): CompletionCoalescingProbeSimulation | undefined {
	const uiCoverage = asRecord(capture?.uiCoverage);
	const requests = normalizeCompletionRequestSequence(uiCoverage?.providerRequestSequence);
	if (requests.length === 0) {
		return undefined;
	}
	const category = typeof probe.category === 'string' ? probe.category : undefined;
	const windows = completionCoalescingDebounceWindowsMs.map((windowMs) =>
		simulateCompletionWindow(requests, capture, category, windowMs)
	);
	const defaultWindow =
		windows.find((window) => window.debounceWindowMs === defaultCompletionCoalescingDebounceWindowMs) ??
		windows[0];
	return {
		label: String(probe.label ?? ''),
		category,
		line: finiteNumber(probe.line),
		defaultDebounceWindowMs: defaultWindow.debounceWindowMs,
		simulatedReceivedRequests: defaultWindow.simulatedReceivedRequests,
		simulatedExecutedRequests: defaultWindow.simulatedExecutedRequests,
		simulatedCoalescedRequests: defaultWindow.simulatedCoalescedRequests,
		simulatedRetainedSequences: defaultWindow.simulatedRetainedSequences,
		simulatedDroppedSequences: defaultWindow.simulatedDroppedSequences,
		latestPrefixLength: defaultWindow.latestPrefixLength,
		wouldReduceBurstBy: defaultWindow.wouldReduceBurstBy,
		eligibleRequestCount: defaultWindow.eligibleRequestCount,
		bypassedRequestCount: defaultWindow.bypassedRequestCount,
		bypassedExplicitRequestCount: defaultWindow.bypassedExplicitRequestCount,
		bypassedMemberRequestCount: defaultWindow.bypassedMemberRequestCount,
		bypassedUnknownRequestCount: defaultWindow.bypassedUnknownRequestCount,
		windows
	};
}

function aggregateCompletionCoalescingWindow(
	probes: CompletionCoalescingProbeSimulation[],
	debounceWindowMs: number
): CompletionCoalescingAggregateWindowSimulation {
	const windows = probes
		.map((probe) => probe.windows.find((window) => window.debounceWindowMs === debounceWindowMs))
		.filter((window): window is CompletionCoalescingWindowSimulation => window !== undefined);
	const sum = (selector: (window: CompletionCoalescingWindowSimulation) => number): number =>
		windows.reduce((total, window) => total + selector(window), 0);
	return {
		debounceWindowMs,
		simulatedReceivedRequests: sum((window) => window.simulatedReceivedRequests),
		simulatedExecutedRequests: sum((window) => window.simulatedExecutedRequests),
		simulatedCoalescedRequests: sum((window) => window.simulatedCoalescedRequests),
		wouldReduceBurstBy: sum((window) => window.wouldReduceBurstBy),
		eligibleRequestCount: sum((window) => window.eligibleRequestCount),
		bypassedRequestCount: sum((window) => window.bypassedRequestCount)
	};
}

function buildCompletionCoalescingSimulationSummary(rows: ProbeLatencyRow[]): CompletionCoalescingSimulationSummary | undefined {
	const probes = rows
		.map((row) => row.coalescingSimulation)
		.filter((simulation): simulation is CompletionCoalescingProbeSimulation => simulation !== undefined);
	if (probes.length === 0) {
		return undefined;
	}
	const windows = completionCoalescingDebounceWindowsMs.map((windowMs) =>
		aggregateCompletionCoalescingWindow(probes, windowMs)
	);
	const defaultWindow =
		windows.find((window) => window.debounceWindowMs === defaultCompletionCoalescingDebounceWindowMs) ??
		windows[0];
	return {
		defaultDebounceWindowMs: defaultWindow.debounceWindowMs,
		probeCount: probes.length,
		simulatedReceivedRequests: defaultWindow.simulatedReceivedRequests,
		simulatedExecutedRequests: defaultWindow.simulatedExecutedRequests,
		simulatedCoalescedRequests: defaultWindow.simulatedCoalescedRequests,
		wouldReduceBurstBy: defaultWindow.wouldReduceBurstBy,
		eligibleRequestCount: defaultWindow.eligibleRequestCount,
		bypassedRequestCount: defaultWindow.bypassedRequestCount,
		windows,
		probes
	};
}

function completionProviderRequestSequence(probe: Record<string, any>, capture: Record<string, any> | undefined): Array<Record<string, any>> {
	const triggerTypingSequence = asRecord(probe.triggerTyping)?.providerRequestSequence;
	const uiCoverageSequence = asRecord(capture?.uiCoverage)?.providerRequestSequence;
	const entries = [
		...(Array.isArray(triggerTypingSequence) ? triggerTypingSequence : []),
		...(Array.isArray(uiCoverageSequence) ? uiCoverageSequence : [])
	]
		.map(asRecord)
		.filter((entry): entry is Record<string, any> => entry !== undefined);
	const seenSequences = new Set<number>();
	const deduped: Array<Record<string, any>> = [];
	for (const entry of entries) {
		const sequence = finiteNumber(entry.sequence);
		if (sequence !== undefined) {
			if (seenSequences.has(sequence)) {
				continue;
			}
			seenSequences.add(sequence);
		}
		deduped.push(entry);
	}
	return deduped.sort((lhs, rhs) => {
		const lhsTime = finiteNumber(lhs.startedAtMs) ?? finiteNumber(lhs.relativeStartedAtMs) ?? finiteNumber(lhs.sequence) ?? 0;
		const rhsTime = finiteNumber(rhs.startedAtMs) ?? finiteNumber(rhs.relativeStartedAtMs) ?? finiteNumber(rhs.sequence) ?? 0;
		return lhsTime - rhsTime;
	});
}

function uiCoverageProviderRequestSequence(capture: Record<string, any> | undefined): Array<Record<string, any>> {
	const uiCoverageSequence = asRecord(capture?.uiCoverage)?.providerRequestSequence;
	return (Array.isArray(uiCoverageSequence) ? uiCoverageSequence : [])
		.map(asRecord)
		.filter((entry): entry is Record<string, any> => entry !== undefined)
		.sort((lhs, rhs) => {
			const lhsTime = finiteNumber(lhs.startedAtMs) ?? finiteNumber(lhs.relativeStartedAtMs) ?? finiteNumber(lhs.sequence) ?? 0;
			const rhsTime = finiteNumber(rhs.startedAtMs) ?? finiteNumber(rhs.relativeStartedAtMs) ?? finiteNumber(rhs.sequence) ?? 0;
			return lhsTime - rhsTime;
		});
}

function explicitInvokeOverlapRequestCount(capture: Record<string, any> | undefined): number {
	if (completionUiCoverageTriggerSource(capture) !== 'explicitSuggest') {
		return 0;
	}
	return uiCoverageProviderRequestSequence(capture).filter((entry) =>
		entry.completionCoordinatorAction === 'bypassedExplicit' ||
		entry.completionCoordinatorSource === 'explicit'
	).length;
}

function isVisibleCompletionProviderReturn(entry: Record<string, any>): boolean {
	const action = typeof entry.completionCoordinatorAction === 'string'
		? entry.completionCoordinatorAction
		: undefined;
	if (
		action === 'coalescedBeforeLsp' ||
		action === 'staleResolvedWhileInFlight' ||
		action === 'staleDroppedAfterLsp' ||
		action === 'cancelledBeforeLsp' ||
		action === 'cancelledWhileInFlight'
	) {
		return false;
	}
	return finiteNumber(entry.relativeCompletedAtMs) !== undefined;
}

function latestVisibleCompletionProviderReturn(
	capture: Record<string, any> | undefined
): Record<string, any> | undefined {
	const visible = uiCoverageProviderRequestSequence(capture).filter(isVisibleCompletionProviderReturn);
	return visible[visible.length - 1];
}

function postLatestVisibleCleanupMs(
	capture: Record<string, any> | undefined,
	latestVisible: Record<string, any> | undefined
): number | undefined {
	const latestCompletedAtMs = finiteNumber(latestVisible?.relativeCompletedAtMs);
	const queueQuietCompletedAtMs = finiteNumber(
		asRecord(asRecord(capture?.uiCoverage)?.queueQuiet)?.relativeCompletedAtMs
	);
	if (latestCompletedAtMs === undefined || queueQuietCompletedAtMs === undefined) {
		return undefined;
	}
	return Math.max(0, queueQuietCompletedAtMs - latestCompletedAtMs);
}

function completionCoordinatorActual(probe: Record<string, any>, capture: Record<string, any> | undefined): CompletionCoordinatorProbeActual | undefined {
	const entries = completionProviderRequestSequence(probe, capture);
	if (entries.length === 0) {
		return undefined;
	}
	let executedLspRequests = 0;
	let coalescedBeforeLspRequests = 0;
	let staleResolvedWhileInFlightRequests = 0;
	let staleDroppedAfterLspRequests = 0;
	let cancelledBeforeLspRequests = 0;
	let cancelledWhileInFlightRequests = 0;
	let bypassedExplicitRequests = 0;
	let bypassedMemberRequests = 0;
	let bypassedRetriggerRequests = 0;
	let bypassedUnknownRequests = 0;
	const retained: Array<Record<string, any>> = [];
	const dropped: Array<Record<string, any>> = [];
	const prefixLengths: number[] = [];

	for (const entry of entries) {
		const action = typeof entry.completionCoordinatorAction === 'string'
			? entry.completionCoordinatorAction
			: undefined;
		const prefixLength = finiteNumber(entry.completionCoordinatorPrefixLength) ?? finiteNumber(entry.prefixLength);
		if (prefixLength !== undefined) {
			prefixLengths.push(prefixLength);
		}
		if (action === 'coalescedBeforeLsp') {
			coalescedBeforeLspRequests++;
			dropped.push(entry);
		} else if (action === 'staleResolvedWhileInFlight') {
			staleResolvedWhileInFlightRequests++;
			executedLspRequests++;
			dropped.push(entry);
		} else if (action === 'staleDroppedAfterLsp') {
			staleDroppedAfterLspRequests++;
			executedLspRequests++;
			dropped.push(entry);
		} else if (action === 'cancelledBeforeLsp') {
			cancelledBeforeLspRequests++;
			dropped.push(entry);
		} else if (action === 'cancelledWhileInFlight') {
			cancelledWhileInFlightRequests++;
			executedLspRequests++;
			dropped.push(entry);
		} else {
			if (action === 'bypassedExplicit') {
				bypassedExplicitRequests++;
			} else if (action === 'bypassedMember') {
				bypassedMemberRequests++;
			} else if (action === 'bypassedRetrigger') {
				bypassedRetriggerRequests++;
			} else if (action === 'bypassedUnknown') {
				bypassedUnknownRequests++;
			}
			if (
				action === 'executed' ||
				action?.startsWith('bypassed') === true ||
				(finiteNumber(entry.lspRequestMs) ?? 0) > 0
			) {
				executedLspRequests++;
			}
			retained.push(entry);
		}
	}

	return {
		label: String(probe.label ?? ''),
		category: typeof probe.category === 'string' ? probe.category : undefined,
		line: finiteNumber(probe.line),
		receivedRequests: entries.length,
		executedLspRequests,
		coalescedBeforeLspRequests,
		staleResolvedWhileInFlightRequests,
		staleDroppedAfterLspRequests,
		cancelledBeforeLspRequests,
		cancelledWhileInFlightRequests,
		bypassedExplicitRequests,
		bypassedMemberRequests,
		bypassedRetriggerRequests,
		bypassedUnknownRequests,
		retainedSequences: sequenceValues(retained as SimulatedCompletionRequest[]),
		droppedSequences: sequenceValues(dropped as SimulatedCompletionRequest[]),
		latestPrefixLength: prefixLengths[prefixLengths.length - 1]
	};
}

function buildCompletionCoordinatorActualSummary(rows: ProbeLatencyRow[]): CompletionCoordinatorActualSummary | undefined {
	const probes = rows
		.map((row) => row.coordinatorActual)
		.filter((actual): actual is CompletionCoordinatorProbeActual => actual !== undefined);
	if (probes.length === 0) {
		return undefined;
	}
	const sum = (selector: (actual: CompletionCoordinatorProbeActual) => number): number =>
		probes.reduce((total, actual) => total + selector(actual), 0);
	return {
		probeCount: probes.length,
		receivedRequests: sum((actual) => actual.receivedRequests),
		executedLspRequests: sum((actual) => actual.executedLspRequests),
		coalescedBeforeLspRequests: sum((actual) => actual.coalescedBeforeLspRequests),
		staleResolvedWhileInFlightRequests: sum((actual) => actual.staleResolvedWhileInFlightRequests),
		staleDroppedAfterLspRequests: sum((actual) => actual.staleDroppedAfterLspRequests),
		cancelledBeforeLspRequests: sum((actual) => actual.cancelledBeforeLspRequests),
		cancelledWhileInFlightRequests: sum((actual) => actual.cancelledWhileInFlightRequests),
		bypassedExplicitRequests: sum((actual) => actual.bypassedExplicitRequests),
		bypassedMemberRequests: sum((actual) => actual.bypassedMemberRequests),
		bypassedRetriggerRequests: sum((actual) => actual.bypassedRetriggerRequests),
		bypassedUnknownRequests: sum((actual) => actual.bypassedUnknownRequests),
		probes
	};
}

function sumDefined(values: Array<number | undefined>): number | undefined {
	const defined = values.filter((value): value is number => value !== undefined);
	if (defined.length === 0) {
		return undefined;
	}
	return defined.reduce((total, value) => total + value, 0);
}

function positiveDeltaMs(end: number | undefined, start: number | undefined): number | undefined {
	if (end === undefined || start === undefined) {
		return undefined;
	}
	return roundMs(Math.max(0, end - start));
}

function completionDebugRequestId(value: Record<string, any> | undefined, field: string): string | undefined {
	const raw = value?.[field];
	return typeof raw === 'string' && raw.length > 0 ? raw : undefined;
}

function completionDebugHistory(capture: Record<string, any> | undefined): Array<Record<string, any>> {
	const uiCoverage = asRecord(capture?.uiCoverage);
	const explicitHistory = uiCoverage?.completionDebugHistory;
	const embeddedHistory = asRecord(uiCoverage?.latestCompletionDebug)?.recent;
	const history = Array.isArray(explicitHistory)
		? explicitHistory
		: Array.isArray(embeddedHistory)
			? embeddedHistory
			: [];
	return history
		.map(asRecord)
		.filter((entry): entry is Record<string, any> => entry !== undefined);
}

function selectCompletionDebugForLatest(
	capture: Record<string, any> | undefined,
	latest: Record<string, any>
): { debug?: Record<string, any>; clientDebugRequestId?: string; matchedByDebugRequestId: boolean } | undefined {
	const latestDebug = asRecord(asRecord(capture?.uiCoverage)?.latestCompletionDebug);
	const clientDebugRequestId = completionDebugRequestId(latest, 'completionDebugRequestId');
	if (clientDebugRequestId) {
		const history = completionDebugHistory(capture);
		for (let index = history.length - 1; index >= 0; index--) {
			const candidate = history[index];
			if (completionDebugRequestId(candidate, 'nsfDebugRequestId') === clientDebugRequestId) {
				return { debug: candidate, clientDebugRequestId, matchedByDebugRequestId: true };
			}
		}
		return { clientDebugRequestId, matchedByDebugRequestId: false };
	}
	return latestDebug ? { debug: latestDebug, clientDebugRequestId, matchedByDebugRequestId: false } : undefined;
}

function completionUiExecutedServerAttribution(
	capture: Record<string, any> | undefined,
	latest: Record<string, any>
): CompletionUiExecutedServerAttribution | undefined {
	const selected = selectCompletionDebugForLatest(capture, latest);
	if (!selected) {
		return undefined;
	}
	const { debug, clientDebugRequestId, matchedByDebugRequestId } = selected;
	if (!debug) {
		return {
			clientDebugRequestId,
			matchedByDebugRequestId,
			serverRequestObserved: false
		};
	}
	const requestQueueWaitMs = finiteNumber(debug.requestQueueWaitMs);
	const requestContextBuildMs = finiteNumber(debug.requestContextBuildMs);
	const handlerTotalMs = finiteNumber(debug.handlerTotalMs);
	const serverKnownLspMs = sumDefined([requestQueueWaitMs, requestContextBuildMs, handlerTotalMs]);
	const latestLspRequestMs = finiteNumber(latest.lspRequestMs);
	const latestLspRequestCompletedAtMs = finiteNumber(latest.lspRequestCompletedAtMs);
	const clientSendStartedAtUnixMs = finiteNumber(debug.clientSendStartedAtUnixMs);
	const serverReceivedAtUnixMs = finiteNumber(debug.serverReceivedAtUnixMs);
	const serverWorkerStartedAtUnixMs = finiteNumber(debug.serverWorkerStartedAtUnixMs);
	const serverResponseWriteCompletedAtUnixMs = finiteNumber(debug.serverResponseWriteCompletedAtUnixMs);
	const line = finiteNumber(debug.line);
	const character = finiteNumber(debug.character);
	return {
		clientDebugRequestId,
		nsfDebugRequestId: completionDebugRequestId(debug, 'nsfDebugRequestId'),
		matchedByDebugRequestId,
		serverRequestObserved: true,
		clientSendStartedAtUnixMs,
		serverReceivedAtUnixMs,
		serverWorkerStartedAtUnixMs,
		serverResponseWriteCompletedAtUnixMs,
		clientSendToServerReceivedMs: positiveDeltaMs(serverReceivedAtUnixMs, clientSendStartedAtUnixMs),
		serverReceivedToWorkerStartMs: positiveDeltaMs(serverWorkerStartedAtUnixMs, serverReceivedAtUnixMs),
		serverResponseWriteToClientResolveMs: positiveDeltaMs(
			latestLspRequestCompletedAtMs,
			serverResponseWriteCompletedAtUnixMs
		),
		serverDidChangeCompletedBeforeRequestCount: finiteNumber(debug.serverDidChangeCompletedBeforeRequestCount),
		serverDidChangeOverlapClientSendCount: finiteNumber(debug.serverDidChangeOverlapClientSendCount),
		serverDidChangeOverlapClientSendMs: finiteNumber(debug.serverDidChangeOverlapClientSendMs),
		serverLastDidChangeDurationMs: finiteNumber(debug.serverLastDidChangeDurationMs),
		serverLastDidChangeEndToRequestReceivedMs: finiteNumber(debug.serverLastDidChangeEndToRequestReceivedMs),
		documentFound: typeof debug.documentFound === 'boolean' ? debug.documentFound : undefined,
		line,
		character,
		clientDocumentVersion: finiteNumber(latest.documentVersion),
		clientDocumentIsDirty: typeof latest.documentIsDirty === 'boolean' ? latest.documentIsDirty : undefined,
		documentVersion: finiteNumber(debug.documentVersion),
		requestDocumentVersion: finiteNumber(debug.requestDocumentVersion),
		completionPrefix: typeof debug.completionPrefix === 'string' ? debug.completionPrefix : undefined,
		path: typeof debug.path === 'string' ? debug.path : undefined,
		itemCount: finiteNumber(debug.itemCount),
		requestQueueWaitMs,
		requestContextBuildMs,
		handlerTotalMs,
		interactiveCollectMs: finiteNumber(debug.interactiveCollectMs),
		memberBaseResolveMs: finiteNumber(debug.memberBaseResolveMs),
		memberQueryMs: finiteNumber(debug.memberQueryMs),
		itemAssemblyMs: finiteNumber(debug.itemAssemblyMs),
		responseWriteMs: finiteNumber(debug.responseWriteMs),
		serverKnownLspMs,
		clientResidualLspMs:
			latestLspRequestMs !== undefined && serverKnownLspMs !== undefined
				? roundMs(Math.max(0, latestLspRequestMs - serverKnownLspMs))
				: undefined,
		matchesLatestExecutedPosition:
			line !== undefined &&
			character !== undefined &&
			line === finiteNumber(latest.line) &&
			character === finiteNumber(latest.character)
	};
}

function versionAdvanced(from: number | undefined, to: number | undefined): boolean | undefined {
	return from !== undefined && to !== undefined ? to > from : undefined;
}

function completionUiExecutedClientAttribution(latest: Record<string, any>): CompletionUiExecutedClientAttribution {
	const documentVersionAtStart = finiteNumber(latest.documentVersion);
	const documentVersionAtNextStart = finiteNumber(latest.documentVersionAtNextStart);
	const documentVersionAtLspStart = finiteNumber(latest.documentVersionAtLspStart);
	const documentVersionAtProviderReturn = finiteNumber(latest.documentVersionAtProviderReturn);
	return {
		documentVersionAtStart,
		documentIsDirtyAtStart: typeof latest.documentIsDirty === 'boolean' ? latest.documentIsDirty : undefined,
		documentVersionAtNextStart,
		documentIsDirtyAtNextStart: typeof latest.documentIsDirtyAtNextStart === 'boolean' ? latest.documentIsDirtyAtNextStart : undefined,
		documentVersionAtLspStart,
		documentIsDirtyAtLspStart: typeof latest.documentIsDirtyAtLspStart === 'boolean' ? latest.documentIsDirtyAtLspStart : undefined,
		documentVersionAtProviderReturn,
		documentIsDirtyAtProviderReturn: typeof latest.documentIsDirtyAtProviderReturn === 'boolean' ? latest.documentIsDirtyAtProviderReturn : undefined,
		activeSameKindProviderCountAtStart: finiteNumber(latest.activeSameKindProviderCountAtStart),
		activeSameKindNextCountAtStart: finiteNumber(latest.activeSameKindNextCountAtStart),
		nextWaitMs: finiteNumber(latest.nextWaitMs),
		nextExecutionMs: finiteNumber(latest.nextExecutionMs),
		lspStartDelayMs: finiteNumber(latest.lspStartDelayMs),
		lspRequestMs: finiteNumber(latest.lspRequestMs),
		lspRequestCount: finiteNumber(latest.lspRequestCount),
		lspCompletionToProviderReturnMs: finiteNumber(latest.lspCompletionToProviderReturnMs),
		relativeNextStartedAtMs: finiteNumber(latest.relativeNextStartedAtMs),
		relativeNextCompletedAtMs: finiteNumber(latest.relativeNextCompletedAtMs),
		relativeLspRequestStartedAtMs: finiteNumber(latest.relativeLspRequestStartedAtMs),
		relativeLspRequestCompletedAtMs: finiteNumber(latest.relativeLspRequestCompletedAtMs),
		documentVersionAdvancedBeforeNext: versionAdvanced(documentVersionAtStart, documentVersionAtNextStart),
		documentVersionAdvancedBeforeLsp: versionAdvanced(documentVersionAtStart, documentVersionAtLspStart),
		documentVersionAdvancedDuringLsp: versionAdvanced(documentVersionAtLspStart, documentVersionAtProviderReturn)
	};
}

function completionUiExecutedAttribution(probe: Record<string, any>, capture: Record<string, any> | undefined): CompletionUiExecutedProbeAttribution | undefined {
	const entries = uiCoverageProviderRequestSequence(capture);
	if (entries.length === 0) {
		return undefined;
	}
	const executed = entries.filter((entry) => entry.completionCoordinatorAction === 'executed');
	if (executed.length === 0) {
		return undefined;
	}
	const latest = executed[executed.length - 1];
	const uiQueueQuiet = asRecord(asRecord(capture?.uiCoverage)?.queueQuiet);
	const queueQuietStartedAtMs = finiteNumber(uiQueueQuiet?.relativeStartedAtMs);
	const queueQuietCompletedAtMs = finiteNumber(uiQueueQuiet?.relativeCompletedAtMs);
	const latestCompletedAtMs = finiteNumber(latest.relativeCompletedAtMs);
	const waitUntilLatestExecutedCompletedMs =
		queueQuietStartedAtMs !== undefined && latestCompletedAtMs !== undefined
			? Math.max(0, latestCompletedAtMs - queueQuietStartedAtMs)
			: undefined;
	const postLatestExecutedQuietMs =
		queueQuietCompletedAtMs !== undefined && latestCompletedAtMs !== undefined
			? Math.max(0, queueQuietCompletedAtMs - latestCompletedAtMs)
			: undefined;
	const latestExecutedServerAttribution = completionUiExecutedServerAttribution(capture, latest);
	return {
		label: String(probe.label ?? ''),
		category: typeof probe.category === 'string' ? probe.category : undefined,
		line: finiteNumber(probe.line),
		executedRequestCount: executed.length,
		coalescedBeforeLspRequests: entries.filter((entry) => entry.completionCoordinatorAction === 'coalescedBeforeLsp').length,
		staleResolvedWhileInFlightRequests: entries.filter((entry) => entry.completionCoordinatorAction === 'staleResolvedWhileInFlight').length,
		executedSequences: sequenceValues(executed as SimulatedCompletionRequest[]),
		executedLspRequestMs: executed
			.map((entry) => finiteNumber(entry.lspRequestMs))
			.filter((value): value is number => value !== undefined),
		executedTotalMs: executed
			.map((entry) => finiteNumber(entry.totalMs))
			.filter((value): value is number => value !== undefined),
		latestExecutedSequence: finiteNumber(latest.sequence),
		latestExecutedPrefixLength: finiteNumber(latest.completionCoordinatorPrefixLength) ?? finiteNumber(latest.prefixLength),
		latestExecutedRelativeStartedAtMs: finiteNumber(latest.relativeStartedAtMs),
		latestExecutedRelativeCompletedAtMs: latestCompletedAtMs,
		latestExecutedLspRequestMs: finiteNumber(latest.lspRequestMs),
		latestExecutedTotalMs: finiteNumber(latest.totalMs),
		queueQuietMs: finiteNumber(uiQueueQuiet?.durationMs),
		queueQuietStartedAtMs,
		queueQuietCompletedAtMs,
		waitUntilLatestExecutedCompletedMs,
		postLatestExecutedQuietMs,
		latestExecutedHasServerDebug: latestExecutedServerAttribution?.serverRequestObserved === true,
		latestExecutedClientAttribution: completionUiExecutedClientAttribution(latest),
		latestExecutedServerAttribution
	};
}

function buildCompletionUiExecutedAttributionSummary(rows: ProbeLatencyRow[]): CompletionUiExecutedAttributionSummary | undefined {
	const probes = rows
		.map((row) => row.uiExecutedAttribution)
		.filter((attribution): attribution is CompletionUiExecutedProbeAttribution => attribution !== undefined);
	if (probes.length === 0) {
		return undefined;
	}
	return {
		probeCount: probes.length,
		executedRequestCount: probes.reduce((sum, probe) => sum + probe.executedRequestCount, 0),
		lspRequest: stats(probes.flatMap((probe) => probe.executedLspRequestMs)),
		providerTotal: stats(probes.flatMap((probe) => probe.executedTotalMs)),
		waitUntilLatestExecutedCompleted: stats(probes.map((probe) => probe.waitUntilLatestExecutedCompletedMs)),
		postLatestExecutedQuiet: stats(probes.map((probe) => probe.postLatestExecutedQuietMs)),
		clientNextWait: stats(probes.map((probe) => probe.latestExecutedClientAttribution?.nextWaitMs)),
		clientNextExecution: stats(probes.map((probe) => probe.latestExecutedClientAttribution?.nextExecutionMs)),
		clientLspStartDelay: stats(probes.map((probe) => probe.latestExecutedClientAttribution?.lspStartDelayMs)),
		clientLspCompletionToProviderReturn: stats(probes.map((probe) => probe.latestExecutedClientAttribution?.lspCompletionToProviderReturnMs)),
		clientActiveProviderOverlapAtStart: stats(probes.map((probe) => probe.latestExecutedClientAttribution?.activeSameKindProviderCountAtStart)),
		clientActiveNextOverlapAtStart: stats(probes.map((probe) => probe.latestExecutedClientAttribution?.activeSameKindNextCountAtStart)),
		documentAdvancedBeforeNextCount: probes.filter((probe) => probe.latestExecutedClientAttribution?.documentVersionAdvancedBeforeNext === true).length,
		documentAdvancedBeforeLspCount: probes.filter((probe) => probe.latestExecutedClientAttribution?.documentVersionAdvancedBeforeLsp === true).length,
		documentAdvancedDuringLspCount: probes.filter((probe) => probe.latestExecutedClientAttribution?.documentVersionAdvancedDuringLsp === true).length,
		serverQueueWait: stats(probes.map((probe) => probe.latestExecutedServerAttribution?.requestQueueWaitMs)),
		serverContextBuild: stats(probes.map((probe) => probe.latestExecutedServerAttribution?.requestContextBuildMs)),
		serverHandler: stats(probes.map((probe) => probe.latestExecutedServerAttribution?.handlerTotalMs)),
		clientResidualLsp: stats(probes.map((probe) => probe.latestExecutedServerAttribution?.clientResidualLspMs)),
		serverDidChangeOverlapClientSend: stats(probes.map((probe) =>
			probe.latestExecutedServerAttribution?.serverDidChangeOverlapClientSendMs
		)),
		serverLastDidChangeDuration: stats(probes.map((probe) =>
			probe.latestExecutedServerAttribution?.serverLastDidChangeDurationMs
		)),
		serverLastDidChangeEndToRequestReceived: stats(probes.map((probe) =>
			probe.latestExecutedServerAttribution?.serverLastDidChangeEndToRequestReceivedMs
		)),
		latestExecutedWithServerDebugCount: probes.filter((probe) => probe.latestExecutedHasServerDebug === true).length,
		serverDebugRequestIdMatchedCount: probes.filter((probe) => probe.latestExecutedServerAttribution?.matchedByDebugRequestId === true).length,
		serverDebugRequestIdUnmatchedCount: probes.filter((probe) =>
			typeof probe.latestExecutedServerAttribution?.clientDebugRequestId === 'string' &&
			probe.latestExecutedServerAttribution?.matchedByDebugRequestId === false
		).length,
		serverDebugRequestIdFallbackCount: probes.filter((probe) =>
			probe.latestExecutedServerAttribution?.clientDebugRequestId === undefined &&
			probe.latestExecutedServerAttribution?.matchedByDebugRequestId === false
		).length,
		probes
	};
}

function providerTiming(capture: Record<string, any> | undefined, kind: 'completion' | 'signatureHelp'): Record<string, any> | undefined {
	const verificationTiming = asRecord(asRecord(capture?.providerVerification)?.clientProviderTiming);
	if (verificationTiming) {
		return verificationTiming;
	}
	const afterProvider = asRecord(asRecord(capture?.requestCounters)?.afterProvider);
	const key = kind === 'completion' ? 'completionLastProviderTiming' : 'signatureHelpLastProviderTiming';
	return asRecord(afterProvider?.[key]);
}

function completionRow(probe: Record<string, any>): ProbeLatencyRow | undefined {
	const capture = asRecord(probe.completionCapture);
	if (!capture) {
		return undefined;
	}
	const counters = asRecord(capture.requestCounters);
	const uiCoverage = asRecord(capture.uiCoverage);
	const providerVerification = asRecord(capture.providerVerification);
	const verificationCounters = asRecord(providerVerification?.requestCounters);
	const provider = providerTiming(capture, 'completion');
	const nativeDelta = asRecord(counters?.nativeTriggerDelta);
	const uiDelta = asRecord(counters?.uiTriggerDelta);
	const providerDelta = asRecord(verificationCounters?.delta) ?? asRecord(counters?.providerDelta);
	const totalDelta = asRecord(counters?.totalDelta);
	const uiCoverageDelta = asRecord(asRecord(uiCoverage?.requestCounters)?.totalTriggerDelta);
	const uiQueueQuiet = asRecord(uiCoverage?.queueQuiet);
	const triggerSource = completionUiCoverageTriggerSource(capture);
	const nativeTriggerRequests = requestCount(nativeDelta, 'completionRequests');
	const uiTriggerRequests = requestCount(uiDelta, 'completionRequests');
	const uiCoverageRequests = requestCount(uiCoverageDelta, 'completionRequests');
	const providerRequests = requestCount(providerDelta, 'completionRequests');
	const separated = capture.measurementMode === 'separated-ui-provider';
	const uiQueueQuietTimedOut = Boolean(providerVerification?.uiQueueQuietTimedOut ?? uiQueueQuiet?.timedOut);
	const latestVisibleProviderReturn = latestVisibleCompletionProviderReturn(capture);
	const latestVisibleClientAttribution = latestVisibleProviderReturn
		? completionUiExecutedClientAttribution(latestVisibleProviderReturn)
		: undefined;
	const latestVisibleServerAttribution = latestVisibleProviderReturn
		? completionUiExecutedServerAttribution(capture, latestVisibleProviderReturn)
		: undefined;
	return {
		label: String(probe.label ?? ''),
		category: typeof probe.category === 'string' ? probe.category : undefined,
		line: finiteNumber(probe.line),
		uiCoverageTriggerSource: triggerSource,
		durationMs: finiteNumber(capture.durationMs),
		triggerTypingMs: finiteNumber(asRecord(probe.triggerTyping)?.durationMs),
		uiCommandMs: finiteNumber(asRecord(capture.uiTrigger)?.commandDurationMs),
		uiTotalMs: finiteNumber(asRecord(capture.uiTrigger)?.durationMs),
		executeProviderMs: finiteNumber(providerVerification?.durationMs) ?? finiteNumber(capture.executeProviderDurationMs),
		clientProviderTotalMs: finiteNumber(provider?.totalMs),
		clientLspRequestMs: finiteNumber(provider?.lspRequestMs),
		clientCode2ProtocolMs: finiteNumber(provider?.code2ProtocolMs),
		clientProtocol2CodeMs: finiteNumber(provider?.protocol2CodeMs),
		directServerMs: finiteNumber(asRecord(capture.directServerCompletion)?.durationMs),
		nativeTriggerRequests,
		uiTriggerRequests,
		explicitInvokeOverlapRequests: explicitInvokeOverlapRequestCount(capture),
		uiLatestVisibleProviderReturnMs: finiteNumber(latestVisibleProviderReturn?.relativeCompletedAtMs),
		uiLatestVisibleProviderExecutionMs: finiteNumber(latestVisibleProviderReturn?.totalMs),
		uiLatestVisibleLspRequestMs: finiteNumber(latestVisibleProviderReturn?.lspRequestMs),
		uiLatestVisibleNextWaitMs: latestVisibleClientAttribution?.nextWaitMs,
		uiLatestVisibleNextExecutionMs: latestVisibleClientAttribution?.nextExecutionMs,
		uiLatestVisibleLspStartDelayMs: latestVisibleClientAttribution?.lspStartDelayMs,
		uiLatestVisibleLspCompletionToProviderReturnMs:
			latestVisibleClientAttribution?.lspCompletionToProviderReturnMs,
		uiLatestVisibleActiveProviderOverlapAtStart:
			latestVisibleClientAttribution?.activeSameKindProviderCountAtStart,
		uiLatestVisibleActiveNextOverlapAtStart:
			latestVisibleClientAttribution?.activeSameKindNextCountAtStart,
		uiLatestVisibleServerHandlerMs: latestVisibleServerAttribution?.handlerTotalMs,
		uiLatestVisibleClientResidualLspMs: latestVisibleServerAttribution?.clientResidualLspMs,
		uiLatestVisibleClientToServerReceivedMs:
			latestVisibleServerAttribution?.clientSendToServerReceivedMs,
		uiLatestVisibleServerResponseToClientResolveMs:
			latestVisibleServerAttribution?.serverResponseWriteToClientResolveMs,
		uiLatestVisibleServerDidChangeOverlapMs:
			latestVisibleServerAttribution?.serverDidChangeOverlapClientSendMs,
		uiLatestVisibleServerDidChangeOverlapCount:
			latestVisibleServerAttribution?.serverDidChangeOverlapClientSendCount,
		uiLatestVisibleServerLastDidChangeDurationMs:
			latestVisibleServerAttribution?.serverLastDidChangeDurationMs,
		uiLatestVisibleServerLastDidChangeGapMs:
			latestVisibleServerAttribution?.serverLastDidChangeEndToRequestReceivedMs,
		uiLatestVisibleHasServerDebug: latestVisibleServerAttribution?.serverRequestObserved === true,
		postLatestVisibleCleanupMs: postLatestVisibleCleanupMs(capture, latestVisibleProviderReturn),
		uiCoverageRequests,
		uiRequestBurstCount: finiteNumber(uiCoverage?.requestBurstCount),
		firstProviderRequestAtMs: finiteNumber(uiCoverage?.firstProviderRequestAtMs),
		lastProviderRequestCompletedAtMs: finiteNumber(uiCoverage?.lastProviderRequestCompletedAtMs),
		uiQueueQuietMs: finiteNumber(uiQueueQuiet?.durationMs),
		uiQueueQuietTimedOut,
		providerRequests,
		totalRequests: requestCount(totalDelta, 'completionRequests'),
		duplicatedRequestPath: separated
			? uiQueueQuietTimedOut && (providerRequests ?? 0) > 0
			: (nativeTriggerRequests ?? 0) + (uiTriggerRequests ?? 0) > 0 && (providerRequests ?? 0) > 0,
		itemCount: finiteNumber(capture.itemCount),
		coalescingSimulation: completionCoalescingSimulation(probe, capture),
		coordinatorActual: completionCoordinatorActual(probe, capture),
		uiExecutedAttribution: completionUiExecutedAttribution(probe, capture)
	};
}

function signatureRow(probe: Record<string, any>): ProbeLatencyRow | undefined {
	const capture = asRecord(probe.signatureHelpCapture);
	if (!capture) {
		return undefined;
	}
	const counters = asRecord(capture.requestCounters);
	const uiCoverage = asRecord(capture.uiCoverage);
	const providerVerification = asRecord(capture.providerVerification);
	const verificationCounters = asRecord(providerVerification?.requestCounters);
	const provider = providerTiming(capture, 'signatureHelp');
	const nativeDelta = asRecord(counters?.nativeTriggerDelta);
	const uiDelta = asRecord(counters?.uiTriggerDelta);
	const providerDelta = asRecord(verificationCounters?.delta) ?? asRecord(counters?.providerDelta);
	const totalDelta = asRecord(counters?.totalDelta);
	const uiCoverageDelta = asRecord(asRecord(uiCoverage?.requestCounters)?.totalTriggerDelta);
	const uiQueueQuiet = asRecord(uiCoverage?.queueQuiet);
	const nativeTriggerRequests = requestCount(nativeDelta, 'signatureHelpRequests');
	const uiTriggerRequests = requestCount(uiDelta, 'signatureHelpRequests');
	const uiCoverageRequests = requestCount(uiCoverageDelta, 'signatureHelpRequests');
	const providerRequests = requestCount(providerDelta, 'signatureHelpRequests');
	const separated = capture.measurementMode === 'separated-ui-provider';
	const uiQueueQuietTimedOut = Boolean(providerVerification?.uiQueueQuietTimedOut ?? uiQueueQuiet?.timedOut);
	return {
		label: String(probe.label ?? ''),
		category: typeof probe.category === 'string' ? probe.category : undefined,
		line: finiteNumber(probe.line),
		durationMs: finiteNumber(capture.durationMs),
		triggerTypingMs: finiteNumber(asRecord(probe.triggerTyping)?.durationMs),
		uiCommandMs: finiteNumber(asRecord(capture.uiTrigger)?.commandDurationMs),
		uiTotalMs: finiteNumber(asRecord(capture.uiTrigger)?.durationMs),
		executeProviderMs: finiteNumber(providerVerification?.durationMs) ?? finiteNumber(capture.executeProviderDurationMs),
		clientProviderTotalMs: finiteNumber(provider?.totalMs),
		clientLspRequestMs: finiteNumber(provider?.lspRequestMs),
		clientCode2ProtocolMs: finiteNumber(provider?.code2ProtocolMs),
		clientProtocol2CodeMs: finiteNumber(provider?.protocol2CodeMs),
		nativeTriggerRequests,
		uiTriggerRequests,
		uiCoverageRequests,
		uiRequestBurstCount: finiteNumber(uiCoverage?.requestBurstCount),
		firstProviderRequestAtMs: finiteNumber(uiCoverage?.firstProviderRequestAtMs),
		lastProviderRequestCompletedAtMs: finiteNumber(uiCoverage?.lastProviderRequestCompletedAtMs),
		uiQueueQuietMs: finiteNumber(uiQueueQuiet?.durationMs),
		uiQueueQuietTimedOut,
		providerRequests,
		totalRequests: requestCount(totalDelta, 'signatureHelpRequests'),
		duplicatedRequestPath: separated
			? uiQueueQuietTimedOut && (providerRequests ?? 0) > 0
			: (nativeTriggerRequests ?? 0) + (uiTriggerRequests ?? 0) > 0 && (providerRequests ?? 0) > 0,
		signatureCount: finiteNumber(capture.signatureCount)
	};
}

function collectTypingProbes(report: unknown): Array<Record<string, any>> {
	const root = asRecord(report);
	const steps = Array.isArray(root?.steps) ? root.steps : [];
	const probes: Array<Record<string, any>> = [];
	for (const step of steps) {
		const fullDocumentTyping = asRecord(asRecord(step)?.fullDocumentTyping);
		const stepProbes = Array.isArray(fullDocumentTyping?.probes) ? fullDocumentTyping.probes : [];
		for (const probe of stepProbes) {
			const record = asRecord(probe);
			if (record) {
				probes.push(record);
			}
		}
	}
	return probes;
}

function slowestRows(rows: ProbeLatencyRow[]): ProbeLatencyRow[] {
	return [...rows]
		.sort((a, b) => (b.durationMs ?? -1) - (a.durationMs ?? -1))
		.slice(0, 10);
}

function buildCompletionUiCoverageTriggerSourceSummary(
	rows: ProbeLatencyRow[]
): CompletionUiCoverageTriggerSourceSummary[] | undefined {
	if (rows.length === 0) {
		return undefined;
	}
	const grouped = new Map<CompletionUiCoverageTriggerSource, ProbeLatencyRow[]>();
	for (const row of rows) {
		const source = row.uiCoverageTriggerSource ?? 'unknown';
		const existing = grouped.get(source) ?? [];
		existing.push(row);
		grouped.set(source, existing);
	}
	return [...grouped.entries()]
		.sort(([lhs], [rhs]) => lhs.localeCompare(rhs))
		.map(([triggerSource, sourceRows]) => ({
			triggerSource,
			probeCount: sourceRows.length,
			uiQueueQuiet: stats(sourceRows.map((row) => row.uiQueueQuietMs)),
			uiLatestVisibleProviderReturn: stats(sourceRows.map((row) => row.uiLatestVisibleProviderReturnMs)),
			uiLatestVisibleLspStartDelay: stats(sourceRows.map((row) => row.uiLatestVisibleLspStartDelayMs)),
			uiLatestVisibleLspRequest: stats(sourceRows.map((row) => row.uiLatestVisibleLspRequestMs)),
			uiLatestVisibleClientResidualLsp: stats(sourceRows.map((row) => row.uiLatestVisibleClientResidualLspMs)),
			uiLatestVisibleClientToServerReceived: stats(
				sourceRows.map((row) => row.uiLatestVisibleClientToServerReceivedMs)
			),
			uiLatestVisibleServerResponseToClientResolve: stats(
				sourceRows.map((row) => row.uiLatestVisibleServerResponseToClientResolveMs)
			),
			uiLatestVisibleServerDidChangeOverlap: stats(
				sourceRows.map((row) => row.uiLatestVisibleServerDidChangeOverlapMs)
			),
			uiLatestVisibleServerLastDidChangeDuration: stats(
				sourceRows.map((row) => row.uiLatestVisibleServerLastDidChangeDurationMs)
			),
			uiLatestVisibleServerLastDidChangeGap: stats(
				sourceRows.map((row) => row.uiLatestVisibleServerLastDidChangeGapMs)
			),
			postLatestVisibleCleanup: stats(sourceRows.map((row) => row.postLatestVisibleCleanupMs)),
			uiExecutedLspRequest: stats(sourceRows.map((row) => row.uiExecutedAttribution?.latestExecutedLspRequestMs)),
			executeProvider: stats(sourceRows.map((row) => row.executeProviderMs)),
			directServer: stats(sourceRows.map((row) => row.directServerMs)),
			explicitInvokeOverlapRequests: sourceRows.reduce(
				(total, row) => total + (row.explicitInvokeOverlapRequests ?? 0),
				0
			),
			latestExecutedWithServerDebugCount: sourceRows.filter(
				(row) => row.uiExecutedAttribution?.latestExecutedHasServerDebug === true
			).length,
			slowest: slowestRows(sourceRows)
		}));
}

export function buildReplayLatencySummary(report: unknown): ReplayLatencySummary | undefined {
	const probes = collectTypingProbes(report);
	if (probes.length === 0) {
		return undefined;
	}
	const completionRows = probes
		.filter((probe) => probe.kind === 'completion')
		.map(completionRow)
		.filter((row): row is ProbeLatencyRow => row !== undefined);
	const signatureRows = probes
		.filter((probe) => probe.kind === 'signatureHelp')
		.map(signatureRow)
		.filter((row): row is ProbeLatencyRow => row !== undefined);
	const diagnosticsCount = probes.filter((probe) => probe.kind === 'diagnostics').length;
	const summary: ReplayLatencySummary = {
		probeCounts: {
			total: probes.length,
			completion: completionRows.length,
			signatureHelp: signatureRows.length,
			diagnostics: diagnosticsCount
		}
	};
	if (completionRows.length > 0) {
		summary.completion = {
			capture: stats(completionRows.map((row) => row.durationMs)),
			executeProvider: stats(completionRows.map((row) => row.executeProviderMs)),
			clientProviderTotal: stats(completionRows.map((row) => row.clientProviderTotalMs)),
			clientLspRequest: stats(completionRows.map((row) => row.clientLspRequestMs)),
			directServer: stats(completionRows.map((row) => row.directServerMs)),
			duplicatedRequestProbeCount: completionRows.filter((row) => row.duplicatedRequestPath).length,
			uiQueueQuietTimedOutProbeCount: completionRows.filter((row) => row.uiQueueQuietTimedOut).length,
			uiQueueQuiet: stats(completionRows.map((row) => row.uiQueueQuietMs)),
			uiRequestBurst: stats(completionRows.map((row) => row.uiRequestBurstCount)),
			uiLatestVisibleProviderReturn: stats(completionRows.map((row) => row.uiLatestVisibleProviderReturnMs)),
			uiLatestVisibleNextWait: stats(completionRows.map((row) => row.uiLatestVisibleNextWaitMs)),
			uiLatestVisibleNextExecution: stats(completionRows.map((row) => row.uiLatestVisibleNextExecutionMs)),
			uiLatestVisibleLspStartDelay: stats(completionRows.map((row) => row.uiLatestVisibleLspStartDelayMs)),
			uiLatestVisibleLspRequest: stats(completionRows.map((row) => row.uiLatestVisibleLspRequestMs)),
			uiLatestVisibleLspCompletionToProviderReturn: stats(
				completionRows.map((row) => row.uiLatestVisibleLspCompletionToProviderReturnMs)
			),
			uiLatestVisibleActiveProviderOverlapAtStart: stats(
				completionRows.map((row) => row.uiLatestVisibleActiveProviderOverlapAtStart)
			),
			uiLatestVisibleActiveNextOverlapAtStart: stats(
				completionRows.map((row) => row.uiLatestVisibleActiveNextOverlapAtStart)
			),
			uiLatestVisibleServerHandler: stats(completionRows.map((row) => row.uiLatestVisibleServerHandlerMs)),
			uiLatestVisibleClientResidualLsp: stats(
				completionRows.map((row) => row.uiLatestVisibleClientResidualLspMs)
			),
			uiLatestVisibleClientToServerReceived: stats(
				completionRows.map((row) => row.uiLatestVisibleClientToServerReceivedMs)
			),
			uiLatestVisibleServerResponseToClientResolve: stats(
				completionRows.map((row) => row.uiLatestVisibleServerResponseToClientResolveMs)
			),
			uiLatestVisibleServerDidChangeOverlap: stats(
				completionRows.map((row) => row.uiLatestVisibleServerDidChangeOverlapMs)
			),
			uiLatestVisibleServerLastDidChangeDuration: stats(
				completionRows.map((row) => row.uiLatestVisibleServerLastDidChangeDurationMs)
			),
			uiLatestVisibleServerLastDidChangeGap: stats(
				completionRows.map((row) => row.uiLatestVisibleServerLastDidChangeGapMs)
			),
			uiLatestVisibleWithServerDebugCount: completionRows.filter(
				(row) => row.uiLatestVisibleHasServerDebug === true
			).length,
			postLatestVisibleCleanup: stats(completionRows.map((row) => row.postLatestVisibleCleanupMs)),
			coalescingSimulation: buildCompletionCoalescingSimulationSummary(completionRows),
			coordinatorActual: buildCompletionCoordinatorActualSummary(completionRows),
			uiExecutedLspRequest: stats(completionRows.map((row) => row.uiExecutedAttribution?.latestExecutedLspRequestMs)),
			uiExecutedAttribution: buildCompletionUiExecutedAttributionSummary(completionRows),
			uiCoverageByTriggerSource: buildCompletionUiCoverageTriggerSourceSummary(completionRows),
			slowest: slowestRows(completionRows)
		};
	}
	if (signatureRows.length > 0) {
		summary.signatureHelp = {
			capture: stats(signatureRows.map((row) => row.durationMs)),
			executeProvider: stats(signatureRows.map((row) => row.executeProviderMs)),
			clientProviderTotal: stats(signatureRows.map((row) => row.clientProviderTotalMs)),
			clientLspRequest: stats(signatureRows.map((row) => row.clientLspRequestMs)),
			duplicatedRequestProbeCount: signatureRows.filter((row) => row.duplicatedRequestPath).length,
			uiQueueQuietTimedOutProbeCount: signatureRows.filter((row) => row.uiQueueQuietTimedOut).length,
			uiQueueQuiet: stats(signatureRows.map((row) => row.uiQueueQuietMs)),
			uiRequestBurst: stats(signatureRows.map((row) => row.uiRequestBurstCount)),
			slowest: slowestRows(signatureRows)
		};
	}
	return summary;
}

function withReplayLatencySummary(report: unknown): unknown {
	const root = asRecord(report);
	if (!root || Array.isArray(report)) {
		return report;
	}
	const summary = buildReplayLatencySummary(report);
	return summary ? { ...root, latencySummary: summary } : report;
}

export function writeReplayReport(reportName: string, report: unknown): string {
	const repoRoot = path.resolve(__dirname, '..', '..', '..');
	const reportDir = path.join(repoRoot, 'out', 'test', 'perf-reports', 'real-replay');
	fs.mkdirSync(reportDir, { recursive: true });
	const safeName = reportName.replace(/[^a-z0-9._-]+/gi, '_').toLowerCase();
	const reportPath = path.join(reportDir, `${safeName}.json`);
	fs.writeFileSync(reportPath, `${JSON.stringify(withReplayLatencySummary(report), null, 2)}\n`, 'utf8');
	console.log(`[real-replay] wrote ${reportPath}`);
	return reportPath;
}
