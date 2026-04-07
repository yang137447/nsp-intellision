import * as assert from 'assert';
import * as vscode from 'vscode';

import { drainMetricsWindow, waitForNextMetricsRevision } from './perf_helpers';
import {
	getCompletionItems,
	openFixture,
	positionOf,
	repoDescribe,
	waitFor
} from './test_helpers';

repoDescribe('NSF client integration: Completion Prefix Filtering', () => {
	it('keeps server-side completion assembly and response small for identifier-prefix requests', async () => {
		const document = await openFixture('module_completion_current_doc.nsf');
		const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);

		const drained = await drainMetricsWindow('completion prefix filtering');
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'prefix-filtered completion items'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('CompletionDocHelper'));
		assert.ok(labels.has('CompletionDocEntry'));
		assert.ok(labels.has('CompletionDocGlobal'));
		assert.ok(labels.has('completionLocalColor'));

		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'completion prefix filtering metrics flush'
		);
		const completionMetrics = metrics.payload?.completionMetrics;
		assert.ok(
			(completionMetrics?.interactiveCollectSamples ?? 0) > 0,
			`Expected interactiveCollectSamples > 0. Actual=${JSON.stringify(completionMetrics)}`
		);
		assert.ok(
			(completionMetrics?.itemAssemblyAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
			`Expected itemAssemblyAvgMs <= 10ms for identifier-prefix completion. Actual=${completionMetrics?.itemAssemblyAvgMs ?? 'n/a'}`
		);
		assert.ok(
			(completionMetrics?.responseWriteAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
			`Expected responseWriteAvgMs <= 10ms for identifier-prefix completion. Actual=${completionMetrics?.responseWriteAvgMs ?? 'n/a'}`
		);
	});
});
