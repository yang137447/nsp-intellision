import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	drainMetricsWindow,
	type MetricsHistorySnapshot,
	waitForMetricsHistorySinceRevision
} from './perf_helpers';
import {
	type ProviderLocation,
	getWorkspaceRoot,
	openFixture,
	positionOf,
	repoDescribe,
	toFsPath,
	waitFor,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from './test_helpers';

repoDescribe('NSF client integration: Definition Metrics', () => {
	it('exports current-doc, current-unit, and response-write timings for definition requests', async function () {
		this.timeout(90000);

		const collectDefinitionMetricSamples = (
			snapshots: MetricsHistorySnapshot[]
		): {
			currentDocInteractiveSamples: number;
			currentUnitCallSamples: number;
			responseWriteSamples: number;
		} =>
			snapshots.reduce(
				(acc, snapshot) => {
					const definitionMetrics = snapshot.payload?.definitionMetrics;
					acc.currentDocInteractiveSamples += definitionMetrics?.currentDocInteractiveSamples ?? 0;
					acc.currentUnitCallSamples += definitionMetrics?.currentUnitCallSamples ?? 0;
					acc.responseWriteSamples += definitionMetrics?.responseWriteSamples ?? 0;
					return acc;
				},
				{
					currentDocInteractiveSamples: 0,
					currentUnitCallSamples: 0,
					responseWriteSamples: 0
				}
			);

		const currentDoc = await openFixture('module_decls.nsf');
		const currentDocPosition = positionOf(currentDoc, 'SuiteParamMatrix', 2, 2);

		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
			await waitForIndexingIdle('definition metrics indexing idle');
			const currentUnitDocument = await openFixture('include_context/units/multi_context_parameter_a.nsf');
			const currentUnitPosition = positionOf(currentUnitDocument, 'MultiContextParameterEntry', 1, 2);

			const drained = await drainMetricsWindow('definition metrics');
			await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						currentDoc.uri,
						currentDocPosition
					),
				(value) =>
					Array.isArray(value) &&
					value.some((location) => toFsPath(location) === currentDoc.uri.fsPath),
				'current-doc definition metrics warmup'
			);
			await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						currentUnitDocument.uri,
						currentUnitPosition
					),
				(value) =>
					Array.isArray(value) &&
					value.some(
						(location) => path.basename(toFsPath(location)) === 'multi_context_parameter_common.hlsl'
				),
				'current-unit definition metrics warmup'
			);

			const metricWindows = await waitForMetricsHistorySinceRevision(
				drained.revision ?? 0,
				(snapshots) => {
					const samples = collectDefinitionMetricSamples(snapshots);
					return (
						samples.currentDocInteractiveSamples > 0 &&
						samples.currentUnitCallSamples > 0 &&
						samples.responseWriteSamples > 0
					);
				},
				'definition metrics flush'
			);
			const definitionMetrics = collectDefinitionMetricSamples(metricWindows);
			assert.ok(
				(definitionMetrics?.currentDocInteractiveSamples ?? 0) > 0,
				`Expected currentDocInteractiveSamples > 0. Actual=${JSON.stringify(definitionMetrics)}`
			);
			assert.ok(
				(definitionMetrics?.currentUnitCallSamples ?? 0) > 0,
				`Expected currentUnitCallSamples > 0. Actual=${JSON.stringify(definitionMetrics)}`
			);
			assert.ok(
				(definitionMetrics?.responseWriteSamples ?? 0) > 0,
				`Expected responseWriteSamples > 0. Actual=${JSON.stringify(definitionMetrics)}`
			);
		});
	});
});
