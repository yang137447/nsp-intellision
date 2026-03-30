import {
	DIAGNOSTICS_INDETERMINATE_REASON_KEYS,
	SIGNATURE_HELP_INDETERMINATE_REASON_KEYS
} from './indeterminate_reasons';
import {
	METRICS_SUMMARY_EMPTY_REASON,
	METRICS_SUMMARY_LABELS,
	METRICS_SUMMARY_PART_DELIMITER
} from './metrics_summary_labels';
import { LSP_METHOD_KEYS } from './lsp_method_keys';

export type MetricsMethodStats = {
	count?: number;
	cancelled?: number;
	failed?: number;
	avgMs?: number;
	maxMs?: number;
	p50Ms?: number;
	p95Ms?: number;
	p99Ms?: number;
};

export type MetricsDiagnostics = {
	count?: number;
	truncated?: number;
	timedOut?: number;
	heavyRulesSkipped?: number;
	indeterminateTotal?: number;
	indeterminateReasons?: Record<string, number>;
	staleDroppedBeforeBuild?: number;
	staleDroppedBeforePublish?: number;
	canceledInPending?: number;
	queueWaitAvgMs?: number;
	queueWaitMaxMs?: number;
	queueMaxPendingTotal?: number;
	queueMaxReadyTotal?: number;
	avgMs?: number;
	maxMs?: number;
};

export type MetricsDiagnosticsQueue = {
	pendingFast?: number;
	pendingFull?: number;
	pendingTotal?: number;
	readyFast?: number;
	readyFull?: number;
	readyTotal?: number;
};

export type MetricsFastAst = {
	lookups?: number;
	cacheHits?: number;
	cacheReused?: number;
	rebuilds?: number;
	functionsIndexed?: number;
};

export type MetricsIncludeGraphCache = {
	lookups?: number;
	cacheHits?: number;
	rebuilds?: number;
	invalidations?: number;
};

export type MetricsFullAst = {
	lookups?: number;
	cacheHits?: number;
	rebuilds?: number;
	functionsIndexed?: number;
	includesIndexed?: number;
	documentsCached?: number;
};

export type MetricsSignatureHelp = {
	indeterminateTotal?: number;
	indeterminateReasons?: Record<string, number>;
	overloadResolver?: {
		attempts?: number;
		resolved?: number;
		ambiguous?: number;
		noViable?: number;
		shadowMismatch?: number;
	};
};

export type MetricsInteractiveRuntime = {
	snapshotRequests?: number;
	analysisKeyHits?: number;
	snapshotBuildAttempts?: number;
	snapshotBuildSuccess?: number;
	snapshotBuildFailed?: number;
	lastGoodServed?: number;
	incrementalPromoted?: number;
	noSnapshotAvailable?: number;
	mergeCurrentDocHits?: number;
	mergeLastGoodHits?: number;
	mergeDeferredDocHits?: number;
	mergeWorkspaceSummaryHits?: number;
	mergeMisses?: number;
	analysisKeyHitRate?: number;
	snapshotReuseRate?: number;
	workspaceMergeHitRate?: number;
	snapshotWaitAvgMs?: number;
	snapshotWaitMaxMs?: number;
};

export type MetricsDeferredDocRuntime = {
	scheduled?: number;
	mergedLatestOnly?: number;
	droppedStale?: number;
	buildCount?: number;
	latestOnlyMergeRate?: number;
	queueWaitAvgMs?: number;
	queueWaitMaxMs?: number;
	buildAvgMs?: number;
	buildMaxMs?: number;
};

export type MetricsPayload = {
	methods?: Record<string, MetricsMethodStats>;
	diagnostics?: MetricsDiagnostics;
	diagnosticsQueue?: MetricsDiagnosticsQueue;
	fastAst?: MetricsFastAst;
	includeGraphCache?: MetricsIncludeGraphCache;
	fullAst?: MetricsFullAst;
	signatureHelp?: MetricsSignatureHelp;
	interactiveRuntime?: MetricsInteractiveRuntime;
	deferredDocRuntime?: MetricsDeferredDocRuntime;
};

export type LatestMetricsSnapshot = {
	summary: string;
	payload?: MetricsPayload;
	revision: number;
	receivedAtMs: number;
};

export type MetricsHistoryEntry = LatestMetricsSnapshot;

type MetricsTrackerDeps = {
	logClient: (message: string) => void;
	onSummaryChanged: () => void;
};

export type MetricsTracker = {
	handleNotification: (payload: MetricsPayload) => void;
	getLatestSummary: () => string;
	getLatestSnapshot: () => LatestMetricsSnapshot;
	getHistory: (sinceRevision?: number) => MetricsHistoryEntry[];
};

