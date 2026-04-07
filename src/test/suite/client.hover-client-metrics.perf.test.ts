import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	computeLatencyStats,
	drainMetricsWindow,
	readPerfIntEnv,
	waitForNextMetricsRevision,
	writePerfReport
} from './perf_helpers';
import {
	getWorkspaceRoot,
	openFixture,
	positionOf,
	waitFor,
	waitForClientQuiescent,
	waitForIndexingIdle
} from './test_helpers';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const perfDescribe = testMode === 'perf' ? describe : describe.skip;

type HoverClientInternalStatus = {
	hoverRequestCount?: number;
	hoverRequestAvgMs?: number;
	hoverRequestMaxMs?: number;
	hoverRpcRequestCount?: number;
	hoverRpcRequestAvgMs?: number;
	hoverRpcRequestMaxMs?: number;
	hoverCode2ProtocolSamples?: number;
	hoverCode2ProtocolAvgMs?: number;
	hoverCode2ProtocolMaxMs?: number;
	hoverProtocolRequestSamples?: number;
	hoverProtocolRequestAvgMs?: number;
	hoverProtocolRequestMaxMs?: number;
	hoverProtocol2CodeSamples?: number;
	hoverProtocol2CodeAvgMs?: number;
	hoverProtocol2CodeMaxMs?: number;
	hoverExtensionHostOverheadAvgMs?: number;
	hoverExtensionHostOverheadMaxMs?: number;
};

