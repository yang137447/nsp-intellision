import * as assert from 'assert';
import * as vscode from 'vscode';

import { PERF_FIXTURES } from './perf_fixtures';
import {
	aggregateMetricsHistory,
	computeLatencyStats,
	drainMetricsWindow,
	measureLatencySamples,
	readPerfIntEnv,
	waitForMetricsHistorySinceRevision,
	waitForNextMetricsRevision,
	writePerfReport
} from './perf_helpers';
import {
	getCompletionItems,
	openFixture,
	positionOf,
	touchDocument,
	waitFor,
	waitForClientQuiescent,
	waitForIndexingIdle
} from './test_helpers';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const perfDescribe = testMode === 'perf' ? describe : describe.skip;

perfDescribe('NSF perf baseline: Interactive hot-path metrics', () => {
	it('captures queue wait, context build, owner didChange, and lazy snapshot metrics for current-doc completion flow', async function () {
		this.timeout(180000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		let document = await openFixture(PERF_FIXTURES.pfx1SmallCurrentDoc.primaryDocument);
		const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

		await waitForIndexingIdle('perf interactive metrics indexing idle');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => {
				const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
				return labels.has('CompletionDocHelper') && labels.has('completionLocalColor');
			},
			'perf interactive metrics completion warmup'
		);
		await waitForClientQuiescent('perf interactive metrics quiescent before drain');

		const drained = await drainMetricsWindow('perf interactive metrics current-doc flow');
		const completionRun = await measureLatencySamples(iterations, async () => {
			await touchDocument(document);
			document = await vscode.workspace.openTextDocument(document.uri);
			const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				document.uri,
				completionPosition
			);
			const labels = new Set(getCompletionItems(result).map((item) => item.label.toString()));
			assert.ok(labels.has('CompletionDocHelper'));
			assert.ok(labels.has('completionLocalColor'));
			return getCompletionItems(result).length;
		});

		const metricWindows = await waitForMetricsHistorySinceRevision(
			drained.revision ?? 0,
			(snapshots) => {
				const aggregate = aggregateMetricsHistory(snapshots);
				const interactiveRuntime = aggregate.interactiveRuntime;
				const completionMetrics = aggregate.completionMetrics;
				return (
					(interactiveRuntime.requestQueueWaitSamples ?? 0) >= iterations &&
					(interactiveRuntime.requestContextBuildSamples ?? 0) >= iterations &&
					(interactiveRuntime.ownerDidChangeSamples ?? 0) >= iterations &&
					(completionMetrics.interactiveCollectSamples ?? 0) >= iterations &&
					(completionMetrics.workspaceSummaryQuerySamples ?? 0) >= iterations &&
					(completionMetrics.itemAssemblySamples ?? 0) >= iterations
				);
			},
			'perf interactive metrics history flush'
		);
		const aggregatedMetrics = aggregateMetricsHistory(metricWindows);
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf interactive metrics latest flush'
		);
		const report = {
			scenario: 'm3-current-doc-interactive-metrics',
			fixture: PERF_FIXTURES.pfx1SmallCurrentDoc,
			loadMode: 'Idle',
			iterations,
			wallClock: {
				completionAfterTouches: computeLatencyStats(completionRun.samples)
			},
			metrics,
			metricWindows,
			aggregatedMetrics
		};
		writePerfReport('m3-current-doc-interactive-metrics', report);

		const interactiveRuntime = aggregatedMetrics.interactiveRuntime;
		const completionMetrics = aggregatedMetrics.completionMetrics;
		assert.ok((interactiveRuntime?.requestQueueWaitSamples ?? 0) >= iterations);
		assert.ok((interactiveRuntime?.requestContextBuildSamples ?? 0) >= iterations);
		assert.ok((interactiveRuntime?.ownerDidChangeSamples ?? 0) >= iterations);
		assert.strictEqual(interactiveRuntime?.prewarmSamples ?? 0, 0);
		assert.ok(
			(interactiveRuntime?.snapshotBuildSuccess ?? 0) +
				(interactiveRuntime?.lastGoodServed ?? 0) +
				(interactiveRuntime?.incrementalPromoted ?? 0) >= iterations
		);
		assert.ok((completionMetrics?.interactiveCollectSamples ?? 0) >= iterations);
		assert.ok((completionMetrics?.workspaceSummaryQuerySamples ?? 0) >= iterations);
		assert.ok((completionMetrics?.itemAssemblySamples ?? 0) >= iterations);
		assert.ok(
			(interactiveRuntime?.requestQueueWaitAvgMs ?? Number.POSITIVE_INFINITY) <= 5,
			`Expected interactive request queue wait avg <= 5ms. Actual=${interactiveRuntime?.requestQueueWaitAvgMs ?? 'n/a'}`
		);
		assert.ok(
			(interactiveRuntime?.requestContextBuildAvgMs ?? Number.POSITIVE_INFINITY) <= 5,
			`Expected interactive request-context build avg <= 5ms. Actual=${interactiveRuntime?.requestContextBuildAvgMs ?? 'n/a'}`
		);
		assert.ok(
			(interactiveRuntime?.ownerDidChangeAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
			`Expected owner didChange avg <= 10ms. Actual=${interactiveRuntime?.ownerDidChangeAvgMs ?? 'n/a'}`
		);
		assert.ok(
			(completionMetrics?.interactiveCollectAvgMs ?? Number.POSITIVE_INFINITY) <= 50,
			`Expected completion interactiveCollect avg <= 50ms. Actual=${completionMetrics?.interactiveCollectAvgMs ?? 'n/a'}`
		);
		assert.ok(
			(completionMetrics?.itemAssemblyAvgMs ?? Number.POSITIVE_INFINITY) <= 20,
			`Expected completion itemAssembly avg <= 20ms. Actual=${completionMetrics?.itemAssemblyAvgMs ?? 'n/a'}`
		);
		assert.ok(
			(completionMetrics?.responseWriteAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
			`Expected completion responseWrite avg <= 10ms. Actual=${completionMetrics?.responseWriteAvgMs ?? 'n/a'}`
		);
	});

	it('captures syntax-only didChange metrics under background deferred load', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		const spamCount = readPerfIntEnv('NSF_PERF_BG_SPAM_COUNT', 24, 1, 120);
		let document = await openFixture(PERF_FIXTURES.pfx1SmallCurrentDoc.primaryDocument);
		const backgroundDocument = await openFixture(PERF_FIXTURES.pfx3LargeCurrentDoc.primaryDocument);
		const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

		await waitForIndexingIdle('perf interactive syntax-only metrics indexing idle');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => {
				const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
				return labels.has('CompletionDocHelper') && labels.has('completionLocalColor');
			},
			'perf interactive syntax-only metrics completion warmup'
		);

		const drained = await drainMetricsWindow('perf interactive syntax-only metrics load-bg');
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

		const completionRun = await measureLatencySamples(iterations, async () => {
			const insertEdit = new vscode.WorkspaceEdit();
			insertEdit.insert(document.uri, new vscode.Position(0, 0), ' ');
			assert.ok(await vscode.workspace.applyEdit(insertEdit));
			document = await vscode.workspace.openTextDocument(document.uri);

			const deleteEdit = new vscode.WorkspaceEdit();
			deleteEdit.delete(document.uri, new vscode.Range(0, 0, 0, 1));
			assert.ok(await vscode.workspace.applyEdit(deleteEdit));
			document = await vscode.workspace.openTextDocument(document.uri);

			const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				document.uri,
				completionPosition
			);
			const labels = new Set(getCompletionItems(result).map((item) => item.label.toString()));
			assert.ok(labels.has('CompletionDocHelper'));
			assert.ok(labels.has('completionLocalColor'));
			return getCompletionItems(result).length;
		});

		const spamResult = await spamPromise;
		const metricWindows = await waitForMetricsHistorySinceRevision(
			drained.revision ?? 0,
			(snapshots) => {
				const aggregate = aggregateMetricsHistory(snapshots);
				const interactiveRuntime = aggregate.interactiveRuntime;
				const completionMetrics = aggregate.completionMetrics;
				return (
					(aggregate.methods['textDocument/inlayHint']?.count ?? 0) > 0 &&
					(interactiveRuntime.requestQueueWaitSamples ?? 0) >= iterations &&
					(interactiveRuntime.requestContextBuildSamples ?? 0) >= iterations &&
					(interactiveRuntime.ownerDidChangeSamples ?? 0) >= iterations * 2 &&
					(completionMetrics.interactiveCollectSamples ?? 0) >= iterations &&
					(completionMetrics.workspaceSummaryQuerySamples ?? 0) >= iterations &&
					(completionMetrics.itemAssemblySamples ?? 0) >= iterations
				);
			},
			'perf interactive syntax-only metrics history flush'
		);
		const aggregatedMetrics = aggregateMetricsHistory(metricWindows);
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf interactive syntax-only metrics latest flush'
		);
		const report = {
			scenario: 'm3-current-doc-syntax-only-load-bg-metrics',
			fixtures: [PERF_FIXTURES.pfx1SmallCurrentDoc, PERF_FIXTURES.pfx3LargeCurrentDoc],
			loadMode: 'Load-BG',
			iterations,
			spamCount,
			spamResult,
			wallClock: {
				completionAfterSyntaxOnlyEdits: computeLatencyStats(completionRun.samples)
			},
			metrics,
			metricWindows,
			aggregatedMetrics
		};
		writePerfReport('m3-current-doc-syntax-only-load-bg-metrics', report);

		const interactiveRuntime = aggregatedMetrics.interactiveRuntime;
		const completionMetrics = aggregatedMetrics.completionMetrics;
		assert.strictEqual(
			(spamResult?.completed ?? 0) + (spamResult?.cancelled ?? 0) + (spamResult?.failed ?? 0),
			spamCount
		);
		assert.ok((aggregatedMetrics.methods['textDocument/inlayHint']?.count ?? 0) > 0);
		assert.ok((interactiveRuntime?.requestQueueWaitSamples ?? 0) >= iterations);
		assert.ok((interactiveRuntime?.requestContextBuildSamples ?? 0) >= iterations);
		assert.ok((interactiveRuntime?.ownerDidChangeSamples ?? 0) >= iterations * 2);
		assert.strictEqual(interactiveRuntime?.prewarmSamples ?? 0, 0);
		assert.ok((completionMetrics?.interactiveCollectSamples ?? 0) >= iterations);
		assert.ok((completionMetrics?.workspaceSummaryQuerySamples ?? 0) >= iterations);
		assert.ok((completionMetrics?.itemAssemblySamples ?? 0) >= iterations);
		assert.ok(
			(completionMetrics?.itemAssemblyAvgMs ?? Number.POSITIVE_INFINITY) <= 20,
			`Expected completion itemAssembly avg <= 20ms under Load-BG syntax-only edits. Actual=${completionMetrics?.itemAssemblyAvgMs ?? 'n/a'}`
		);
		assert.ok(
			(completionMetrics?.responseWriteAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
			`Expected completion responseWrite avg <= 10ms under Load-BG syntax-only edits. Actual=${completionMetrics?.responseWriteAvgMs ?? 'n/a'}`
		);
	});
});
