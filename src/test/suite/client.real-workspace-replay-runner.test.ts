import * as assert from 'assert';
import * as vscode from 'vscode';

import { repoDescribe } from './test_helpers';
import type { ReplayScript } from '../replay/real_workspace_replay_types';
import { runReplayScript } from '../replay/real_workspace_replay_runner';
import { resolveReplayAnchor } from '../replay/real_workspace_replay_targets';

repoDescribe('NSF real workspace replay runner', () => {
    it('replays a short script, samples properly, and restores documents', async function () {
        this.timeout(120000);

        const anchor = {
            workspaceFolderSuffix: 'nsp-intellision',
            relativePath: 'test_files/module_completion_current_doc.nsf',
            anchorText: 'CompletionDocHelper',
            occurrence: 1,
            characterOffset: 0
        };

        const scriptBase: ReplayScript = {
            id: 'repo-runner-smoke',
            title: 'Repo runner smoke',
            workspaceHint: 'nsp-intellision',
            targetDocument: anchor,
            intent: 'Verify replay step execution on repo fixtures.',
            tags: ['repo', 'smoke'],
            steps: [
                {
                    kind: 'openDocument',
                    label: 'open fixture',
                    target: anchor
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

        const resolvedAnchor = await resolveReplayAnchor(anchor);
        const document = await vscode.workspace.openTextDocument(resolvedAnchor.uri);
        const anchorRange = new vscode.Range(
            resolvedAnchor.position,
            resolvedAnchor.position.translate(0, anchor.anchorText.length)
        );
        const anchorOffset = document.offsetAt(resolvedAnchor.position);
        const originalText = document.getText();
        assert.strictEqual(document.getText(anchorRange), anchor.anchorText);

        const scriptWithoutCleanup = { ...scriptBase, cleanup: undefined };
        await runReplayScript(scriptWithoutCleanup);

        const mutatedDocument = await vscode.workspace.openTextDocument(resolvedAnchor.uri);
        const mutatedText = mutatedDocument.getText();
        assert.strictEqual(
            mutatedText.substring(anchorOffset, anchorOffset + anchor.anchorText.length + 3),
            `Pix${anchor.anchorText}`
        );

        const restoreEdit = new vscode.WorkspaceEdit();
        restoreEdit.replace(
            resolvedAnchor.uri,
            new vscode.Range(
                mutatedDocument.positionAt(0),
                mutatedDocument.positionAt(mutatedDocument.getText().length)
            ),
            originalText
        );
        await vscode.workspace.applyEdit(restoreEdit);

        const baselineStatus = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
        assert.ok(baselineStatus, 'expected pre-step internal status');

        const report = await runReplayScript(scriptBase);
        assert.strictEqual(report.scriptId, 'repo-runner-smoke');
        assert.strictEqual(report.steps.length, 2);
        assert.strictEqual(report.steps[1].samples.length, 3);
        report.steps[1].samples.forEach((sample) => {
            assert.strictEqual(
                sample.baselineInternalStatus?.completionRequestCount,
                baselineStatus.completionRequestCount
            );
            assert.strictEqual(
                sample.baselineInternalStatus?.signatureHelpRequestCount,
                baselineStatus.signatureHelpRequestCount
            );
        });

        const afterCleanup = await vscode.workspace.openTextDocument(resolvedAnchor.uri);
        assert.strictEqual(afterCleanup.getText(anchorRange), anchor.anchorText);
    });
});
