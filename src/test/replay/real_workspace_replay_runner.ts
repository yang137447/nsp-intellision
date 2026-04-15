import * as vscode from 'vscode';

import { detectReplayAnomalies } from './real_workspace_replay_analyzer';
import { sampleReplayWindow } from './real_workspace_replay_sampler';
import { resolveReplayAnchor } from './real_workspace_replay_targets';
import type { ReplaySampleSnapshot, ReplayScript, ReplayStep } from './real_workspace_replay_types';
import {
    deleteLeftForTests,
    typeTextForTests,
    waitForClientQuiescent,
    waitForIndexingIdle
} from '../suite/test_helpers';

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

function completionLabelToString(label: unknown): string {
    if (typeof label === 'string') {
        return label;
    }
    const maybe = label as { label?: unknown } | null;
    if (maybe && typeof maybe.label === 'string') {
        return maybe.label;
    }
    return String(label ?? '');
}

function getCompletionItems(
    result: vscode.CompletionList | vscode.CompletionItem[] | undefined
): vscode.CompletionItem[] {
    if (!result) {
        return [];
    }
    return Array.isArray(result) ? result : result.items;
}

function getLineText(document: vscode.TextDocument, line: number): string {
    try {
        return document.lineAt(line).text;
    } catch {
        return '';
    }
}

type CompletionCaptureReport = {
    durationMs: number;
    triggerCharacter?: string;
    itemCount: number;
    topLabels: string[];
    expectedLabels?: string[];
    expectedPresent?: Record<string, boolean>;
    line?: number;
    character?: number;
    lineText?: string;
    error?: string;
    lastCompletionDebug?: unknown;
};

type SignatureHelpCaptureReport = {
    durationMs: number;
    triggerCharacter?: string;
    retrigger?: boolean;
    signatureCount: number;
    signatureLabels: string[];
    activeSignature?: number;
    activeParameter?: number;
    expectedSubstrings?: string[];
    expectedMatched?: Record<string, boolean>;
    line?: number;
    character?: number;
    lineText?: string;
    error?: string;
};

type WorkspaceSymbolCaptureReport = {
    durationMs: number;
    query: string;
    symbolCount: number;
    topNames: string[];
    expectedNames?: string[];
    expectedPresent?: Record<string, boolean>;
    error?: string;
};

