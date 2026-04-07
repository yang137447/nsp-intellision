import * as assert from 'assert';
import * as vscode from 'vscode';

import { openFixture, repoDescribe, waitFor } from './test_helpers';

type InternalStatusWithIncludeUnderline = {
	includeUnderlineRefreshCount?: number;
	includeUnderlineLastRangeCount?: number;
	includeUnderlineLastDocumentUri?: string;
	includeUnderlineLastDurationMs?: number;
	includeUnderlineTotalDurationMs?: number;
	includeUnderlineAvgDurationMs?: number;
};

repoDescribe('NSF client integration: Include Underline Metrics', () => {
	it('records include underline refresh count, range count, and last document uri', async function () {
		this.timeout(90000);

		await openFixture('module_suite.nsf');
		await vscode.commands.executeCommand('nsf._resetInternalStatus');
		const document = await openFixture('module_definition_function_call_in_assignment.nsf');

		const status = await waitFor(
			() => vscode.commands.executeCommand<InternalStatusWithIncludeUnderline>('nsf._getInternalStatus'),
			(value) =>
				(value?.includeUnderlineRefreshCount ?? 0) > 0 &&
				(value?.includeUnderlineLastRangeCount ?? 0) > 0 &&
				value?.includeUnderlineLastDocumentUri === document.uri.toString(),
			'include underline refresh metrics'
		);

		assert.ok((status?.includeUnderlineRefreshCount ?? 0) > 0);
		assert.ok((status?.includeUnderlineLastRangeCount ?? 0) > 0);
		assert.strictEqual(status?.includeUnderlineLastDocumentUri, document.uri.toString());
		assert.ok((status?.includeUnderlineLastDurationMs ?? -1) >= 0);
		assert.ok((status?.includeUnderlineTotalDurationMs ?? -1) >= 0);
		assert.ok((status?.includeUnderlineAvgDurationMs ?? -1) >= 0);
	});
});