const summarizeTopReasons = (pairs: Array<[string, number | undefined]>, topN = 2): string => {
	const normalized = pairs
		.map(([name, value]) => [name, Math.trunc(value ?? 0)] as const)
		.filter(([, value]) => value > 0)
		.sort((a, b) => b[1] - a[1]);
	if (normalized.length === 0) {
		return '';
	}
	return normalized
		.slice(0, topN)
		.map(([name, value]) => `${name}:${value}`)
		.join(',');
};

const summarizeTopReasonsFromMap = (
	reasonMap: Record<string, number> | undefined,
	preferredOrder: readonly string[],
	topN = 2
): string => {
	if (!reasonMap) {
		return '';
	}
	const preferred = preferredOrder.map((name) => [name, reasonMap[name]] as [string, number | undefined]);
	const known = new Set(preferredOrder);
	for (const [name, value] of Object.entries(reasonMap)) {
		if (known.has(name)) {
			continue;
		}
		preferred.push([name, value]);
	}
	return summarizeTopReasons(preferred, topN);
};

export function createMetricsTracker(deps: MetricsTrackerDeps): MetricsTracker {
	let latestMetricsPayload: MetricsPayload | undefined;
	let latestMetricsSummary = '';
	let latestMetricsRevision = 0;
	let latestMetricsReceivedAtMs = 0;
	let latestMetricsHistory: MetricsHistoryEntry[] = [];

	return {
		handleNotification: (payload: MetricsPayload): void => {
			latestMetricsPayload = payload;
			latestMetricsRevision += 1;
			latestMetricsReceivedAtMs = Date.now();
			const inlay = payload.methods?.[LSP_METHOD_KEYS.inlayHint];
			const hover = payload.methods?.[LSP_METHOD_KEYS.hover];
			const completion = payload.methods?.[LSP_METHOD_KEYS.completion];
			const diag = payload.diagnostics;
			const signatureHelp = payload.signatureHelp;
			const diagQueue = payload.diagnosticsQueue;
			const fastAst = payload.fastAst;
			const includeGraphCache = payload.includeGraphCache;
			const fullAst = payload.fullAst;
			const interactiveRuntime = payload.interactiveRuntime;
			const deferredDocRuntime = payload.deferredDocRuntime;
			const inlayPart = inlay
				? `${METRICS_SUMMARY_LABELS.inlay} ${METRICS_SUMMARY_LABELS.avg}=${(inlay.avgMs ?? 0).toFixed(1)}ms p95=${(inlay.p95Ms ?? 0).toFixed(1)}ms ${METRICS_SUMMARY_LABELS.cancel}=${Math.trunc(inlay.cancelled ?? 0)}`
				: '';
			const hoverPart = hover
				? `${METRICS_SUMMARY_LABELS.hover} ${METRICS_SUMMARY_LABELS.avg}=${(hover.avgMs ?? 0).toFixed(1)}ms p95=${(hover.p95Ms ?? 0).toFixed(1)}ms`
				: '';
			const completionPart = completion
				? `Comp ${METRICS_SUMMARY_LABELS.avg}=${(completion.avgMs ?? 0).toFixed(1)}ms p95=${(completion.p95Ms ?? 0).toFixed(1)}ms`
				: '';
			const diagPart = diag
				? `${METRICS_SUMMARY_LABELS.diag} ${METRICS_SUMMARY_LABELS.avg}=${(diag.avgMs ?? 0).toFixed(1)}ms ${METRICS_SUMMARY_LABELS.timeout}=${Math.trunc(diag.timedOut ?? 0)} ${METRICS_SUMMARY_LABELS.stale}=${Math.trunc((diag.staleDroppedBeforeBuild ?? 0) + (diag.staleDroppedBeforePublish ?? 0))}`
				: '';
			const diagReasonTop = diag
				? summarizeTopReasonsFromMap(diag.indeterminateReasons, DIAGNOSTICS_INDETERMINATE_REASON_KEYS)
				: '';
			const signatureHelpReasonTop = signatureHelp
				? summarizeTopReasonsFromMap(
						signatureHelp.indeterminateReasons,
						SIGNATURE_HELP_INDETERMINATE_REASON_KEYS
					)
				: '';
			const overloadPart =
				(signatureHelp?.overloadResolver?.attempts ?? 0) > 0
					? `${METRICS_SUMMARY_LABELS.ovl} ${METRICS_SUMMARY_LABELS.a}=${Math.trunc(signatureHelp?.overloadResolver?.attempts ?? 0)} ${METRICS_SUMMARY_LABELS.rz}=${Math.trunc(signatureHelp?.overloadResolver?.resolved ?? 0)} ${METRICS_SUMMARY_LABELS.amb}=${Math.trunc(signatureHelp?.overloadResolver?.ambiguous ?? 0)} ${METRICS_SUMMARY_LABELS.nv}=${Math.trunc(signatureHelp?.overloadResolver?.noViable ?? 0)}`
					: '';
			const indeterminatePart =
				(diag?.indeterminateTotal ?? 0) > 0 || (signatureHelp?.indeterminateTotal ?? 0) > 0
					? `${METRICS_SUMMARY_LABELS.indet} ${METRICS_SUMMARY_LABELS.d}=${Math.trunc(diag?.indeterminateTotal ?? 0)} ${METRICS_SUMMARY_LABELS.s}=${Math.trunc(signatureHelp?.indeterminateTotal ?? 0)} ${METRICS_SUMMARY_LABELS.diagTop}=${diagReasonTop || METRICS_SUMMARY_EMPTY_REASON} ${METRICS_SUMMARY_LABELS.sigTop}=${signatureHelpReasonTop || METRICS_SUMMARY_EMPTY_REASON}`
					: '';
			const diagQueuePart = diagQueue
				? `${METRICS_SUMMARY_LABELS.dQueue} ${METRICS_SUMMARY_LABELS.p}=${Math.trunc(diagQueue.pendingTotal ?? 0)} ${METRICS_SUMMARY_LABELS.r}=${Math.trunc(diagQueue.readyTotal ?? 0)}`
				: '';
			const fastAstPart =
				fastAst && (fastAst.lookups ?? 0) > 0
					? `${METRICS_SUMMARY_LABELS.fastAst} ${METRICS_SUMMARY_LABELS.hit}=${(((fastAst.cacheHits ?? 0) * 100) / Math.max(1, fastAst.lookups ?? 0)).toFixed(0)}%`
					: '';
			const includeGraphPart =
				includeGraphCache && (includeGraphCache.lookups ?? 0) > 0
					? `${METRICS_SUMMARY_LABELS.includeGraph} ${METRICS_SUMMARY_LABELS.hit}=${(((includeGraphCache.cacheHits ?? 0) * 100) / Math.max(1, includeGraphCache.lookups ?? 0)).toFixed(0)}%`
					: '';
			const fullAstPart =
				fullAst && (fullAst.lookups ?? 0) > 0
					? `${METRICS_SUMMARY_LABELS.fullAst} ${METRICS_SUMMARY_LABELS.hit}=${(((fullAst.cacheHits ?? 0) * 100) / Math.max(1, fullAst.lookups ?? 0)).toFixed(0)}% ${METRICS_SUMMARY_LABELS.docs}=${Math.trunc(fullAst.documentsCached ?? 0)}`
					: '';
			const interactivePart = interactiveRuntime
				? `IR keyHit=${(((interactiveRuntime.analysisKeyHitRate ?? 0) * 100)).toFixed(0)}% reuse=${(((interactiveRuntime.snapshotReuseRate ?? 0) * 100)).toFixed(0)}% inc=${Math.trunc(interactiveRuntime.incrementalPromoted ?? 0)} wait=${(interactiveRuntime.snapshotWaitAvgMs ?? 0).toFixed(1)}ms`
				: '';
			const deferredPart = deferredDocRuntime
				? `DD merge=${(((deferredDocRuntime.latestOnlyMergeRate ?? 0) * 100)).toFixed(0)}% wait=${(deferredDocRuntime.queueWaitAvgMs ?? 0).toFixed(1)}ms build=${(deferredDocRuntime.buildAvgMs ?? 0).toFixed(1)}ms`
				: '';
			latestMetricsSummary = [
				inlayPart,
				hoverPart,
				completionPart,
				diagPart,
				indeterminatePart,
				overloadPart,
				diagQueuePart,
				fastAstPart,
				includeGraphPart,
				fullAstPart,
				interactivePart,
				deferredPart
			]
				.filter((item) => item.length > 0)
				.join(METRICS_SUMMARY_PART_DELIMITER);
			latestMetricsHistory.push({
				summary: latestMetricsSummary,
				payload,
				revision: latestMetricsRevision,
				receivedAtMs: latestMetricsReceivedAtMs
			});
			if (latestMetricsHistory.length > 64) {
				latestMetricsHistory = latestMetricsHistory.slice(-64);
			}
			deps.logClient(`metrics ${JSON.stringify(payload)}`);
			deps.onSummaryChanged();
		},
		getLatestSummary: () => latestMetricsSummary,
		getLatestSnapshot: () => ({
			summary: latestMetricsSummary,
			payload: latestMetricsPayload,
			revision: latestMetricsRevision,
			receivedAtMs: latestMetricsReceivedAtMs
		}),
		getHistory: (sinceRevision) =>
			latestMetricsHistory.filter((entry) => entry.revision > (sinceRevision ?? 0))
	};
}
