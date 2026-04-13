import * as assert from 'assert';

import { loadReplayScripts } from '../replay/real_workspace_replay_script_loader';
import { writeReplayReport } from '../replay/real_workspace_replay_report_writer';
import { runReplayScript } from '../replay/real_workspace_replay_runner';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const realDescribe = testMode === 'real' ? describe : describe.skip;

realDescribe('NSF real workspace replay', () => {
	const scriptFilter = (process.env.NSF_TEST_REAL_REPLAY_SCRIPT_FILTER ?? '').toLowerCase();
	const scripts = loadReplayScripts().filter((script) =>
		scriptFilter.length === 0 ? true : script.id.toLowerCase().includes(scriptFilter)
	);

	it('loads at least one replay script', () => {
		assert.ok(scripts.length > 0, 'Expected at least one real-workspace replay script.');
	});

	for (const script of scripts) {
		it(`replays ${script.id}`, async function () {
			this.timeout(300000);
			const report = await runReplayScript(script);
			const reportPath = writeReplayReport(script.id, report);
			assert.strictEqual(report.scriptId, script.id);
			assert.ok(report.steps.length > 0);
			assert.ok(reportPath.replace(/\\/g, '/').includes('/real-replay/'));
		});
	}
});
