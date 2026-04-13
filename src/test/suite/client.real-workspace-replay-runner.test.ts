import * as assert from 'assert';

import { repoDescribe } from './test_helpers';
import type { ReplayScript } from '../replay/real_workspace_replay_types';
import { runReplayScript } from '../replay/real_workspace_replay_runner';

repoDescribe('NSF real workspace replay runner', () => {
    it('replays a short script and captures sampled timeline entries', async function () {
        this.timeout(120000);

        const script: ReplayScript = {
            id: 'repo-runner-smoke',
            title: 'Repo runner smoke',
            workspaceHint: 'nsp-intellision',
            targetDocument: {
                workspaceFolderSuffix: 'nsp-intellision',
                relativePath: 'test_files/module_completion_current_doc.nsf',
                anchorText: 'CompletionDocHelper',
                occurrence: 1,
                characterOffset: 0
            },
            intent: 'Verify replay step execution on repo fixtures.',
            tags: ['repo', 'smoke'],
            steps: [
                {
                    kind: 'openDocument',
                    label: 'open fixture',
                    target: {
                        workspaceFolderSuffix: 'nsp-intellision',
                        relativePath: 'test_files/module_completion_current_doc.nsf',
                        anchorText: 'CompletionDocHelper',
                        occurrence: 1,
                        characterOffset: 0
                    }
                },
                {
                    kind: 'typeText',
                    label: 'type prefix',
                    payload: { text: 'Pix' },
                    samplingWindow: { label: 'completion-window', delaysMs: [0, 30, 80] }
                }
            ],
            cleanup: { restoreTouchedDocuments: true }
        };

        const report = await runReplayScript(script);
        assert.strictEqual(report.scriptId, 'repo-runner-smoke');
        assert.strictEqual(report.steps.length, 2);
        assert.strictEqual(report.steps[1].samples.length, 3);
    });
});