type ReplayStepReport = {
    stepLabel: string;
    stepKind: ReplayStep['kind'];
    actionStartedAtMs: number;
    actionEndedAtMs: number;
    documentUri?: string;
    samples: ReplaySampleSnapshot[];
    anomalies: string[];
    completionCapture?: CompletionCaptureReport;
    signatureHelpCapture?: SignatureHelpCaptureReport;
    workspaceSymbolCapture?: WorkspaceSymbolCaptureReport;
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

            let completionCapture: CompletionCaptureReport | undefined;
            let signatureHelpCapture: SignatureHelpCaptureReport | undefined;
            let workspaceSymbolCapture: WorkspaceSymbolCaptureReport | undefined;
            let completionCapturePromise: Promise<CompletionCaptureReport> | undefined;
            let signatureHelpCapturePromise: Promise<SignatureHelpCaptureReport> | undefined;

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
                case 'captureCompletion': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] captureCompletion requires an active editor. Step: ${step.label}`);
                    }

                    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
                    const document = activeEditor.document;
                    const position = activeEditor.selection.active;
                    const lineText = getLineText(document, position.line);

                    const triggerCharacter =
                        typeof step.payload?.triggerCharacter === 'string' ? step.payload.triggerCharacter : undefined;
                    const expectedLabels = Array.isArray(step.payload?.expectedLabels)
                        ? step.payload?.expectedLabels.filter((entry): entry is string => typeof entry === 'string')
                        : undefined;
                    const maxLabels = Math.max(1, Math.min(200, step.payload?.maxLabels ?? 50));

                    const startedAt = Date.now();
                    const completionProviderPromise = Promise.resolve(
                        vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[] | undefined>(
                            'vscode.executeCompletionItemProvider',
                            document.uri,
                            position,
                            triggerCharacter
                        )
                    )
                        .then((completionResult) => ({ completionResult, endedAt: Date.now() }))
                        .catch((error) => ({
                            completionResult: undefined,
                            endedAt: Date.now(),
                            error: error instanceof Error ? error.message : String(error)
                        }));

                    // Do not await here: sample windows should run while the provider is in-flight.
                    completionCapturePromise = (async (): Promise<CompletionCaptureReport> => {
                        const finished = await completionProviderPromise;
                        const durationMs = finished.endedAt - startedAt;
                        const items = getCompletionItems(finished.completionResult);
                        const labels = items
                            .map((item) => completionLabelToString((item as unknown as { label?: unknown }).label))
                            .filter((label) => label.length > 0);
                        const topLabels = labels.slice(0, maxLabels);
                        const expectedPresent = expectedLabels
                            ? Object.fromEntries(expectedLabels.map((label) => [label, topLabels.includes(label)]))
                            : undefined;

                        let lastCompletionDebug: unknown | undefined;
                        try {
                            lastCompletionDebug = await vscode.commands.executeCommand<any>('nsf._getLastCompletionDebug');
                        } catch {
                            // ignore
                        }

                        return {
                            durationMs,
                            triggerCharacter,
                            itemCount: labels.length,
                            topLabels,
                            expectedLabels,
                            expectedPresent,
                            line: position.line,
                            character: position.character,
                            lineText,
                            error: 'error' in finished ? finished.error : undefined,
                            lastCompletionDebug
                        };
                    })();
                    break;
                }
                case 'captureSignatureHelp': {
                    if (!activeEditor) {
                        throw new Error(
                            `[real-replay] captureSignatureHelp requires an active editor. Step: ${step.label}`
                        );
                    }

                    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
                    const document = activeEditor.document;
                    const position = activeEditor.selection.active;
                    const lineText = getLineText(document, position.line);

                    const triggerCharacter =
                        typeof step.payload?.triggerCharacter === 'string' ? step.payload.triggerCharacter : undefined;
                    const retrigger = typeof step.payload?.retrigger === 'boolean' ? step.payload.retrigger : undefined;
                    const expectedSubstrings = Array.isArray(step.payload?.expectedSubstrings)
                        ? step.payload?.expectedSubstrings.filter((entry): entry is string => typeof entry === 'string')
                        : undefined;
                    const maxSignatures = Math.max(1, Math.min(50, step.payload?.maxSignatures ?? 10));

                    const startedAt = Date.now();
                    const signatureProviderPromise = Promise.resolve(
                        vscode.commands.executeCommand<vscode.SignatureHelp | undefined>(
                            'vscode.executeSignatureHelpProvider',
                            document.uri,
                            position,
                            triggerCharacter,
                            retrigger
                        )
                    )
                        .then((signatureHelp) => ({ signatureHelp, endedAt: Date.now() }))
                        .catch((error) => ({
                            signatureHelp: undefined,
                            endedAt: Date.now(),
                            error: error instanceof Error ? error.message : String(error)
                        }));

                    // Do not await here: sample windows should run while the provider is in-flight.
                    signatureHelpCapturePromise = (async (): Promise<SignatureHelpCaptureReport> => {
                        const finished = await signatureProviderPromise;
                        const durationMs = finished.endedAt - startedAt;
                        const allLabels = (finished.signatureHelp?.signatures ?? [])
                            .map((signature) => String(signature.label ?? ''))
                            .filter((label) => label.length > 0);
                        const signatureLabels = allLabels.slice(0, maxSignatures);
                        const expectedMatched = expectedSubstrings
                            ? Object.fromEntries(
                                  expectedSubstrings.map((needle) => [
                                      needle,
                                      signatureLabels.some((label) => label.includes(needle))
                                  ])
                              )
                            : undefined;

                        return {
                            durationMs,
                            triggerCharacter,
                            retrigger,
                            signatureCount: allLabels.length,
                            signatureLabels,
                            activeSignature: finished.signatureHelp?.activeSignature,
                            activeParameter: finished.signatureHelp?.activeParameter,
                            expectedSubstrings,
                            expectedMatched,
                            line: position.line,
                            character: position.character,
                            lineText,
                            error: 'error' in finished ? finished.error : undefined
                        };
                    })();
                    break;
                }
                case 'captureWorkspaceSymbols': {
                    const query = String(step.payload?.query ?? '');
                    const expectedNames = Array.isArray(step.payload?.expectedNames)
                        ? step.payload.expectedNames.filter((entry): entry is string => typeof entry === 'string')
                        : undefined;
                    const maxNames = Math.max(1, Math.min(200, step.payload?.maxNames ?? 50));

                    const startedAt = Date.now();
                    let result: vscode.SymbolInformation[] | vscode.DocumentSymbol[] | undefined;
                    let captureError: string | undefined;
                    try {
                        result = await vscode.commands.executeCommand<
                            vscode.SymbolInformation[] | vscode.DocumentSymbol[] | undefined
                        >('vscode.executeWorkspaceSymbolProvider', query);
                    } catch (error) {
                        captureError = error instanceof Error ? error.message : String(error);
                        result = undefined;
                    }
                    const durationMs = Date.now() - startedAt;

                    const names: string[] = [];
                    if (Array.isArray(result)) {
                        for (const item of result) {
                            if (item && typeof (item as unknown as { name?: unknown }).name === 'string') {
                                names.push((item as unknown as { name: string }).name);
                            }
                        }
                    }
                    const topNames = names.slice(0, maxNames);
                    const expectedPresent = expectedNames
                        ? Object.fromEntries(expectedNames.map((name) => [name, topNames.includes(name)]))
                        : undefined;

                    workspaceSymbolCapture = {
                        durationMs,
                        query,
                        symbolCount: names.length,
                        topNames,
                        expectedNames,
                        expectedPresent,
                        error: captureError
                    };
                    break;
                }
                case 'waitForInternalStatus': {
                    const mode = step.payload?.mode === 'quiescent' ? 'quiescent' : 'idle';
                    const timeoutMs = Math.max(1000, step.payload?.timeoutMs ?? 180000);
                    if (mode === 'quiescent') {
                        await waitForClientQuiescent(step.label || 'replay waitForInternalStatus quiescent');
                    } else {
                        await waitForIndexingIdle(step.label || 'replay waitForInternalStatus idle', {
                            attempts: Math.max(1, Math.ceil(timeoutMs / 500)),
                            delayMs: 500
                        });
                    }
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

            if (completionCapturePromise) {
                completionCapture = await completionCapturePromise;
            }
            if (signatureHelpCapturePromise) {
                signatureHelpCapture = await signatureHelpCapturePromise;
            }
            stepReports.push({
                stepLabel: step.label,
                stepKind: step.kind,
                actionStartedAtMs,
                actionEndedAtMs: Date.now(),
                documentUri,
                samples,
                anomalies: detectReplayAnomalies(step, samples),
                completionCapture,
                signatureHelpCapture,
                workspaceSymbolCapture
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
