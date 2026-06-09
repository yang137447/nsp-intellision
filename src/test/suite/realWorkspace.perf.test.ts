import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

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

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const perfDescribe = testMode === 'perf' ? describe : describe.skip;
type Awaitable<T> = T | Thenable<T> | PromiseLike<T>;

function getWorkspaceFolderPath(folderSuffix: string): string | undefined {
	const folder = vscode.workspace.workspaceFolders?.find((item) =>
		item.uri.fsPath.replace(/\\/g, '/').toLowerCase().endsWith(folderSuffix.toLowerCase())
	);
	return folder?.uri.fsPath;
}

async function openDocument(absolutePath: string): Promise<vscode.TextDocument> {
	const document = await vscode.workspace.openTextDocument(absolutePath);
	await vscode.window.showTextDocument(document, { preview: false });
	return document;
}

async function waitFor<T>(
	producer: () => Awaitable<T>,
	isReady: (value: T) => boolean,
	label: string
): Promise<T> {
	let lastValue: T | undefined;
	for (let attempt = 0; attempt < 120; attempt++) {
		lastValue = await Promise.resolve(producer());
		if (isReady(lastValue)) {
			return lastValue;
		}
		await new Promise((resolve) => setTimeout(resolve, 500));
	}
	throw new Error(`Timed out waiting for ${label}.`);
}

async function waitForIndexingIdle(label: string): Promise<void> {
	await waitFor(
		() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
		(value) => !value?.indexingState || value.indexingState.state === 'Idle',
		label
	);
}

function positionOf(
	document: vscode.TextDocument,
	needle: string,
	occurrence = 1,
	characterOffset = 0
): vscode.Position {
	let fromIndex = 0;
	let foundIndex = -1;
	for (let current = 0; current < occurrence; current++) {
		foundIndex = document.getText().indexOf(needle, fromIndex);
		assert.notStrictEqual(foundIndex, -1, `Unable to find occurrence ${occurrence} of '${needle}'.`);
		fromIndex = foundIndex + needle.length;
	}
	return document.positionAt(foundIndex + characterOffset);
}

