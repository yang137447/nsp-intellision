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
import { openFixture, positionOf, waitFor, waitForIndexingIdle } from './test_helpers';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const perfDescribe = testMode === 'perf' ? describe : describe.skip;

perfDescribe('NSF perf baseline: Hover metrics', () => {
	it('captures current-doc declaration, builtin doc, and response-write metrics at idle', async function () {
		this.timeout(180000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		const document = await openFixture('module_hover_docs.nsf');
		const currentDocPosition = positionOf(document, 'SuiteHoverVar', 3, 2);
		const currentDocFunctionPosition = positionOf(document, 'SuiteHoverFunc(input)', 1, 2);
		const builtinPosition = positionOf(document, 'saturate', 1, 2);

		await waitForIndexingIdle('perf hover metrics indexing idle');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					currentDocPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf current-doc hover warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					currentDocFunctionPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf current-doc function hover warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					builtinPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf builtin hover warmup'
		);

		const drained = await drainMetricsWindow('perf hover metrics');
		const currentDocRun = await measureLatencySamples(iterations, async () => {
			const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				document.uri,
				currentDocPosition
			);
			assert.ok(Array.isArray(hovers) && hovers.length > 0);
			return hovers.length;
		});
		const currentDocFunctionRun = await measureLatencySamples(iterations, async () => {
			const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				document.uri,
				currentDocFunctionPosition
			);
			assert.ok(Array.isArray(hovers) && hovers.length > 0);
			return hovers.length;
		});
		const builtinRun = await measureLatencySamples(iterations, async () => {
			const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				document.uri,
				builtinPosition
			);
			assert.ok(Array.isArray(hovers) && hovers.length > 0);
			return hovers.length;
		});

		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf hover metrics flush'
		);
		const report = {
			scenario: 'm3-hover-metrics',
			fixtures: [
				{ id: 'PFX-H1', label: 'CurrentDocHover', primaryDocument: 'module_hover_docs.nsf' },
				{ id: 'PFX-H2', label: 'BuiltinHover', primaryDocument: 'module_hover_docs.nsf' }
			],
			loadMode: 'Idle',
			iterations,
			wallClock: {
				currentDocHover: computeLatencyStats(currentDocRun.samples),
				currentDocFunctionHover: computeLatencyStats(currentDocFunctionRun.samples),
				builtinHover: computeLatencyStats(builtinRun.samples)
			},
			metrics
		};
		writePerfReport('m3-hover-metrics', report);

		const hoverMetrics = metrics.payload?.hoverMetrics;
		assert.ok((hoverMetrics?.currentDocDeclarationSamples ?? 0) >= iterations);
		assert.ok((hoverMetrics?.currentDocFunctionSamples ?? 0) >= iterations);
		assert.ok((hoverMetrics?.builtinDocSamples ?? 0) >= iterations);
		assert.ok((hoverMetrics?.responseWriteSamples ?? 0) >= iterations * 2);
		assert.ok((hoverMetrics?.requestSetupSamples ?? -1) >= 0);
		assert.ok((hoverMetrics?.markdownRenderSamples ?? -1) >= 0);
		assert.ok(
			(hoverMetrics?.currentDocDeclarationAvgMs ?? Number.POSITIVE_INFINITY) <= 20,
			`Expected hover currentDocDeclaration avg <= 20ms. Actual=${hoverMetrics?.currentDocDeclarationAvgMs ?? 'n/a'}`
		);
		assert.ok(
			(hoverMetrics?.responseWriteAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
			`Expected hover responseWrite avg <= 10ms. Actual=${hoverMetrics?.responseWriteAvgMs ?? 'n/a'}`
		);
	});

	it('captures current-doc declaration and response-write metrics under background deferred load', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		const spamCount = readPerfIntEnv('NSF_PERF_BG_SPAM_COUNT', 24, 1, 120);
		const document = await openFixture('module_hover_docs.nsf');
		const hoverPosition = positionOf(document, 'SuiteHoverVar', 3, 2);
		const functionHoverPosition = positionOf(document, 'SuiteHoverFunc(input)', 1, 2);
		const backgroundDocument = await openFixture('module_perf_large_current_doc.nsf');

		await waitForIndexingIdle('perf hover load-bg metrics indexing idle');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					functionHoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf hover function load-bg warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf hover load-bg warmup'
		);

		const drained = await drainMetricsWindow('perf hover load-bg metrics');
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
		const hoverRun = await measureLatencySamples(iterations, async () => {
			const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				document.uri,
				hoverPosition
			);
			assert.ok(Array.isArray(hovers) && hovers.length > 0);
			return hovers.length;
		});
		const functionHoverRun = await measureLatencySamples(iterations, async () => {
			const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				document.uri,
				functionHoverPosition
			);
			assert.ok(Array.isArray(hovers) && hovers.length > 0);
			return hovers.length;
		});
		const spamResult = await spamPromise;
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf hover load-bg metrics flush'
		);
		const report = {
			scenario: 'm3-hover-load-bg-metrics',
			fixtures: [
				{ id: 'PFX-H1', label: 'CurrentDocHover', primaryDocument: 'module_hover_docs.nsf' },
				{ id: 'PFX-3', label: 'LargeCurrentDoc', primaryDocument: 'module_perf_large_current_doc.nsf' }
			],
			loadMode: 'Load-BG',
			iterations,
			spamCount,
			spamResult,
			wallClock: {
				currentDocHover: computeLatencyStats(hoverRun.samples),
				currentDocFunctionHover: computeLatencyStats(functionHoverRun.samples)
			},
			metrics
		};
		writePerfReport('m3-hover-load-bg-metrics', report);

		const hoverMetrics = metrics.payload?.hoverMetrics;
		assert.strictEqual(
			(spamResult?.completed ?? 0) + (spamResult?.cancelled ?? 0) + (spamResult?.failed ?? 0),
			spamCount
		);
		assert.ok((metrics.payload?.methods?.['textDocument/inlayHint']?.count ?? 0) > 0);
		assert.ok((hoverMetrics?.currentDocDeclarationSamples ?? 0) >= iterations);
		assert.ok((hoverMetrics?.currentDocFunctionSamples ?? 0) >= iterations);
		assert.ok((hoverMetrics?.responseWriteSamples ?? 0) >= iterations);
		assert.ok((hoverMetrics?.requestSetupSamples ?? -1) >= 0);
		assert.ok((hoverMetrics?.markdownRenderSamples ?? -1) >= 0);
		assert.ok(
			(hoverMetrics?.responseWriteAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
			`Expected hover responseWrite avg <= 10ms under Load-BG. Actual=${hoverMetrics?.responseWriteAvgMs ?? 'n/a'}`
		);
	});
});