perfDescribe('NSF perf baseline: Hover client metrics', () => {
	it('captures client-side hover middleware timing at idle and under background load', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		const spamCount = readPerfIntEnv('NSF_PERF_BG_SPAM_COUNT', 24, 1, 120);
		await openFixture('module_suite.nsf');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'hover client internal commands ready'
		);

		const document = await openFixture('module_hover_docs.nsf');
		const hoverPosition = positionOf(document, 'SuiteHoverVar', 3, 2);

		await waitForIndexingIdle('perf hover client metrics indexing idle');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf hover client warmup'
		);
		await waitForClientQuiescent('perf hover client idle quiescent');

		const idleMetricsStart = await drainMetricsWindow('perf hover client idle metrics');
		await vscode.commands.executeCommand('nsf._resetInternalStatus');
		const idleSamples: number[] = [];
		for (let index = 0; index < iterations; index++) {
			const startedAt = process.hrtime.bigint();
			const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				document.uri,
				hoverPosition
			);
			const elapsedMs = Number(process.hrtime.bigint() - startedAt) / 1_000_000;
			assert.ok(Array.isArray(hovers) && hovers.length > 0);
			idleSamples.push(elapsedMs);
		}
		const idleStatus = await waitFor(
			() => vscode.commands.executeCommand<HoverClientInternalStatus>('nsf._getInternalStatus'),
			(value) =>
				(value?.hoverRequestCount ?? 0) >= iterations &&
				(value?.hoverRpcRequestCount ?? 0) >= iterations &&
				(value?.hoverCode2ProtocolSamples ?? 0) >= iterations &&
				(value?.hoverProtocolRequestSamples ?? 0) >= iterations &&
				(value?.hoverProtocol2CodeSamples ?? 0) >= iterations,
			'perf hover client idle status'
		);
		const idleMetrics = await waitForNextMetricsRevision(
			idleMetricsStart.revision ?? 0,
			'perf hover client idle metrics flush'
		);

		const backgroundDocument = await vscode.workspace.openTextDocument(
			path.join(getWorkspaceRoot(), 'test_files', 'module_perf_large_current_doc.nsf')
		);
		const loadMetricsStart = await drainMetricsWindow('perf hover client load metrics');
		await vscode.commands.executeCommand('nsf._resetInternalStatus');
		const spamPromise = vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
			'nsf._spamInlayRequests',
			{
				uri: backgroundDocument.uri.toString(),
				startLine: 0,
				startCharacter: 0,
				endLine: backgroundDocument.lineCount,
				endCharacter: 0,
				count: spamCount
			}
		);
		const loadSamples: number[] = [];
		for (let index = 0; index < iterations; index++) {
			const startedAt = process.hrtime.bigint();
			const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				document.uri,
				hoverPosition
			);
			const elapsedMs = Number(process.hrtime.bigint() - startedAt) / 1_000_000;
			assert.ok(Array.isArray(hovers) && hovers.length > 0);
			loadSamples.push(elapsedMs);
		}
		const spamResult = await spamPromise;
		const loadStatus = await waitFor(
			() => vscode.commands.executeCommand<HoverClientInternalStatus>('nsf._getInternalStatus'),
			(value) =>
				(value?.hoverRequestCount ?? 0) >= iterations &&
				(value?.hoverRpcRequestCount ?? 0) >= iterations &&
				(value?.hoverCode2ProtocolSamples ?? 0) >= iterations &&
				(value?.hoverProtocolRequestSamples ?? 0) >= iterations &&
				(value?.hoverProtocol2CodeSamples ?? 0) >= iterations,
			'perf hover client load status'
		);
		const loadMetrics = await waitForNextMetricsRevision(
			loadMetricsStart.revision ?? 0,
			'perf hover client load metrics flush'
		);

		const buildComparison = (
			status: HoverClientInternalStatus | undefined,
			metrics: { payload?: any } | undefined
		) => {
			const serverHoverAvgMs = metrics?.payload?.methods?.['textDocument/hover']?.avgMs ?? 0;
			const hoverMetrics = metrics?.payload?.hoverMetrics;
			const middlewareAvgMs = status?.hoverRequestAvgMs ?? 0;
			const rpcAvgMs = status?.hoverRpcRequestAvgMs ?? 0;
			const protocolRequestAvgMs = status?.hoverProtocolRequestAvgMs ?? 0;
			const serverKnownAvgMs =
				(hoverMetrics?.currentDocDeclarationAvgMs ?? 0) +
				(hoverMetrics?.currentDocFunctionAvgMs ?? 0) +
				(hoverMetrics?.includeContextSummaryAvgMs ?? 0) +
				(hoverMetrics?.builtinDocAvgMs ?? 0) +
				(hoverMetrics?.responseWriteAvgMs ?? 0);
			return {
				middlewareAvgMs,
				rpcAvgMs,
				code2ProtocolAvgMs: status?.hoverCode2ProtocolAvgMs ?? 0,
				protocolRequestAvgMs,
				protocol2CodeAvgMs: status?.hoverProtocol2CodeAvgMs ?? 0,
				extensionHostOverheadAvgMs: status?.hoverExtensionHostOverheadAvgMs ?? 0,
				serverHoverAvgMs,
				serverKnownAvgMs,
				rpcMinusServerAvgMs: Math.max(0, rpcAvgMs - serverHoverAvgMs),
				middlewareMinusServerAvgMs: Math.max(0, middlewareAvgMs - serverHoverAvgMs),
				rpcMinusProtocolRequestAvgMs: Math.max(0, rpcAvgMs - protocolRequestAvgMs),
				rpcMinusServerKnownAvgMs: Math.max(0, rpcAvgMs - serverKnownAvgMs),
				middlewareMinusServerKnownAvgMs: Math.max(0, middlewareAvgMs - serverKnownAvgMs)
			};
		};

		const report = {
			scenario: 'm3-hover-client-metrics',
			fixtures: [
				{ id: 'PFX-H1', label: 'CurrentDocHover', primaryDocument: 'module_hover_docs.nsf' },
				{ id: 'PFX-3', label: 'LargeCurrentDoc', primaryDocument: 'module_perf_large_current_doc.nsf' }
			],
			idle: {
				wallClock: computeLatencyStats(idleSamples),
				clientStatus: idleStatus,
				serverMetrics: idleMetrics,
				comparison: buildComparison(idleStatus, idleMetrics)
			},
			loadBg: {
				wallClock: computeLatencyStats(loadSamples),
				spamCount,
				spamResult,
				clientStatus: loadStatus,
				serverMetrics: loadMetrics,
				comparison: buildComparison(loadStatus, loadMetrics)
			}
		};
		writePerfReport('m3-hover-client-metrics', report);

		assert.strictEqual(
			(spamResult?.completed ?? 0) + (spamResult?.cancelled ?? 0) + (spamResult?.failed ?? 0),
			spamCount
		);
		assert.ok(
			(idleStatus?.hoverRequestAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
			`Expected idle hover client middleware avg <= 10ms. Actual=${idleStatus?.hoverRequestAvgMs ?? 'n/a'}`
		);
		assert.ok((idleStatus?.hoverRequestAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((loadStatus?.hoverRequestAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((idleStatus?.hoverRpcRequestAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((loadStatus?.hoverRpcRequestAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((idleStatus?.hoverCode2ProtocolAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((loadStatus?.hoverCode2ProtocolAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((idleStatus?.hoverProtocolRequestAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((loadStatus?.hoverProtocolRequestAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((idleStatus?.hoverProtocol2CodeAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((loadStatus?.hoverProtocol2CodeAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((idleStatus?.hoverExtensionHostOverheadAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
		assert.ok((loadStatus?.hoverExtensionHostOverheadAvgMs ?? Number.POSITIVE_INFINITY) >= 0);
	});
});
