import * as assert from 'assert';
import * as vscode from 'vscode';

import { openFixture, repoDescribe, waitFor, waitForClientReady } from './test_helpers';

type InternalStatusWithCompletionTriggers = {
	completionTriggerCharacters?: string[];
};

repoDescribe('NSF client integration: Completion Trigger Characters', () => {
	it('advertises identifier characters as completion triggers', async function () {
		this.timeout(90000);

		await openFixture('module_suite.nsf');
		await waitForClientReady('client ready for completion trigger characters');
		const status = await waitFor(
			() => vscode.commands.executeCommand<InternalStatusWithCompletionTriggers>('nsf._getInternalStatus'),
			(value) => Array.isArray(value?.completionTriggerCharacters),
			'completion trigger characters'
		);

		const triggers = new Set(status?.completionTriggerCharacters ?? []);
		assert.ok(triggers.has('.'), `Expected '.' trigger. Actual=${JSON.stringify(status)}`);
		assert.ok(triggers.has('a'), `Expected identifier trigger 'a'. Actual=${JSON.stringify(status)}`);
		assert.ok(triggers.has('Z'), `Expected identifier trigger 'Z'. Actual=${JSON.stringify(status)}`);
		assert.ok(triggers.has('_'), `Expected identifier trigger '_'. Actual=${JSON.stringify(status)}`);
	});
});
