import * as assert from 'assert';
import * as vscode from 'vscode';

import { drainMetricsWindow, waitForNextMetricsRevision } from './perf_helpers';
import { openFixture, positionOf, repoDescribe, waitFor } from './test_helpers';

repoDescribe('NSF client integration: Hover Metrics', () => {
	it('exports current-doc declaration, builtin doc, and response-write timings for hover', async function () {
		this.timeout(90000);

		const currentDoc = await openFixture('module_hover_docs.nsf');
		const currentDocPosition = positionOf(currentDoc, 'SuiteHoverVar', 3, 2);
		const currentDocFunctionPosition = positionOf(currentDoc, 'SuiteHoverFunc(input)', 1, 2);
		const builtinPosition = positionOf(currentDoc, 'saturate', 1, 2);

		const drained = await drainMetricsWindow('hover metrics');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					currentDoc.uri,
					currentDocPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'current-doc hover metrics warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					currentDoc.uri,
					currentDocFunctionPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'current-doc function hover metrics warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					currentDoc.uri,
					builtinPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'builtin hover metrics warmup'
		);

		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'hover metrics flush'
		);
		const hoverMetrics = metrics.payload?.hoverMetrics;
		assert.ok(
			(hoverMetrics?.currentDocDeclarationSamples ?? 0) > 0,
			`Expected currentDocDeclarationSamples > 0. Actual=${JSON.stringify(hoverMetrics)}`
		);
		assert.ok(
			(hoverMetrics?.currentDocFunctionSamples ?? 0) > 0,
			`Expected currentDocFunctionSamples > 0. Actual=${JSON.stringify(hoverMetrics)}`
		);
		assert.ok(
			(hoverMetrics?.builtinDocSamples ?? 0) > 0,
			`Expected builtinDocSamples > 0. Actual=${JSON.stringify(hoverMetrics)}`
		);
		assert.ok(
			(hoverMetrics?.responseWriteSamples ?? 0) > 0,
			`Expected responseWriteSamples > 0. Actual=${JSON.stringify(hoverMetrics)}`
		);
		assert.strictEqual(
			typeof hoverMetrics?.requestSetupSamples,
			'number',
			`Expected requestSetupSamples to be exported. Actual=${JSON.stringify(hoverMetrics)}`
		);
		assert.ok(
			(hoverMetrics?.requestSetupSamples ?? -1) >= 0,
			`Expected requestSetupSamples >= 0. Actual=${JSON.stringify(hoverMetrics)}`
		);
		assert.strictEqual(
			typeof hoverMetrics?.markdownRenderSamples,
			'number',
			`Expected markdownRenderSamples to be exported. Actual=${JSON.stringify(hoverMetrics)}`
		);
		assert.ok(
			(hoverMetrics?.markdownRenderSamples ?? -1) >= 0,
			`Expected markdownRenderSamples >= 0. Actual=${JSON.stringify(hoverMetrics)}`
		);
		assert.strictEqual(
			typeof hoverMetrics?.includeContextSummarySamples,
			'number',
			`Expected includeContextSummarySamples to be exported. Actual=${JSON.stringify(hoverMetrics)}`
		);
		assert.ok(
			(hoverMetrics?.includeContextSummarySamples ?? -1) >= 0,
			`Expected includeContextSummarySamples >= 0. Actual=${JSON.stringify(hoverMetrics)}`
		);
	});
});