perfDescribe('NSF real workspace perf', () => {
	it('captures idle and load-bg perf samples for real workspace hover and definition', async function () {
		this.timeout(300000);

		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		if (!shaderSourceRoot) {
			this.skip();
			return;
		}

		const iterations = readPerfIntEnv('NSF_PERF_REAL_ITERATIONS', 4, 1, 20);
		const spamCount = readPerfIntEnv('NSF_PERF_BG_SPAM_COUNT', 24, 1, 120);
		const buildingPath = path.join(shaderSourceRoot, 'base', 'building.nsf');
		const heavyPath = path.join(shaderSourceRoot, 'sfx', 'uber_fx_common_multilayer.nsf');

		const buildingDocument = await openDocument(buildingPath);
		const heavyDocument = await openDocument(heavyPath);
		await vscode.window.showTextDocument(buildingDocument, { preview: false });
		await waitForIndexingIdle('real workspace perf indexing idle');

		const hoverPosition = positionOf(buildingDocument, 'CalcWorldDataInstance', 1, 3);
		const definitionPosition = positionOf(buildingDocument, 'CalcWorldDataInstance', 1, 3);
		const heavyRange = new vscode.Range(
			new vscode.Position(0, 0),
			heavyDocument.positionAt(heavyDocument.getText().length)
		);

		await waitFor(
			() => vscode.commands.executeCommand<vscode.Hover[]>('vscode.executeHoverProvider', buildingDocument.uri, hoverPosition),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace perf hover warmup'
		);
		await waitFor(
			() => vscode.commands.executeCommand<any[]>('vscode.executeDefinitionProvider', buildingDocument.uri, definitionPosition),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace perf definition warmup'
		);

		const idleDrained = await drainMetricsWindow('real workspace perf idle');
		const idleHoverRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				buildingDocument.uri,
				hoverPosition
			);
			assert.ok(Array.isArray(result) && result.length > 0);
			return result.length;
		});
		const idleDefinitionRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeDefinitionProvider',
				buildingDocument.uri,
				definitionPosition
			);
			assert.ok(Array.isArray(result) && result.length > 0);
			return result.length;
		});
		const idleMetricWindows = await waitForMetricsHistorySinceRevision(
			idleDrained.revision ?? 0,
			(snapshots) => {
				const aggregate = aggregateMetricsHistory(snapshots);
				return (
					(aggregate.methods['textDocument/hover']?.count ?? 0) >= iterations &&
					(aggregate.methods['textDocument/definition']?.count ?? 0) >= iterations
				);
			},
			'real workspace perf idle metrics history'
		);
		const idleAggregatedMetrics = aggregateMetricsHistory(idleMetricWindows);
		const idleMetrics = idleMetricWindows[idleMetricWindows.length - 1];

		const loadDrained = await drainMetricsWindow('real workspace perf load-bg');
		const spamPromise = vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
			'nsf._spamInlayRequests',
			{
				uri: heavyDocument.uri.toString(),
				startLine: 0,
				startCharacter: 0,
				endLine: heavyDocument.lineCount,
				endCharacter: 0,
				count: spamCount
			}
		);
		const loadHoverRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				buildingDocument.uri,
				hoverPosition
			);
			assert.ok(Array.isArray(result) && result.length > 0);
			return result.length;
		});
		const loadDefinitionRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeDefinitionProvider',
				buildingDocument.uri,
				definitionPosition
			);
			assert.ok(Array.isArray(result) && result.length > 0);
			return result.length;
		});
		const spamResult = await spamPromise;
		const loadMetricWindows = await waitForMetricsHistorySinceRevision(
			loadDrained.revision ?? 0,
			(snapshots) => {
				const aggregate = aggregateMetricsHistory(snapshots);
				return (
					(aggregate.methods['textDocument/hover']?.count ?? 0) >= iterations &&
					(aggregate.methods['textDocument/definition']?.count ?? 0) >= iterations &&
					(aggregate.methods['textDocument/inlayHint']?.count ?? 0) > 0
				);
			},
			'real workspace perf load-bg metrics history'
		);
		const loadAggregatedMetrics = aggregateMetricsHistory(loadMetricWindows);
		const loadMetrics = loadMetricWindows[loadMetricWindows.length - 1];

		const report = {
			scenario: 'real-workspace-interactive-perf',
			fixtures: [
				{ id: 'RW-1', label: 'BuildingNSF', primaryDocument: 'base/building.nsf' },
				{ id: 'RW-2', label: 'MultilayerNSF', primaryDocument: 'sfx/uber_fx_common_multilayer.nsf' }
			],
			idle: {
				hover: computeLatencyStats(idleHoverRun.samples),
				definition: computeLatencyStats(idleDefinitionRun.samples),
				metrics: idleMetrics,
				metricWindows: idleMetricWindows,
				aggregatedMetrics: idleAggregatedMetrics
			},
			loadBg: {
				hover: computeLatencyStats(loadHoverRun.samples),
				definition: computeLatencyStats(loadDefinitionRun.samples),
				spamCount,
				spamResult,
				metrics: loadMetrics,
				metricWindows: loadMetricWindows,
				aggregatedMetrics: loadAggregatedMetrics
			}
		};
		writePerfReport('real-workspace-interactive-perf', report);

		assert.ok((idleAggregatedMetrics.methods['textDocument/hover']?.count ?? 0) >= iterations);
		assert.ok((idleAggregatedMetrics.methods['textDocument/definition']?.count ?? 0) >= iterations);
		assert.ok((loadAggregatedMetrics.methods['textDocument/hover']?.count ?? 0) >= iterations);
		assert.ok((loadAggregatedMetrics.methods['textDocument/definition']?.count ?? 0) >= iterations);
		assert.ok((loadAggregatedMetrics.methods['textDocument/inlayHint']?.count ?? 0) > 0);
		assert.strictEqual(
			(spamResult?.completed ?? 0) + (spamResult?.cancelled ?? 0) + (spamResult?.failed ?? 0),
			spamCount
		);
	});

	it('captures cold visible-range inlay samples for a real workspace document', async function () {
		this.timeout(300000);

		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		if (!shaderSourceRoot) {
			this.skip();
			return;
		}

		const heavyPath = path.join(shaderSourceRoot, 'sfx', 'uber_fx_common_multilayer.nsf');
		const heavyDocument = await openDocument(heavyPath);
		await waitForIndexingIdle('real workspace inlay perf indexing idle');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._invalidateInlayHintsForTests'),
			'real workspace inlay perf internal commands'
		);

		const visibleRange = new vscode.Range(new vscode.Position(0, 0), new vscode.Position(80, 0));
		await waitFor(
			() => vscode.commands.executeCommand<any[]>('vscode.executeInlayHintProvider', heavyDocument.uri, visibleRange),
			(value) => Array.isArray(value) && value.length > 0,
			'real workspace inlay warmup'
		);
		await vscode.commands.executeCommand('nsf._invalidateInlayHintsForTests', heavyDocument.uri.toString());

		const drained = await drainMetricsWindow('real workspace inlay cold visible');
		const coldRun = await measureLatencySamples(1, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeInlayHintProvider',
				heavyDocument.uri,
				visibleRange
			);
			assert.ok(Array.isArray(result) && result.length > 0);
			return result.length;
		});
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'real workspace inlay cold visible metrics flush'
		);
		const report = {
			scenario: 'real-workspace-inlay-cold-visible',
			fixture: { id: 'RW-2', label: 'MultilayerNSF', primaryDocument: 'sfx/uber_fx_common_multilayer.nsf' },
			wallClock: {
				inlayColdVisible: computeLatencyStats(coldRun.samples)
			},
			metrics
		};
		writePerfReport('real-workspace-inlay-cold-visible', report);

		const inlayMetrics = metrics.payload?.inlayMetrics;
		assert.ok(
			(inlayMetrics?.deferredSnapshotMissCount ?? 0) > 0,
			`Expected deferredSnapshotMissCount > 0. Actual=${JSON.stringify(inlayMetrics)}`
		);
		assert.ok(
			((inlayMetrics?.rangeBuildSamples ?? 0) + (inlayMetrics?.rangeFilterSamples ?? 0)) > 0,
			`Expected rangeBuildSamples or rangeFilterSamples > 0. Actual=${JSON.stringify(inlayMetrics)}`
		);
		assert.strictEqual(
			inlayMetrics?.fullBuildSamples ?? 0,
			0,
			`Expected fullBuildSamples == 0 on real workspace cold visible request. Actual=${JSON.stringify(inlayMetrics)}`
		);
	});
});
