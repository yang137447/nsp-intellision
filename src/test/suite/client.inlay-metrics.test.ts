import * as assert from 'assert';
import * as vscode from 'vscode';

import { drainMetricsWindow, waitForNextMetricsRevision } from './perf_helpers';
import { openFixture, repoDescribe, waitFor } from './test_helpers';

type InternalStatusWithInlayClientMetrics = {
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

repoDescribe('NSF client integration: Inlay Metrics', () => {
	it('exports deferred-hit, range-build/range-filter, and response-write timings for inlay hints', async function () {
		this.timeout(90000);

		const document = await openFixture('module_inlay_hints.nsf');
		const range = new vscode.Range(new vscode.Position(0, 0), document.positionAt(document.getText().length));

		const drained = await drainMetricsWindow('inlay metrics');
		await waitFor(
			() =>
				vscode.commands.executeCommand<any[]>(
					'vscode.executeInlayHintProvider',
					document.uri,
					range
				),
			(value) => Array.isArray(value) && value.length > 0,
			'inlay metrics warmup'
		);

		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'inlay metrics flush'
		);
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
			((inlayMetrics?.rangeBuildSamples ?? 0) + (inlayMetrics?.rangeFilterSamples ?? 0)) > 0,
			`Expected rangeBuildSamples or rangeFilterSamples > 0. Actual=${JSON.stringify(inlayMetrics)}`
		);
		assert.ok(
			(inlayMetrics?.responseWriteSamples ?? 0) > 0,
			`Expected responseWriteSamples > 0. Actual=${JSON.stringify(inlayMetrics)}`
		);
	});

	it('records client-side inlay provider timings across range adjustment, state check, rpc, and assembly', async function () {
		this.timeout(90000);

		const document = await openFixture('module_inlay_hints.nsf');
		const range = new vscode.Range(new vscode.Position(0, 0), document.positionAt(document.getText().length));
		await vscode.commands.executeCommand('nsf._resetInternalStatus');

		await waitFor(
			() =>
				vscode.commands.executeCommand<any[]>(
					'vscode.executeInlayHintProvider',
					document.uri,
					range
				),
			(value) => Array.isArray(value) && value.length > 0,
			'inlay client metrics warmup'
		);

		const status = await waitFor(
			() => vscode.commands.executeCommand<InternalStatusWithInlayClientMetrics>('nsf._getInternalStatus'),
			(value) =>
				(value?.inlayProviderRequestCount ?? 0) > 0 &&
				(value?.inlayProviderRequestAvgMs ?? -1) >= 0 &&
				(value?.inlayProviderRequestMaxMs ?? -1) >= 0 &&
				(value?.inlayRangeAdjustSamples ?? 0) > 0 &&
				(value?.inlayRangeAdjustAvgMs ?? -1) >= 0 &&
				(value?.inlayRangeAdjustMaxMs ?? -1) >= 0 &&
				(value?.inlayStateCheckSamples ?? 0) > 0 &&
				(value?.inlayStateCheckAvgMs ?? -1) >= 0 &&
				(value?.inlayStateCheckMaxMs ?? -1) >= 0 &&
				(value?.inlayRpcRequestSamples ?? 0) > 0 &&
				(value?.inlayRpcRequestAvgMs ?? -1) >= 0 &&
				(value?.inlayRpcRequestMaxMs ?? -1) >= 0 &&
				(value?.inlayAssemblySamples ?? 0) > 0 &&
				(value?.inlayAssemblyAvgMs ?? -1) >= 0 &&
				(value?.inlayAssemblyMaxMs ?? -1) >= 0,
			'inlay client metrics'
		);

		assert.ok((status?.inlayProviderRequestCount ?? 0) > 0);
		assert.ok((status?.inlayRangeAdjustSamples ?? 0) > 0);
		assert.ok((status?.inlayStateCheckSamples ?? 0) > 0);
		assert.ok((status?.inlayRpcRequestSamples ?? 0) > 0);
		assert.ok((status?.inlayAssemblySamples ?? 0) > 0);
	});
});
