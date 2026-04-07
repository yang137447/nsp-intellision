import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as assert from 'assert';
import * as vscode from 'vscode';

import {
	computeLatencyStats,
	drainMetricsWindow,
	measureLatencySamples,
	readPerfIntEnv,
	waitForNextMetricsRevision,
	writePerfReport
} from './perf_helpers';
import { getWorkspaceRoot, openFixture, waitFor, waitForClientReady, waitForIndexingIdle } from './test_helpers';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const perfDescribe = testMode === 'perf' ? describe : describe.skip;
const fileFilter = (process.env.NSF_TEST_FILE_FILTER ?? '').trim().toLowerCase();
const focusedInlayPerfDescribe = fileFilter.includes('inlay-metrics.perf') ? describe : describe.skip;

type InlayClientInternalStatus = {
	inlayProviderRequestCount?: number;
	inlayProviderRequestAvgMs?: number;
	inlayProviderRequestMaxMs?: number;
	inlayRangeAdjustSamples?: number;
	inlayRangeAdjustAvgMs?: number;
	inlayRangeAdjustMaxMs?: number;
	inlayStateCheckSamples?: number;
	inlayStateCheckAvgMs?: number;
	inlayStateCheckMaxMs?: number;
	inlayRpcRequestSamples?: number;
	inlayRpcRequestAvgMs?: number;
	inlayRpcRequestMaxMs?: number;
	inlayAssemblySamples?: number;
	inlayAssemblyAvgMs?: number;
	inlayAssemblyMaxMs?: number;
};

