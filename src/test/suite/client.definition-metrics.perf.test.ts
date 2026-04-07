import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	computeLatencyStats,
	drainMetricsWindow,
	measureLatencySamples,
	readPerfIntEnv,
	waitForNextMetricsRevision,
	writePerfReport
} from './perf_helpers';
import {
	type ProviderLocation,
	getWorkspaceRoot,
	openFixture,
	positionOf,
	toFsPath,
	waitFor,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from './test_helpers';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const perfDescribe = testMode === 'perf' ? describe : describe.skip;

perfDescribe('NSF perf baseline: Definition metrics', () => {
	it('captures current-doc, current-unit, and response-write metrics at idle', async function () {
		this.timeout(180000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		const currentDoc = await openFixture('module_decls.nsf');
		const currentDocPosition = positionOf(currentDoc, 'SuiteParamMatrix', 2, 2);

		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
			await waitForIndexingIdle('perf definition metrics indexing idle');
			const currentUnitDocument = await openFixture('include_context/units/multi_context_parameter_a.nsf');
			const currentUnitPosition = positionOf(currentUnitDocument, 'MultiContextParameterEntry', 1, 2);

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
				'perf current-doc definition warmup'
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
				'perf current-unit definition warmup'
			);

			const drained = await drainMetricsWindow('perf definition metrics');
			const currentDocRun = await measureLatencySamples(iterations, async () => {
				const definitions = await vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					currentDoc.uri,
					currentDocPosition
				);
				assert.ok(
					Array.isArray(definitions) &&
						definitions.some((location) => toFsPath(location) === currentDoc.uri.fsPath)
				);
				return definitions.length;
			});
			const currentUnitRun = await measureLatencySamples(iterations, async () => {
				const definitions = await vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					currentUnitDocument.uri,
					currentUnitPosition
				);
				assert.ok(
					Array.isArray(definitions) &&
						definitions.some(
							(location) => path.basename(toFsPath(location)) === 'multi_context_parameter_common.hlsl'
						)
				);
				return definitions.length;
			});

			const metrics = await waitForNextMetricsRevision(
				drained.revision ?? 0,
				'perf definition metrics flush'
			);
			const report = {
				scenario: 'm3-definition-metrics',
				fixtures: [
					{ id: 'PFX-D1', label: 'CurrentDocDefinition', primaryDocument: 'module_decls.nsf' },
					{ id: 'PFX-D2', label: 'CurrentUnitDefinition', primaryDocument: 'include_context/units/multi_context_parameter_a.nsf' }
				],
				loadMode: 'Idle',
				iterations,
				wallClock: {
					currentDocDefinition: computeLatencyStats(currentDocRun.samples),
					currentUnitDefinition: computeLatencyStats(currentUnitRun.samples)
				},
				metrics
			};
			writePerfReport('m3-definition-metrics', report);

			const definitionMetrics = metrics.payload?.definitionMetrics;
			assert.ok((definitionMetrics?.currentDocInteractiveSamples ?? 0) >= iterations);
			assert.ok((definitionMetrics?.currentUnitCallSamples ?? 0) >= iterations);
			assert.ok((definitionMetrics?.responseWriteSamples ?? 0) >= iterations * 2);
			assert.ok(
				(definitionMetrics?.currentDocInteractiveAvgMs ?? Number.POSITIVE_INFINITY) <= 20,
				`Expected definition currentDocInteractive avg <= 20ms. Actual=${definitionMetrics?.currentDocInteractiveAvgMs ?? 'n/a'}`
			);
			assert.ok(
				(definitionMetrics?.responseWriteAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
				`Expected definition responseWrite avg <= 10ms. Actual=${definitionMetrics?.responseWriteAvgMs ?? 'n/a'}`
			);
		});
	});

	it('captures current-doc, current-unit, and response-write metrics under background deferred load', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		const spamCount = readPerfIntEnv('NSF_PERF_BG_SPAM_COUNT', 24, 1, 120);
		const currentDoc = await openFixture('module_decls.nsf');
		const currentDocPosition = positionOf(currentDoc, 'SuiteParamMatrix', 2, 2);
		const backgroundDocument = await openFixture('module_perf_large_current_doc.nsf');

		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
			await waitForIndexingIdle('perf definition load-bg metrics indexing idle');
			const currentUnitDocument = await openFixture('include_context/units/multi_context_parameter_a.nsf');
			const currentUnitPosition = positionOf(currentUnitDocument, 'MultiContextParameterEntry', 1, 2);

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
				'perf definition load-bg current-doc warmup'
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
				'perf definition load-bg current-unit warmup'
			);

			const drained = await drainMetricsWindow('perf definition load-bg metrics');
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
			const currentDocRun = await measureLatencySamples(iterations, async () => {
				const definitions = await vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					currentDoc.uri,
					currentDocPosition
				);
				assert.ok(
					Array.isArray(definitions) &&
						definitions.some((location) => toFsPath(location) === currentDoc.uri.fsPath)
				);
				return definitions.length;
			});
			const currentUnitRun = await measureLatencySamples(iterations, async () => {
				const definitions = await vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					currentUnitDocument.uri,
					currentUnitPosition
				);
				assert.ok(
					Array.isArray(definitions) &&
						definitions.some(
							(location) => path.basename(toFsPath(location)) === 'multi_context_parameter_common.hlsl'
						)
				);
				return definitions.length;
			});
			const spamResult = await spamPromise;

			const metrics = await waitForNextMetricsRevision(
				drained.revision ?? 0,
				'perf definition load-bg metrics flush'
			);
			const report = {
				scenario: 'm3-definition-load-bg-metrics',
				fixtures: [
					{ id: 'PFX-D1', label: 'CurrentDocDefinition', primaryDocument: 'module_decls.nsf' },
					{ id: 'PFX-D2', label: 'CurrentUnitDefinition', primaryDocument: 'include_context/units/multi_context_parameter_a.nsf' },
					{ id: 'PFX-3', label: 'LargeCurrentDoc', primaryDocument: 'module_perf_large_current_doc.nsf' }
				],
				loadMode: 'Load-BG',
				iterations,
				spamCount,
				spamResult,
				wallClock: {
					currentDocDefinition: computeLatencyStats(currentDocRun.samples),
					currentUnitDefinition: computeLatencyStats(currentUnitRun.samples)
				},
				metrics
			};
			writePerfReport('m3-definition-load-bg-metrics', report);

			const definitionMetrics = metrics.payload?.definitionMetrics;
			assert.strictEqual(
				(spamResult?.completed ?? 0) + (spamResult?.cancelled ?? 0) + (spamResult?.failed ?? 0),
				spamCount
			);
			assert.ok((metrics.payload?.methods?.['textDocument/inlayHint']?.count ?? 0) > 0);
			assert.ok((definitionMetrics?.currentDocInteractiveSamples ?? 0) >= iterations);
			assert.ok((definitionMetrics?.currentUnitCallSamples ?? 0) >= iterations);
			assert.ok((definitionMetrics?.responseWriteSamples ?? 0) >= iterations * 2);
			assert.ok(
				(definitionMetrics?.responseWriteAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
				`Expected definition responseWrite avg <= 10ms under Load-BG. Actual=${definitionMetrics?.responseWriteAvgMs ?? 'n/a'}`
			);
		});
	});
});
