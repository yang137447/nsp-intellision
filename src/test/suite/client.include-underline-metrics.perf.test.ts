import * as assert from 'assert';
import * as vscode from 'vscode';

import { readPerfIntEnv, writePerfReport } from './perf_helpers';
import { openFixture, repoDescribe, touchDocument, waitFor } from './test_helpers';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const perfDescribe = testMode === 'perf' ? describe : describe.skip;

type IncludeUnderlineInternalStatus = {
	includeUnderlineRefreshCount?: number;
	includeUnderlineLastRangeCount?: number;
	includeUnderlineLastDocumentUri?: string;
	includeUnderlineLastDurationMs?: number;
	includeUnderlineMaxDurationMs?: number;
	includeUnderlineTotalDurationMs?: number;
	includeUnderlineAvgDurationMs?: number;
};

perfDescribe('NSF perf baseline: Include underline metrics', () => {
	it('captures include underline refresh counts and durations under light edit churn', async function () {
		this.timeout(180000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		await openFixture('module_suite.nsf');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'perf include underline internal commands ready'
		);
		await vscode.commands.executeCommand('nsf._resetInternalStatus');
		const document = await openFixture('module_definition_function_call_in_assignment.nsf');

		const initialStatus = await waitFor(
			() => vscode.commands.executeCommand<IncludeUnderlineInternalStatus>('nsf._getInternalStatus'),
			(value) =>
				(value?.includeUnderlineRefreshCount ?? 0) > 0 &&
				(value?.includeUnderlineLastRangeCount ?? 0) > 0 &&
				value?.includeUnderlineLastDocumentUri === document.uri.toString(),
			'perf include underline initial refresh'
		);
		let refreshCount = initialStatus?.includeUnderlineRefreshCount ?? 0;

		for (let index = 0; index < iterations; index++) {
			await touchDocument(document);
			const nextStatus = await waitFor(
				() => vscode.commands.executeCommand<IncludeUnderlineInternalStatus>('nsf._getInternalStatus'),
				(value) =>
					(value?.includeUnderlineRefreshCount ?? 0) > refreshCount &&
					value?.includeUnderlineLastDocumentUri === document.uri.toString(),
				`perf include underline refresh ${index + 1}`
			);
			refreshCount = nextStatus?.includeUnderlineRefreshCount ?? refreshCount;
		}

		const finalStatus = await vscode.commands.executeCommand<IncludeUnderlineInternalStatus>('nsf._getInternalStatus');
		const report = {
			scenario: 'm0-include-underline-metrics',
			fixture: {
				id: 'PFX-U1',
				label: 'IncludeUnderline',
				primaryDocument: 'module_definition_function_call_in_assignment.nsf'
			},
			loadMode: 'Idle',
			iterations,
			status: finalStatus
		};
		writePerfReport('m0-include-underline-metrics', report);

		assert.ok((finalStatus?.includeUnderlineRefreshCount ?? 0) >= iterations + 1);
		assert.ok((finalStatus?.includeUnderlineLastRangeCount ?? 0) > 0);
		assert.ok((finalStatus?.includeUnderlineAvgDurationMs ?? Number.POSITIVE_INFINITY) <= 20);
	});
});