perfDescribe('NSF perf baseline: Inlay metrics', () => {
	it('captures deferred-hit, full-build, range-filter, and response-write metrics for medium and large docs', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		const mediumDocument = await openFixture('module_inlay_hints.nsf');
		const largeDocument = await openFixture('module_perf_large_current_doc.nsf');
		const mediumRange = new vscode.Range(new vscode.Position(0, 0), mediumDocument.positionAt(mediumDocument.getText().length));
		const largeRange = new vscode.Range(new vscode.Position(0, 0), largeDocument.positionAt(largeDocument.getText().length));

		await waitForIndexingIdle('perf inlay metrics indexing idle');
		await waitFor(
			() => vscode.commands.executeCommand<any[]>('vscode.executeInlayHintProvider', mediumDocument.uri, mediumRange),
			(value) => Array.isArray(value) && value.length > 0,
			'perf inlay metrics medium warmup'
		);
		await waitFor(
			() => vscode.commands.executeCommand<any[]>('vscode.executeInlayHintProvider', largeDocument.uri, largeRange),
			(value) => Array.isArray(value) && value.length > 0,
			'perf inlay metrics large warmup'
		);

		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'inlay client internal commands ready'
		);
		const drained = await drainMetricsWindow('perf inlay metrics');
		await vscode.commands.executeCommand('nsf._resetInternalStatus');
		const mediumRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeInlayHintProvider',
				mediumDocument.uri,
				mediumRange
			);
			assert.ok(Array.isArray(result) && result.length > 0);
			return result.length;
		});
		const largeRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeInlayHintProvider',
				largeDocument.uri,
				largeRange
			);
			assert.ok(Array.isArray(result) && result.length > 0);
			return result.length;
		});
		const clientStatus = await waitFor(
			() => vscode.commands.executeCommand<InlayClientInternalStatus>('nsf._getInternalStatus'),
			(value) =>
				(value?.inlayProviderRequestCount ?? 0) >= iterations * 2 &&
				(value?.inlayRpcRequestSamples ?? 0) >= iterations * 2 &&
				(value?.inlayAssemblySamples ?? 0) >= iterations * 2,
			'perf inlay client status'
		);

		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf inlay metrics flush'
		);
		const buildComparison = (
			status: InlayClientInternalStatus | undefined,
			serverMetrics: { payload?: any } | undefined
		) => {
			const inlayServer = serverMetrics?.payload?.inlayMetrics;
			const knownServerAvgMs =
				(inlayServer?.rangeBuildAvgMs ?? 0) +
				(inlayServer?.fullBuildAvgMs ?? 0) +
				(inlayServer?.rangeFilterAvgMs ?? 0) +
				(inlayServer?.responseWriteAvgMs ?? 0);
			return {
				providerAvgMs: status?.inlayProviderRequestAvgMs ?? 0,
				rangeAdjustAvgMs: status?.inlayRangeAdjustAvgMs ?? 0,
				stateCheckAvgMs: status?.inlayStateCheckAvgMs ?? 0,
				rpcAvgMs: status?.inlayRpcRequestAvgMs ?? 0,
				assemblyAvgMs: status?.inlayAssemblyAvgMs ?? 0,
				serverKnownAvgMs: knownServerAvgMs,
				providerMinusServerKnownAvgMs: Math.max(
					0,
					(status?.inlayProviderRequestAvgMs ?? 0) - knownServerAvgMs
				),
				rpcMinusServerMethodAvgMs: Math.max(
					0,
					(status?.inlayRpcRequestAvgMs ?? 0) - (serverMetrics?.payload?.methods?.['textDocument/inlayHint']?.avgMs ?? 0)
				)
			};
		};
		const report = {
			scenario: 'm4-inlay-metrics',
			fixtures: [
				{ id: 'PFX-2', label: 'MediumEditPath', primaryDocument: 'module_inlay_hints.nsf' },
				{ id: 'PFX-3', label: 'LargeCurrentDoc', primaryDocument: 'module_perf_large_current_doc.nsf' }
			],
			loadMode: 'Idle',
			iterations,
			wallClock: {
				inlayMedium: computeLatencyStats(mediumRun.samples),
				inlayLarge: computeLatencyStats(largeRun.samples)
			},
			clientStatus,
			metrics,
			comparison: buildComparison(clientStatus, metrics)
		};
		writePerfReport('m4-inlay-metrics', report);

		const inlayMetrics = metrics.payload?.inlayMetrics;
		assert.ok(
			typeof inlayMetrics?.deferredSnapshotHitCount === 'number',
			`Expected deferredSnapshotHitCount to be exported. Actual=${JSON.stringify(inlayMetrics)}`
		);
		assert.ok(
			typeof inlayMetrics?.deferredSnapshotMissCount === 'number',
			`Expected deferredSnapshotMissCount to be exported. Actual=${JSON.stringify(inlayMetrics)}`
		);
		assert.ok(
			((inlayMetrics?.rangeBuildSamples ?? 0) + (inlayMetrics?.rangeFilterSamples ?? 0)) >=
				iterations * 2
		);
		assert.ok((inlayMetrics?.responseWriteSamples ?? 0) >= iterations * 2);
		assert.ok((clientStatus?.inlayProviderRequestAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((clientStatus?.inlayRpcRequestAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((clientStatus?.inlayAssemblyAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
	});

	focusedInlayPerfDescribe('NSF perf baseline: Inlay metrics (Focused Cold Miss)', () => {
		it('captures forced cold-miss full-build metrics after invalidating a warmed inlay snapshot', async function () {
		this.timeout(240000);

		await vscode.commands.executeCommand('workbench.action.closeAllEditors');
		await openFixture('tecvan.txt');
		await waitForClientReady('perf cold inlay client ready');
		await vscode.commands.executeCommand('nsf.restartServer');
		await waitForIndexingIdle('perf cold inlay indexing idle');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'cold inlay client internal commands ready'
		);

		const mediumDocument = await openFixture('module_inlay_hints.nsf');
		const mediumRange = new vscode.Range(
			new vscode.Position(0, 0),
			mediumDocument.positionAt(mediumDocument.getText().length)
		);
		const warmResult = await waitFor(
			() =>
				vscode.commands.executeCommand<any[]>(
					'vscode.executeInlayHintProvider',
					mediumDocument.uri,
					mediumRange
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf forced cold inlay warmup'
		);
		assert.ok(Array.isArray(warmResult) && warmResult.length > 0);
		await vscode.commands.executeCommand('nsf._invalidateInlayHintsForTests', mediumDocument.uri.toString());

		const drained = await drainMetricsWindow('perf cold inlay metrics');
		await vscode.commands.executeCommand('nsf._resetInternalStatus');
		const coldRun = await measureLatencySamples(1, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeInlayHintProvider',
				mediumDocument.uri,
				mediumRange
			);
			assert.ok(Array.isArray(result) && result.length > 0);
			return result.length;
		});
			const clientStatus = await waitFor(
				() => vscode.commands.executeCommand<InlayClientInternalStatus>('nsf._getInternalStatus'),
				(value) =>
					(value?.inlayProviderRequestCount ?? 0) >= 1 &&
					(value?.inlayRpcRequestSamples ?? 0) >= 1 &&
					(value?.inlayAssemblySamples ?? 0) >= 1,
				'perf cold inlay client status'
			);
			const metrics = await waitForNextMetricsRevision(
				drained.revision ?? 0,
				'perf cold inlay metrics flush'
			);
			const buildComparison = (
				status: InlayClientInternalStatus | undefined,
				serverMetrics: { payload?: any } | undefined
			) => {
				const inlayServer = serverMetrics?.payload?.inlayMetrics;
				const knownServerAvgMs =
					(inlayServer?.rangeBuildAvgMs ?? 0) +
					(inlayServer?.rangeFilterAvgMs ?? 0) +
					(inlayServer?.responseWriteAvgMs ?? 0);
				return {
					providerAvgMs: status?.inlayProviderRequestAvgMs ?? 0,
					rangeAdjustAvgMs: status?.inlayRangeAdjustAvgMs ?? 0,
					stateCheckAvgMs: status?.inlayStateCheckAvgMs ?? 0,
					rpcAvgMs: status?.inlayRpcRequestAvgMs ?? 0,
					assemblyAvgMs: status?.inlayAssemblyAvgMs ?? 0,
					serverKnownAvgMs: knownServerAvgMs,
					providerMinusServerKnownAvgMs: Math.max(
						0,
						(status?.inlayProviderRequestAvgMs ?? 0) - knownServerAvgMs
					)
				};
			};

		const report = {
			scenario: 'm4-inlay-cold-miss',
			fixture: { id: 'PFX-2', label: 'MediumEditPath', primaryDocument: 'module_inlay_hints.nsf' },
			loadMode: 'Idle',
			wallClock: {
				inlayColdFirstRequest: computeLatencyStats(coldRun.samples)
			},
			clientStatus,
			metrics,
			comparison: buildComparison(clientStatus, metrics)
		};
		writePerfReport('m4-inlay-cold-miss', report);

		const inlayMetrics = metrics.payload?.inlayMetrics;
		assert.ok(
			((inlayMetrics?.deferredSnapshotMissCount ?? 0) + (inlayMetrics?.deferredSnapshotHitCount ?? 0)) > 0,
			`Expected at least one inlay snapshot path to be recorded. Actual=${JSON.stringify(inlayMetrics)}`
		);
		assert.ok(
			((inlayMetrics?.rangeBuildSamples ?? 0) + (inlayMetrics?.rangeFilterSamples ?? 0)) > 0,
			`Expected rangeBuild or rangeFilter samples > 0. Actual=${JSON.stringify(inlayMetrics)}`
		);
		assert.strictEqual(
			inlayMetrics?.fullBuildSamples ?? 0,
			0,
			`Expected fullBuildSamples == 0 on visible-range-first cold request. Actual=${JSON.stringify(inlayMetrics)}`
		);
		assert.ok((inlayMetrics?.responseWriteSamples ?? 0) > 0);
		assert.ok((clientStatus?.inlayProviderRequestAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((clientStatus?.inlayRpcRequestAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((clientStatus?.inlayAssemblyAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		});
	});
});
