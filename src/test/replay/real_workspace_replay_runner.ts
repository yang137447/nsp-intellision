import * as vscode from 'vscode';

import { detectReplayAnomalies } from './real_workspace_replay_analyzer';
import { sampleReplayWindow } from './real_workspace_replay_sampler';
import { resolveReplayAnchor } from './real_workspace_replay_targets';
import type { ReplaySampleSnapshot, ReplayScript, ReplayStep } from './real_workspace_replay_types';

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

    for (const step of script.steps) {
        const actionStartedAtMs = Date.now();

        switch (step.kind) {
            case 'openDocument': {
                const resolved = await resolveReplayAnchor(step.target);
                const document = await vscode.workspace.openTextDocument(resolved.uri);
                activeEditor = await vscode.window.showTextDocument(document, { preview: false });
                if (!originalContents.has(document.uri.toString())) {
                    originalContents.set(document.uri.toString(), document.getText());
                }
                break;
            }
            case 'placeCursor': {
                const resolved = await resolveReplayAnchor(step.target);
                const document = await vscode.workspace.openTextDocument(resolved.uri);
                activeEditor = await vscode.window.showTextDocument(document, { preview: false });
                activeEditor.selection = new vscode.Selection(resolved.position, resolved.position);
                if (!originalContents.has(document.uri.toString())) {
                    originalContents.set(document.uri.toString(), document.getText());
                }
                break;
            }
            case 'selectRange': {
                const resolved = await resolveReplayAnchor(step.target);
                const document = await vscode.workspace.openTextDocument(resolved.uri);
                activeEditor = await vscode.window.showTextDocument(document, { preview: false });
                const start = resolved.position.translate(0, step.payload.startOffset);
                const end = resolved.position.translate(0, step.payload.endOffset);
                activeEditor.selection = new vscode.Selection(start, end);
                if (!originalContents.has(document.uri.toString())) {
                    originalContents.set(document.uri.toString(), document.getText());
                }
                break;
            }
            case 'typeText': {
                await vscode.commands.executeCommand('type', { text: step.payload.text });
                break;
            }
            case 'deleteLeft': {
                const deleteCount = Math.max(1, step.payload?.count ?? 1);
                for (let index = 0; index < deleteCount; index++) {
                    await vscode.commands.executeCommand('deleteLeft');
                }
                break;
            }
            case 'invokeCommand': {
                await vscode.commands.executeCommand(step.payload.command, ...(step.payload.args ?? []));
                break;
            }
            default:
                throw new Error(`Unsupported replay step kind: ${(step as ReplayStep).kind}`);
        }

        if (step.afterActionPauseMs && step.afterActionPauseMs > 0) {
            await delay(step.afterActionPauseMs);
        }

        const documentUri = activeEditor?.document.uri.toString();
        const samples = await sampleReplayWindow(step, documentUri);
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

    if (script.cleanup?.restoreTouchedDocuments) {
        for (const [uriText, originalText] of originalContents.entries()) {
            const uri = vscode.Uri.parse(uriText);
            const document = await vscode.workspace.openTextDocument(uri);
            const fullRange = new vscode.Range(
                document.positionAt(0),
                document.positionAt(document.getText().length)
            );
            const edit = new vscode.WorkspaceEdit();
            edit.replace(uri, fullRange, originalText);
            await vscode.workspace.applyEdit(edit);
        }
    }

    return { scriptId: script.id, steps: stepReports };
}
