import * as vscode from 'vscode';

import type { ReplaySampleSnapshot, ReplayStep } from './real_workspace_replay_types';

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

export async function sampleReplayWindow(
    step: ReplayStep,
    documentUri?: string,
    baselineInternalStatus?: unknown
): Promise<ReplaySampleSnapshot[]> {
    const delaysMs = step.samplingWindow?.delaysMs ?? [];
    if (delaysMs.length === 0) {
        return [];
    }

    const samples: ReplaySampleSnapshot[] = [];
    const startedAt = Date.now();

    for (const targetDelay of delaysMs) {
        const elapsed = Date.now() - startedAt;
        if (targetDelay > elapsed) {
            await delay(targetDelay - elapsed);
        }

        const [internalStatus, latestMetrics, runtimeDebug, interactiveDebug] = await Promise.all([
            vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
            vscode.commands.executeCommand<any>('nsf._getLatestMetrics'),
            step.samplingWindow?.captureRuntimeDebug && documentUri
                ? vscode.commands.executeCommand<any>('nsf._getDocumentRuntimeDebug', { uris: [documentUri] })
                : Promise.resolve(undefined),
            step.samplingWindow?.captureInteractiveDebug && documentUri
                ? vscode.commands.executeCommand<any>('nsf._getInteractiveRuntimeDebug', { uri: documentUri })
                : Promise.resolve(undefined)
        ]);

        const sample: ReplaySampleSnapshot = {
            offsetMs: targetDelay,
            internalStatus,
            latestMetrics,
            runtimeDebug,
            interactiveDebug,
            baselineInternalStatus
        };

        samples.push(sample);
    }

    return samples;
}
