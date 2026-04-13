import * as vscode from 'vscode';

import { detectReplayAnomalies } from './real_workspace_replay_analyzer';
import { sampleReplayWindow } from './real_workspace_replay_sampler';
import { resolveReplayAnchor } from './real_workspace_replay_targets';
import type { ReplaySampleSnapshot, ReplayScript, ReplayStep } from './real_workspace_replay_types';
import { deleteLeftForTests, typeTextForTests } from '../suite/test_helpers';

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

type ReplayStepReport = {
    stepLabel: string;
    stepKind: ReplayStep['kind'];
    actionStartedAtMs: number;
    actionEndedAtMs: number;
    documentUri?: string;
    samples: ReplaySampleSnapshot[];
    anomalies: string[];
};

export async function runReplayScript(script: ReplayScript): Promise<{ scriptId: string; steps: ReplayStepReport[] }> {
    const originalContents = new Map<string, string>();
    let activeEditor: vscode.TextEditor | undefined;
    const stepReports: ReplayStepReport[] = [];
    const recordDocumentSnapshot = (document?: vscode.TextDocument) => {
        if (!document) {
            return;
        }
        const key = document.uri.toString();
        if (!originalContents.has(key)) {
            originalContents.set(key, document.getText());
        }
    };

    try {
        for (const step of script.steps) {
            const actionStartedAtMs = Date.now();
            const delaysMs = step.samplingWindow?.delaysMs?.length ?? 0;
            let baselineInternalStatus: unknown | undefined;
            if (delaysMs > 0) {
                baselineInternalStatus = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
            }

            switch (step.kind) {
                case 'openDocument': {
                    const resolved = await resolveReplayAnchor(step.target);
                    const document = await vscode.workspace.openTextDocument(resolved.uri);
                    activeEditor = await vscode.window.showTextDocument(document, { preview: false });
                    activeEditor.selection = new vscode.Selection(resolved.position, resolved.position);
                    recordDocumentSnapshot(document);
                    break;
                }
                case 'placeCursor': {
                    const resolved = await resolveReplayAnchor(step.target);
                    const document = await vscode.workspace.openTextDocument(resolved.uri);
                    activeEditor = await vscode.window.showTextDocument(document, { preview: false });
                    activeEditor.selection = new vscode.Selection(resolved.position, resolved.position);
                    recordDocumentSnapshot(document);
                    break;
                }
                case 'selectRange': {
                    const resolved = await resolveReplayAnchor(step.target);
                    const document = await vscode.workspace.openTextDocument(resolved.uri);
                    activeEditor = await vscode.window.showTextDocument(document, { preview: false });
                    const start = resolved.position.translate(0, step.payload.startOffset);
                    const end = resolved.position.translate(0, step.payload.endOffset);
                    activeEditor.selection = new vscode.Selection(start, end);
                    recordDocumentSnapshot(document);
                    break;
                }
                case 'typeText': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] typeText requires an active editor. Step: ${step.label}`);
                    }
                    // Keep replay scripts "keystroke-like": a single step may contain multiple characters.
                    // Applying a single bulk edit here can skip VS Code's auto-triggered completion behavior.
                    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
                    for (const ch of step.payload.text) {
                        await typeTextForTests(activeEditor, ch);
                        await delay(0);
                    }
                    recordDocumentSnapshot(activeEditor.document);
                    break;
                }
                case 'deleteLeft': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] deleteLeft requires an active editor. Step: ${step.label}`);
                    }
                    const deleteCount = Math.max(1, step.payload?.count ?? 1);
                    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
                    await deleteLeftForTests(activeEditor, deleteCount);
                    recordDocumentSnapshot(activeEditor.document);
                    break;
                }
                case 'invokeCommand': {
                    await vscode.commands.executeCommand(step.payload.command, ...(step.payload.args ?? []));
                    activeEditor = vscode.window.activeTextEditor ?? activeEditor;
                    recordDocumentSnapshot(activeEditor?.document);
                    break;
                }
                default:
                    throw new Error(`Unsupported replay step kind: ${(step as ReplayStep).kind}`);
            }

            if (step.afterActionPauseMs && step.afterActionPauseMs > 0) {
                await delay(step.afterActionPauseMs);
            }

            activeEditor = vscode.window.activeTextEditor ?? activeEditor;
            const documentUri = activeEditor?.document.uri.toString();
            const samples = await sampleReplayWindow(step, documentUri, baselineInternalStatus);
            stepReports.push({
                stepLabel: step.label,
                stepKind: step.kind,
                actionStartedAtMs,
                actionEndedAtMs: Date.now(),
                documentUri,
                samples,
                anomalies: detectReplayAnomalies(step, samples)
            });
        }
    } finally {
        if (script.cleanup?.restoreTouchedDocuments) {
            for (const [uriText, originalText] of originalContents.entries()) {
                try {
                    const uri = vscode.Uri.parse(uriText);
                    const document = await vscode.workspace.openTextDocument(uri);
                    const fullRange = new vscode.Range(
                        document.positionAt(0),
                        document.positionAt(document.getText().length)
                    );
                    const edit = new vscode.WorkspaceEdit();
                    edit.replace(uri, fullRange, originalText);
                    await vscode.workspace.applyEdit(edit);
                } catch (error) {
                    console.error(`[real-replay] failed to restore ${uriText}:`, error);
                }
            }
        }
    }

    return { scriptId: script.id, steps: stepReports };
}
