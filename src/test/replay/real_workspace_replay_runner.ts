import * as vscode from 'vscode';

import { detectReplayAnomalies } from './real_workspace_replay_analyzer';
import { resolveReplaySamplingDelays, sampleReplayWindow } from './real_workspace_replay_sampler';
import { resolveReplayAnchor } from './real_workspace_replay_targets';
import type {
    ReplayLocationExpectation,
    ReplaySampleSnapshot,
    ReplayScript,
    ReplayStep,
    ReplayTypingProbe
} from './real_workspace_replay_types';
import {
    countWorkspaceEdits,
    deleteLeftForTests,
    touchDocument,
    typeTextForTests,
    typeWithEditorFocusForTests,
    waitForClientQuiescent
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

function markdownContentToString(value: unknown): string {
    if (typeof value === 'string') {
        return value;
    }
    const markdown = value as { value?: unknown } | undefined;
    if (typeof markdown?.value === 'string') {
        return markdown.value;
    }
    const languageString = value as { language?: unknown; value?: unknown } | undefined;
    if (typeof languageString?.language === 'string' && typeof languageString?.value === 'string') {
        return `${languageString.language}\n${languageString.value}`;
    }
    return String(value ?? '');
}

function hoverContentsToStrings(hovers: vscode.Hover[] | undefined, maxContents: number): string[] {
    const contents: string[] = [];
    for (const hover of hovers ?? []) {
        for (const content of hover.contents ?? []) {
            const text = markdownContentToString(content).trim();
            if (text.length > 0) {
                contents.push(text);
            }
            if (contents.length >= maxContents) {
                return contents;
            }
        }
    }
    return contents;
}

function locationUriString(value: unknown): string {
    return locationUri(value)?.toString() ?? '';
}

function locationUri(value: unknown): vscode.Uri | undefined {
    const directUri = (value as { uri?: unknown } | undefined)?.uri;
    if (directUri instanceof vscode.Uri) {
        return directUri;
    }
    const targetUri = (value as { targetUri?: unknown } | undefined)?.targetUri;
    if (targetUri instanceof vscode.Uri) {
        return targetUri;
    }
    return undefined;
}

function locationRange(value: unknown): vscode.Range | undefined {
    const range = (value as { targetSelectionRange?: unknown } | undefined)?.targetSelectionRange ??
        (value as { range?: unknown } | undefined)?.range ??
        (value as { targetRange?: unknown } | undefined)?.targetRange;
    if (range instanceof vscode.Range) {
        return range;
    }
    return undefined;
}

function locationRangeString(value: unknown): string {
    const range = locationRange(value);
    if (!range) {
        return '';
    }
    return `${range.start.line + 1}:${range.start.character + 1}`;
}

function locationToString(value: unknown): string {
    const uri = locationUriString(value);
    const range = locationRangeString(value);
    return range.length > 0 ? `${uri}#${range}` : uri;
}

async function locationToDetail(value: unknown): Promise<LocationCaptureDetail | undefined> {
    const uri = locationUri(value);
    if (!uri) {
        return undefined;
    }
    const range = locationRange(value);
    const detail: LocationCaptureDetail = {
        uri: uri.toString(),
        fsPath: uri.scheme === 'file' ? uri.fsPath : undefined,
        range: range
            ? {
                  startLine: range.start.line,
                  startCharacter: range.start.character,
                  endLine: range.end.line,
                  endCharacter: range.end.character
              }
            : undefined
    };
    if (range) {
        try {
            const document = await vscode.workspace.openTextDocument(uri);
            detail.targetText = document.getText(range);
            detail.lineText = getLineText(document, range.start.line);
        } catch {
            // Keep URI/range details even if the target file cannot be opened.
        }
    }
    return detail;
}

async function locationDetails(values: unknown[] | undefined, maxLocations: number): Promise<LocationCaptureDetail[]> {
    const details: LocationCaptureDetail[] = [];
    for (const value of values ?? []) {
        const detail = await locationToDetail(value);
        if (detail) {
            details.push(detail);
        }
        if (details.length >= maxLocations) {
            break;
        }
    }
    return details;
}

function locationExpectationKey(expectation: ReplayLocationExpectation, index: number): string {
    const parts = [
        expectation.uriSubstring ? `uri=${expectation.uriSubstring}` : '',
        expectation.targetText ? `text=${expectation.targetText}` : '',
        expectation.lineTextSubstring ? `line=${expectation.lineTextSubstring}` : ''
    ].filter((part) => part.length > 0);
    return parts.length > 0 ? parts.join('|') : `expectation-${index + 1}`;
}

function locationDetailMatchesExpectation(
    detail: LocationCaptureDetail,
    expectation: ReplayLocationExpectation
): boolean {
    if (
        expectation.uriSubstring &&
        !detail.uri.toLowerCase().includes(expectation.uriSubstring.toLowerCase()) &&
        !detail.fsPath?.toLowerCase().includes(expectation.uriSubstring.toLowerCase())
    ) {
        return false;
    }
    if (expectation.targetText && detail.targetText !== expectation.targetText) {
        return false;
    }
    if (expectation.lineTextSubstring && !detail.lineText?.includes(expectation.lineTextSubstring)) {
        return false;
    }
    return true;
}

function flattenDocumentSymbolNames(
    symbols: Array<vscode.DocumentSymbol | vscode.SymbolInformation> | undefined,
    output: string[] = []
): string[] {
    for (const symbol of symbols ?? []) {
        const name = (symbol as { name?: unknown }).name;
        if (typeof name === 'string' && name.length > 0) {
            output.push(name);
        }
        const children = (symbol as vscode.DocumentSymbol).children;
        if (Array.isArray(children) && children.length > 0) {
            flattenDocumentSymbolNames(children, output);
        }
    }
    return output;
}

function inlayHintLabelToString(label: unknown): string {
    if (typeof label === 'string') {
        return label;
    }
    if (Array.isArray(label)) {
        return label
            .map((part) => {
                if (typeof part === 'string') {
                    return part;
                }
                const value = (part as { value?: unknown } | undefined)?.value;
                return typeof value === 'string' ? value : '';
            })
            .join('');
    }
    return String(label ?? '');
}

async function waitForReplayServerIndexingIdle(label: string, timeoutMs: number): Promise<void> {
    const deadline = Date.now() + timeoutMs;
    let lastValue: unknown;
    while (Date.now() < deadline) {
        try {
            lastValue = await vscode.commands.executeCommand<any>('nsf._sendServerRequest', {
                method: 'nsf/getIndexingState',
                params: {}
            });
            const state = (lastValue as any)?.state;
            const queued = (lastValue as any)?.pending?.queuedTasks ?? 0;
            const running = (lastValue as any)?.pending?.runningWorkers ?? 0;
            if (state === 'Idle' && queued === 0 && running === 0) {
                return;
            }
        } catch {
            // Keep polling; extension activation and server startup can race replay setup.
        }
        await delay(500);
    }
    throw new Error(`[real-replay] Timed out waiting for ${label}. Last=${JSON.stringify(lastValue)}`);
}

type CompletionCaptureReport = {
    measurementMode?: 'combined' | 'separated-ui-provider';
    uiCoverageTriggerSource?: CompletionUiCoverageTriggerSource;
    durationMs: number;
    executeProviderDurationMs?: number;
    triggerKind: 'Invoked' | 'TriggerCharacter';
    triggerCharacter?: string;
    triggerText?: string;
    triggerStartOffset?: number;
    triggerEndOffset?: number;
    itemCount: number;
    topLabels: string[];
    expectedLabels?: string[];
    expectedPresent?: Record<string, boolean>;
    line?: number;
    character?: number;
    lineText?: string;
    uiTrigger?: UiTriggerReport;
    uiCoverage?: UiCoverageReport;
    providerVerification?: ProviderVerificationReport;
    requestCounters?: RequestCounterTrace;
    error?: string;
    lastCompletionDebug?: unknown;
    directServerCompletion?: {
        durationMs: number;
        triggerKind: 'Invoked' | 'TriggerCharacter';
        itemCount: number;
        topLabels: string[];
        expectedPresent?: Record<string, boolean>;
        error?: string;
    };
};

type SignatureHelpCaptureReport = {
    measurementMode?: 'combined' | 'separated-ui-provider';
    durationMs: number;
    executeProviderDurationMs?: number;
    triggerCharacter?: string;
    triggerText?: string;
    triggerStartOffset?: number;
    triggerEndOffset?: number;
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
    uiTrigger?: UiTriggerReport;
    uiCoverage?: UiCoverageReport;
    providerVerification?: ProviderVerificationReport;
    requestCounters?: RequestCounterTrace;
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
    workspaceIndexDebug?: unknown;
};

type HoverCaptureReport = {
    durationMs: number;
    hoverCount: number;
    topContents: string[];
    expectedSubstrings?: string[];
    expectedMatched?: Record<string, boolean>;
    line?: number;
    character?: number;
    lineText?: string;
    error?: string;
};

type LocationCaptureReport = {
    durationMs: number;
    locationCount: number;
    topLocations: string[];
    topLocationDetails: LocationCaptureDetail[];
    expectedUriSubstrings?: string[];
    expectedMatched?: Record<string, boolean>;
    expectedTargetTexts?: string[];
    expectedTargetTextMatched?: Record<string, boolean>;
    expectedLineTextSubstrings?: string[];
    expectedLineTextMatched?: Record<string, boolean>;
    expectedLocationMatches?: ReplayLocationExpectation[];
    expectedLocationMatched?: Record<string, boolean>;
    minLocations?: number;
    maxLocations?: number;
    maxDurationMs?: number;
    withinExpectedMin?: boolean;
    withinExpectedMax?: boolean;
    withinExpectedDuration?: boolean;
    line?: number;
    character?: number;
    lineText?: string;
    error?: string;
};

type LocationCaptureDetail = {
    uri: string;
    fsPath?: string;
    range?: {
        startLine: number;
        startCharacter: number;
        endLine: number;
        endCharacter: number;
    };
    targetText?: string;
    lineText?: string;
};

type DocumentSymbolCaptureReport = {
    durationMs: number;
    symbolCount: number;
    topNames: string[];
    expectedNames?: string[];
    expectedPresent?: Record<string, boolean>;
    error?: string;
};

type InlayHintsCaptureReport = {
    durationMs: number;
    hintCount: number;
    rangeStartLine: number;
    rangeEndLine: number;
    topLabels: string[];
    expectedLabels?: string[];
    expectedPresent?: Record<string, boolean>;
    minHints?: number;
    maxHints?: number;
    withinExpectedMin?: boolean;
    withinExpectedMax?: boolean;
    error?: string;
};

type PrepareRenameCaptureReport = {
    durationMs: number;
    placeholder?: string;
    expectedPlaceholder?: string;
    placeholderMatched?: boolean;
    line?: number;
    character?: number;
    lineText?: string;
    error?: string;
};

type RenameEditCaptureReport = {
    durationMs: number;
    newName: string;
    changeCount: number;
    minChanges?: number;
    maxChanges?: number;
    withinExpectedMin?: boolean;
    withinExpectedMax?: boolean;
    line?: number;
    character?: number;
    lineText?: string;
    error?: string;
};

type SemanticTokensCaptureReport = {
    durationMs: number;
    dataLength: number;
    minDataLength?: number;
    maxDataLength?: number;
    withinExpectedMin?: boolean;
    withinExpectedMax?: boolean;
    error?: string;
};

type DiagnosticsCaptureReport = {
    durationMs: number;
    diagnosticCount: number;
    errorCount: number;
    warningCount: number;
    runtimeReadyObserved?: boolean;
    requireRuntimeReady?: boolean;
    touchCount?: number;
    firstTouchAtMs?: number;
    lastTouchAtMs?: number;
    readyWaitTimedOut?: boolean;
    expectedMaxDiagnostics?: number;
    expectedMaxErrors?: number;
    withinExpectedDiagnostics?: boolean;
    withinExpectedErrors?: boolean;
    topMessages: string[];
};

type FullDocumentTypingCheckpointReport = {
    label: string;
    offset: number;
    line: number;
    samples: ReplaySampleSnapshot[];
    diagnostics: DiagnosticsCaptureReport;
    inlayContinuitySample?: InlayContinuitySampleReport;
};

type InlayContinuitySampleReport = {
    label: string;
    offset: number;
    line: number;
    durationMs: number;
    hintCount: number;
    rangeStartLine: number;
    rangeEndLine: number;
    topLabels: string[];
    error?: string;
};

type InlayContinuityDropReport = {
    startLabel: string;
    startLine: number;
    recoveredAtLabel?: string;
    recoveredAtLine?: number;
    sampleCount: number;
};

type FullDocumentTypingProbeReport = {
    label: string;
    category?: string;
    kind: ReplayTypingProbe['kind'];
    offset: number;
    line: number;
    triggerText?: string;
    triggerCharacter?: string;
    triggerStartOffset?: number;
    triggerEndOffset?: number;
    nativeTrigger?: boolean;
    triggerTyping?: TriggerTypingReport;
    samples: ReplaySampleSnapshot[];
    completionCapture?: CompletionCaptureReport;
    signatureHelpCapture?: SignatureHelpCaptureReport;
    diagnosticsCapture?: DiagnosticsCaptureReport;
    error?: string;
};

type FullDocumentTypingReport = {
    durationMs: number;
    sourceLineCount: number;
    sourceCharacterCount: number;
    finalCharacterCount: number;
    finalTextMatchesSource: boolean;
    firstMismatchOffset?: number;
    sourceMismatchSnippet?: string;
    finalMismatchSnippet?: string;
    charactersPerEdit: number;
    checkpointEveryLines: number;
    checkpointCount: number;
    probeCount: number;
    inlayContinuity?: {
        enabled: boolean;
        sampleCount: number;
        nonEmptySampleCount: number;
        firstNonEmptyLabel?: string;
        firstNonEmptyLine?: number;
        zeroAfterFirstNonEmptyCount: number;
        errorAfterFirstNonEmptyCount: number;
        transientDropCount: number;
        trailingMissingRunLength: number;
        transientDropDetected: boolean;
        endedMissingAfterVisible: boolean;
        drops: InlayContinuityDropReport[];
        samples: InlayContinuitySampleReport[];
    };
    anomalies: string[];
    checkpoints: FullDocumentTypingCheckpointReport[];
    probes: FullDocumentTypingProbeReport[];
};

type TypingProbePlan = {
    probe: ReplayTypingProbe;
    offset: number;
    triggerText?: string;
    triggerCharacter?: string;
    triggerStartOffset: number;
    nativeTrigger: boolean;
    triggerError?: string;
};

type UiTriggerReport = {
    command: string;
    durationMs: number;
    commandDurationMs: number;
    delayMs: number;
    error?: string;
};

type RequestCounterSnapshot = {
    completionRequestCount?: number;
    signatureHelpRequestCount?: number;
    activeRpcCount?: number;
    completionLastProviderTiming?: unknown;
    signatureHelpLastProviderTiming?: unknown;
    completionRecentProviderTimings?: unknown[];
    signatureHelpRecentProviderTimings?: unknown[];
};

type RequestCounterDelta = {
    completionRequests: number;
    signatureHelpRequests: number;
};

type RequestCounterTrace = {
    beforeCapture?: RequestCounterSnapshot;
    beforeUiTrigger?: RequestCounterSnapshot;
    afterUiTrigger?: RequestCounterSnapshot;
    uiTriggerDelta?: RequestCounterDelta;
    beforeProvider?: RequestCounterSnapshot;
    afterProvider?: RequestCounterSnapshot;
    providerDelta?: RequestCounterDelta;
    beforeDirectServer?: RequestCounterSnapshot;
    afterDirectServer?: RequestCounterSnapshot;
    directServerDelta?: RequestCounterDelta;
    totalDelta?: RequestCounterDelta;
    nativeTriggerDelta?: RequestCounterDelta;
    afterUiQueueQuiet?: RequestCounterSnapshot;
    uiQueueQuietDelta?: RequestCounterDelta;
};

type ProviderQueueQuietReport = {
    durationMs: number;
    quietMs: number;
    timeoutMs: number;
    timedOut: boolean;
    startedAtMs: number;
    completedAtMs: number;
    relativeStartedAtMs?: number;
    relativeCompletedAtMs?: number;
    before?: RequestCounterSnapshot;
    after?: RequestCounterSnapshot;
    requestDelta?: RequestCounterDelta;
};

type UiCoverageReport = {
    measurementMode: 'uiCoverage';
    triggerSource?: CompletionUiCoverageTriggerSource;
    nativeTriggerDelta?: RequestCounterDelta;
    uiTrigger?: UiTriggerReport;
    providerRequestSequence?: ProviderRequestSequenceEntry[];
    latestCompletionDebug?: unknown;
    completionDebugHistory?: unknown[];
    requestBurstCount?: number;
    firstProviderRequestAtMs?: number;
    lastProviderRequestCompletedAtMs?: number;
    requestCounters: {
        before?: RequestCounterSnapshot;
        afterUiTrigger?: RequestCounterSnapshot;
        afterQueueQuiet?: RequestCounterSnapshot;
        uiTriggerDelta?: RequestCounterDelta;
        totalTriggerDelta?: RequestCounterDelta;
    };
    queueQuiet?: ProviderQueueQuietReport;
};

type CompletionUiCoverageTriggerSource = 'nativeOnly' | 'explicitSuggest';

type ProviderRequestSequenceEntry = {
    sequence?: number;
    documentVersion?: number;
    documentIsDirty?: boolean;
    documentVersionAtNextStart?: number;
    documentIsDirtyAtNextStart?: boolean;
    documentVersionAtLspStart?: number;
    documentIsDirtyAtLspStart?: boolean;
    documentVersionAtProviderReturn?: number;
    documentIsDirtyAtProviderReturn?: boolean;
    triggerKind?: number;
    triggerCharacter?: string;
    isRetrigger?: boolean;
    line?: number;
    character?: number;
    prefixLength?: number;
    totalMs?: number;
    nextWaitMs?: number;
    nextExecutionMs?: number;
    lspRequestMs?: number;
    lspStartDelayMs?: number;
    lspCompletionToProviderReturnMs?: number;
    lspRequestCount?: number;
    activeSameKindProviderCountAtStart?: number;
    activeSameKindNextCountAtStart?: number;
    itemCount?: number;
    signatureCount?: number;
    startedAtMs?: number;
    completedAtMs?: number;
    nextStartedAtMs?: number;
    nextCompletedAtMs?: number;
    lspRequestStartedAtMs?: number;
    lspRequestCompletedAtMs?: number;
    relativeStartedAtMs?: number;
    relativeCompletedAtMs?: number;
    relativeNextStartedAtMs?: number;
    relativeNextCompletedAtMs?: number;
    relativeLspRequestStartedAtMs?: number;
    relativeLspRequestCompletedAtMs?: number;
    completionCoordinatorAction?: string;
    completionCoordinatorSource?: string;
    completionCoordinatorKey?: string;
    completionCoordinatorPrefixLength?: number;
    completionDebugRequestId?: string;
};

type ProviderVerificationReport = {
    measurementMode: 'providerVerification';
    durationMs: number;
    requestCounters: {
        before?: RequestCounterSnapshot;
        after?: RequestCounterSnapshot;
        delta?: RequestCounterDelta;
    };
    clientProviderTiming?: unknown;
    uiQueueQuietTimedOut?: boolean;
};

type TriggerTypingReport = {
    durationMs: number;
    typedText: string;
    nativeTrigger: boolean;
    before?: RequestCounterSnapshot;
    after?: RequestCounterSnapshot;
    requestDelta?: RequestCounterDelta;
    providerRequestSequence?: ProviderRequestSequenceEntry[];
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
    hoverCapture?: HoverCaptureReport;
    definitionCapture?: LocationCaptureReport;
    referencesCapture?: LocationCaptureReport;
    documentSymbolCapture?: DocumentSymbolCaptureReport;
    inlayHintsCapture?: InlayHintsCaptureReport;
    prepareRenameCapture?: PrepareRenameCaptureReport;
    renameEditCapture?: RenameEditCaptureReport;
    semanticTokensCapture?: SemanticTokensCaptureReport;
    workspaceSymbolCapture?: WorkspaceSymbolCaptureReport;
    fullDocumentTyping?: FullDocumentTypingReport;
};

function clampInt(value: number | undefined, fallback: number, min: number, max: number): number {
    const resolved = Number.isFinite(value) ? Math.trunc(value as number) : fallback;
    return Math.max(min, Math.min(max, resolved));
}

function countLines(text: string): number {
    if (text.length === 0) {
        return 0;
    }
    return text.split(/\r\n|\r|\n/).length;
}

function lineAtOffset(text: string, offset: number): number {
    const prefix = text.slice(0, Math.max(0, Math.min(text.length, offset)));
    return Math.max(0, countLines(prefix) - 1);
}

function lastCharacter(text: string | undefined): string | undefined {
    if (!text || text.length === 0) {
        return undefined;
    }
    return text[text.length - 1];
}

function completionTriggerKind(triggerCharacter: string | undefined): 'Invoked' | 'TriggerCharacter' {
    return triggerCharacter ? 'TriggerCharacter' : 'Invoked';
}

function uiTriggerDelayMs(payload: ReplayTypingProbe['payload'] | undefined): number {
    return clampInt(payload?.uiTriggerDelayMs, 80, 0, 5000);
}

function normalizeCompletionUiMode(value: unknown): CompletionUiCoverageTriggerSource | undefined {
    if (typeof value !== 'string') {
        return undefined;
    }
    const normalized = value.trim().toLowerCase();
    if (normalized === 'native' || normalized === 'native-only' || normalized === 'nativeonly') {
        return 'nativeOnly';
    }
    if (normalized === 'explicit' || normalized === 'explicit-suggest' || normalized === 'explicitsuggest') {
        return 'explicitSuggest';
    }
    return undefined;
}

function completionUiModeOverride(): CompletionUiCoverageTriggerSource | undefined {
    return normalizeCompletionUiMode(process.env.NSF_REAL_REPLAY_COMPLETION_UI_MODE);
}

function completionUiCoverageTriggerSource(
    payload: ReplayTypingProbe['payload'] | undefined
): CompletionUiCoverageTriggerSource {
    const mode = completionUiModeOverride() ?? normalizeCompletionUiMode(payload?.completionUiMode);
    if (mode) {
        return mode;
    }
    return payload?.triggerSuggestUi === true ? 'explicitSuggest' : 'nativeOnly';
}

async function readRequestCounterSnapshot(): Promise<RequestCounterSnapshot | undefined> {
    try {
        const [status, providerTimingStatus] = await Promise.all([
            vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
            Promise.resolve(vscode.commands.executeCommand<any>('nsf._getProviderTimingStatus')).catch(() => undefined)
        ]);
        return {
            completionRequestCount:
                typeof status?.completionRequestCount === 'number' ? status.completionRequestCount : undefined,
            signatureHelpRequestCount:
                typeof status?.signatureHelpRequestCount === 'number' ? status.signatureHelpRequestCount : undefined,
            activeRpcCount:
                typeof status?.activeRpcCount === 'number' ? status.activeRpcCount : undefined,
            completionLastProviderTiming: status?.completionLastProviderTiming,
            signatureHelpLastProviderTiming: status?.signatureHelpLastProviderTiming,
            completionRecentProviderTimings: Array.isArray(providerTimingStatus?.completionRecentProviderTimings)
                ? providerTimingStatus.completionRecentProviderTimings
                : undefined,
            signatureHelpRecentProviderTimings: Array.isArray(providerTimingStatus?.signatureHelpRecentProviderTimings)
                ? providerTimingStatus.signatureHelpRecentProviderTimings
                : undefined
        };
    } catch {
        return undefined;
    }
}

function recentProviderTimings(
    snapshot: RequestCounterSnapshot | undefined,
    kind: 'completion' | 'signatureHelp'
): unknown[] {
    const timings = kind === 'completion'
        ? snapshot?.completionRecentProviderTimings
        : snapshot?.signatureHelpRecentProviderTimings;
    return Array.isArray(timings) ? timings : [];
}

function timingSequence(value: unknown): number {
    const sequence = (value as { sequence?: unknown } | undefined)?.sequence;
    return typeof sequence === 'number' && Number.isFinite(sequence) ? sequence : 0;
}

function timingNumber(value: unknown, field: string): number | undefined {
    const raw = (value as Record<string, unknown> | undefined)?.[field];
    return typeof raw === 'number' && Number.isFinite(raw) ? raw : undefined;
}

function timingString(value: unknown, field: string): string | undefined {
    const raw = (value as Record<string, unknown> | undefined)?.[field];
    return typeof raw === 'string' ? raw : undefined;
}

function timingBoolean(value: unknown, field: string): boolean | undefined {
    const raw = (value as Record<string, unknown> | undefined)?.[field];
    return typeof raw === 'boolean' ? raw : undefined;
}

async function readLastCompletionDebug(): Promise<unknown | undefined> {
    try {
        return await vscode.commands.executeCommand<any>('nsf._getLastCompletionDebug');
    } catch {
        return undefined;
    }
}

function completionDebugHistoryFromSnapshot(snapshot: unknown): unknown[] | undefined {
    const recent = (snapshot as { recent?: unknown } | undefined)?.recent;
    return Array.isArray(recent) ? recent : undefined;
}

function providerRequestSequence(
    kind: 'completion' | 'signatureHelp',
    before: RequestCounterSnapshot | undefined,
    after: RequestCounterSnapshot | undefined,
    captureStartedAt: number
): ProviderRequestSequenceEntry[] {
    const beforeSequence = providerLastSequence(before, kind);
    return recentProviderTimings(after, kind)
        .filter((timing) => timingSequence(timing) > beforeSequence)
        .map((timing) => {
            const startedAtMs = timingNumber(timing, 'startedAtMs');
            const completedAtMs = timingNumber(timing, 'completedAtMs');
            const nextStartedAtMs = timingNumber(timing, 'nextStartedAtMs');
            const nextCompletedAtMs = timingNumber(timing, 'nextCompletedAtMs');
            const lspRequestStartedAtMs = timingNumber(timing, 'lspRequestStartedAtMs');
            const lspRequestCompletedAtMs = timingNumber(timing, 'lspRequestCompletedAtMs');
            return {
                sequence: timingNumber(timing, 'sequence'),
                documentVersion: timingNumber(timing, 'documentVersion'),
                documentIsDirty: timingBoolean(timing, 'documentIsDirty'),
                documentVersionAtNextStart: timingNumber(timing, 'documentVersionAtNextStart'),
                documentIsDirtyAtNextStart: timingBoolean(timing, 'documentIsDirtyAtNextStart'),
                documentVersionAtLspStart: timingNumber(timing, 'documentVersionAtLspStart'),
                documentIsDirtyAtLspStart: timingBoolean(timing, 'documentIsDirtyAtLspStart'),
                documentVersionAtProviderReturn: timingNumber(timing, 'documentVersionAtProviderReturn'),
                documentIsDirtyAtProviderReturn: timingBoolean(timing, 'documentIsDirtyAtProviderReturn'),
                triggerKind: timingNumber(timing, 'triggerKind'),
                triggerCharacter: timingString(timing, 'triggerCharacter'),
                isRetrigger: timingBoolean(timing, 'isRetrigger'),
                line: timingNumber(timing, 'line'),
                character: timingNumber(timing, 'character'),
                prefixLength: timingNumber(timing, 'prefixLength'),
                totalMs: timingNumber(timing, 'totalMs'),
                nextWaitMs: timingNumber(timing, 'nextWaitMs'),
                nextExecutionMs: timingNumber(timing, 'nextExecutionMs'),
                lspRequestMs: timingNumber(timing, 'lspRequestMs'),
                lspStartDelayMs: timingNumber(timing, 'lspStartDelayMs'),
                lspCompletionToProviderReturnMs: timingNumber(timing, 'lspCompletionToProviderReturnMs'),
                lspRequestCount: timingNumber(timing, 'lspRequestCount'),
                activeSameKindProviderCountAtStart: timingNumber(timing, 'activeSameKindProviderCountAtStart'),
                activeSameKindNextCountAtStart: timingNumber(timing, 'activeSameKindNextCountAtStart'),
                itemCount: timingNumber(timing, 'itemCount'),
                signatureCount: timingNumber(timing, 'signatureCount'),
                startedAtMs,
                completedAtMs,
                nextStartedAtMs,
                nextCompletedAtMs,
                lspRequestStartedAtMs,
                lspRequestCompletedAtMs,
                relativeStartedAtMs: startedAtMs === undefined ? undefined : Math.max(0, startedAtMs - captureStartedAt),
                relativeCompletedAtMs: completedAtMs === undefined ? undefined : Math.max(0, completedAtMs - captureStartedAt),
                relativeNextStartedAtMs: nextStartedAtMs === undefined ? undefined : Math.max(0, nextStartedAtMs - captureStartedAt),
                relativeNextCompletedAtMs: nextCompletedAtMs === undefined ? undefined : Math.max(0, nextCompletedAtMs - captureStartedAt),
                relativeLspRequestStartedAtMs: lspRequestStartedAtMs === undefined ? undefined : Math.max(0, lspRequestStartedAtMs - captureStartedAt),
                relativeLspRequestCompletedAtMs: lspRequestCompletedAtMs === undefined ? undefined : Math.max(0, lspRequestCompletedAtMs - captureStartedAt),
                completionCoordinatorAction: timingString(timing, 'completionCoordinatorAction'),
                completionCoordinatorSource: timingString(timing, 'completionCoordinatorSource'),
                completionCoordinatorKey: timingString(timing, 'completionCoordinatorKey'),
                completionCoordinatorPrefixLength: timingNumber(timing, 'completionCoordinatorPrefixLength'),
                completionDebugRequestId: timingString(timing, 'completionDebugRequestId')
            };
        });
}

function counterValue(snapshot: RequestCounterSnapshot | undefined, field: keyof RequestCounterSnapshot): number {
    const value = snapshot?.[field];
    return typeof value === 'number' ? value : 0;
}

function requestCounterDelta(
    before: RequestCounterSnapshot | undefined,
    after: RequestCounterSnapshot | undefined
): RequestCounterDelta {
    return {
        completionRequests: Math.max(
            0,
            counterValue(after, 'completionRequestCount') - counterValue(before, 'completionRequestCount')
        ),
        signatureHelpRequests: Math.max(
            0,
            counterValue(after, 'signatureHelpRequestCount') - counterValue(before, 'signatureHelpRequestCount')
        )
    };
}

function providerRequestCount(
    snapshot: RequestCounterSnapshot | undefined,
    kind: 'completion' | 'signatureHelp'
): number {
    return kind === 'completion'
        ? counterValue(snapshot, 'completionRequestCount')
        : counterValue(snapshot, 'signatureHelpRequestCount');
}

function providerLastSequence(
    snapshot: RequestCounterSnapshot | undefined,
    kind: 'completion' | 'signatureHelp'
): number {
    const timing = kind === 'completion'
        ? snapshot?.completionLastProviderTiming
        : snapshot?.signatureHelpLastProviderTiming;
    const sequence = (timing as { sequence?: unknown } | undefined)?.sequence;
    return typeof sequence === 'number' && Number.isFinite(sequence) ? sequence : 0;
}

function providerTimingFromSnapshot(
    snapshot: RequestCounterSnapshot | undefined,
    kind: 'completion' | 'signatureHelp'
): unknown {
    return kind === 'completion'
        ? snapshot?.completionLastProviderTiming
        : snapshot?.signatureHelpLastProviderTiming;
}

async function waitForProviderQueueQuiet(
    kind: 'completion' | 'signatureHelp',
    before: RequestCounterSnapshot | undefined,
    quietMs = 160,
    timeoutMs = 3000
): Promise<ProviderQueueQuietReport> {
    const startedAt = Date.now();
    let lastSnapshot = await readRequestCounterSnapshot();
    let lastCount = providerRequestCount(lastSnapshot, kind);
    let stableSince = Date.now();
    while (Date.now() - startedAt < timeoutMs) {
        await delay(50);
        const current = await readRequestCounterSnapshot();
        const currentCount = providerRequestCount(current, kind);
        const currentSequence = providerLastSequence(current, kind);
        const countChanged = currentCount !== lastCount;
        if (countChanged) {
            lastCount = currentCount;
            stableSince = Date.now();
        }
        lastSnapshot = current;
        if (!countChanged && Date.now() - stableSince >= quietMs && currentSequence >= currentCount) {
            const completedAt = Date.now();
            return {
                durationMs: completedAt - startedAt,
                quietMs,
                timeoutMs,
                timedOut: false,
                startedAtMs: startedAt,
                completedAtMs: completedAt,
                before,
                after: current,
                requestDelta: requestCounterDelta(before, current)
            };
        }
    }
    const completedAt = Date.now();
    return {
        durationMs: completedAt - startedAt,
        quietMs,
        timeoutMs,
        timedOut: true,
        startedAtMs: startedAt,
        completedAtMs: completedAt,
        before,
        after: lastSnapshot,
        requestDelta: requestCounterDelta(before, lastSnapshot)
    };
}

function attachQueueQuietRelativeTimes(queueQuiet: ProviderQueueQuietReport, captureStartedAt: number): void {
    queueQuiet.relativeStartedAtMs = Math.max(0, queueQuiet.startedAtMs - captureStartedAt);
    queueQuiet.relativeCompletedAtMs = Math.max(0, queueQuiet.completedAtMs - captureStartedAt);
}

async function runEditorUiTriggerCommand(
    command: string,
    payload: ReplayTypingProbe['payload'] | undefined
): Promise<UiTriggerReport> {
    const delayMs = uiTriggerDelayMs(payload);
    const startedAt = Date.now();
    let commandDurationMs = 0;
    let error: string | undefined;
    try {
        await vscode.commands.executeCommand('workbench.action.focusActiveEditorGroup');
        const commandStartedAt = Date.now();
        await vscode.commands.executeCommand(command);
        commandDurationMs = Date.now() - commandStartedAt;
        if (delayMs > 0) {
            await delay(delayMs);
        }
    } catch (triggerError) {
        error = triggerError instanceof Error ? triggerError.message : String(triggerError);
    }
    return {
        command,
        durationMs: Date.now() - startedAt,
        commandDurationMs,
        delayMs,
        error
    };
}

async function closeEditorUiWidgets(): Promise<void> {
    for (const command of ['hideSuggestWidget', 'closeParameterHints']) {
        try {
            await vscode.commands.executeCommand(command);
        } catch {
            // Best effort: older VS Code builds may not expose every UI close command.
        }
    }
}

function expectedMissing(expectedPresent: Record<string, boolean> | undefined): string[] {
    if (!expectedPresent) {
        return [];
    }
    return Object.entries(expectedPresent)
        .filter(([, present]) => !present)
        .map(([label]) => label);
}

function findNthEndOffset(text: string, needle: string, occurrence: number): number {
    if (needle.length === 0) {
        return -1;
    }
    let searchFrom = 0;
    let foundAt = -1;
    for (let index = 0; index < Math.max(1, occurrence); index++) {
        foundAt = text.indexOf(needle, searchFrom);
        if (foundAt < 0) {
            return -1;
        }
        searchFrom = foundAt + needle.length;
    }
    return foundAt + needle.length;
}

function diagnosticSeverityName(severity: vscode.DiagnosticSeverity | undefined): string {
    switch (severity) {
        case vscode.DiagnosticSeverity.Error:
            return 'error';
        case vscode.DiagnosticSeverity.Warning:
            return 'warning';
        case vscode.DiagnosticSeverity.Information:
            return 'information';
        case vscode.DiagnosticSeverity.Hint:
            return 'hint';
        default:
            return 'unknown';
    }
}

function inlayHintLabelToText(label: unknown): string {
    if (typeof label === 'string') {
        return label;
    }
    if (Array.isArray(label)) {
        return label
            .map((part) => {
                if (part && typeof (part as { value?: unknown }).value === 'string') {
                    return (part as { value: string }).value;
                }
                return '';
            })
            .join('');
    }
    return '';
}

async function captureInlayContinuityAtEditor(
    activeEditor: vscode.TextEditor,
    label: string,
    offset: number,
    line: number
): Promise<InlayContinuitySampleReport> {
    const document = activeEditor.document;
    const startedAt = Date.now();
    const range = new vscode.Range(new vscode.Position(0, 0), document.positionAt(document.getText().length));
    let hints: vscode.InlayHint[] = [];
    let error: string | undefined;
    try {
        const result = await vscode.commands.executeCommand<vscode.InlayHint[]>(
            'vscode.executeInlayHintProvider',
            document.uri,
            range
        );
        hints = Array.isArray(result) ? result : [];
    } catch (captureError) {
        error = captureError instanceof Error ? captureError.message : String(captureError);
    }
    const topLabels = hints
        .slice(0, 20)
        .map((hint) => `${hint.position.line}:${inlayHintLabelToText(hint.label)}`)
        .filter((entry) => entry.length > 0);
    return {
        label,
        offset,
        line,
        durationMs: Date.now() - startedAt,
        hintCount: hints.length,
        rangeStartLine: range.start.line,
        rangeEndLine: range.end.line,
        topLabels,
        error
    };
}

function summarizeInlayContinuity(samples: InlayContinuitySampleReport[]): NonNullable<FullDocumentTypingReport['inlayContinuity']> {
    let firstNonEmpty: InlayContinuitySampleReport | undefined;
    let zeroAfterFirstNonEmptyCount = 0;
    let errorAfterFirstNonEmptyCount = 0;
    let currentDropStart: InlayContinuitySampleReport | undefined;
    let currentDropLength = 0;
    let trailingMissingRunLength = 0;
    const drops: InlayContinuityDropReport[] = [];

    const closeDrop = (recoveredAt: InlayContinuitySampleReport | undefined) => {
        if (!currentDropStart || currentDropLength <= 0) {
            return;
        }
        drops.push({
            startLabel: currentDropStart.label,
            startLine: currentDropStart.line,
            recoveredAtLabel: recoveredAt?.label,
            recoveredAtLine: recoveredAt?.line,
            sampleCount: currentDropLength
        });
        currentDropStart = undefined;
        currentDropLength = 0;
    };

    for (const sample of samples) {
        const nonEmpty = !sample.error && sample.hintCount > 0;
        if (nonEmpty) {
            if (!firstNonEmpty) {
                firstNonEmpty = sample;
            } else {
                closeDrop(sample);
            }
            trailingMissingRunLength = 0;
            continue;
        }

        if (!firstNonEmpty) {
            continue;
        }
        zeroAfterFirstNonEmptyCount++;
        if (sample.error) {
            errorAfterFirstNonEmptyCount++;
        }
        if (!currentDropStart) {
            currentDropStart = sample;
        }
        currentDropLength++;
        trailingMissingRunLength = currentDropLength;
    }

    const hasOpenDrop = Boolean(currentDropStart && currentDropLength > 0);
    if (hasOpenDrop) {
        drops.push({
            startLabel: currentDropStart!.label,
            startLine: currentDropStart!.line,
            sampleCount: currentDropLength
        });
    }
    const transientDropCount = drops.filter((drop) => typeof drop.recoveredAtLabel === 'string').length;

    return {
        enabled: true,
        sampleCount: samples.length,
        nonEmptySampleCount: samples.filter((sample) => !sample.error && sample.hintCount > 0).length,
        firstNonEmptyLabel: firstNonEmpty?.label,
        firstNonEmptyLine: firstNonEmpty?.line,
        zeroAfterFirstNonEmptyCount,
        errorAfterFirstNonEmptyCount,
        transientDropCount,
        trailingMissingRunLength: hasOpenDrop ? trailingMissingRunLength : 0,
        transientDropDetected: transientDropCount > 0,
        endedMissingAfterVisible: hasOpenDrop,
        drops,
        samples
    };
}

async function captureDiagnosticsAtEditor(
    activeEditor: vscode.TextEditor,
    payload?: ReplayTypingProbe['payload']
): Promise<DiagnosticsCaptureReport> {
    const startedAt = Date.now();
    const readyWaitMs = clampInt(payload?.waitForReadyMs, 0, 0, 120000);
    const maxDiagnostics = typeof payload?.maxDiagnostics === 'number' ? payload.maxDiagnostics : undefined;
    const maxErrors = typeof payload?.maxErrors === 'number' ? payload.maxErrors : undefined;
    const requireRuntimeReady = payload?.requireRuntimeReady === true;
    const touchEveryMs = clampInt(payload?.touchEveryMs, 0, 0, 60000);
    const maxTouches = clampInt(payload?.maxTouches, 0, 0, 100);

    const readDiagnostics = () => {
        const diagnostics = vscode.languages.getDiagnostics(activeEditor.document.uri);
        const severityCounts = new Map<string, number>();
        for (const diagnostic of diagnostics) {
            const severity = diagnosticSeverityName(diagnostic.severity);
            severityCounts.set(severity, (severityCounts.get(severity) ?? 0) + 1);
        }
        return { diagnostics, severityCounts };
    };
    const withinExpected = (diagnostics: readonly vscode.Diagnostic[], severityCounts: Map<string, number>) =>
        (maxDiagnostics === undefined || diagnostics.length <= maxDiagnostics) &&
        (maxErrors === undefined || (severityCounts.get('error') ?? 0) <= maxErrors);
    const runtimeReady = async () => {
        try {
            const response = await vscode.commands.executeCommand<any>(
                'nsf._getDocumentRuntimeDebug',
                { uris: [activeEditor.document.uri.toString()] }
            );
            const entry = response?.documents?.[0];
            const layer = entry?.lastDiagnosticsPublishLayer ?? '';
            return (
                Boolean(entry?.exists) &&
                (Boolean(entry?.deferredHasFullDiagnostics) ||
                    layer === 'GlobalContext' ||
                    layer === 'CurrentDocSemantic' ||
                    layer === 'LocalStructural')
            );
        } catch {
            return false;
        }
    };

    let ready = readyWaitMs <= 0;
    let current = readDiagnostics();
    const deadline = Date.now() + readyWaitMs;
    let touchCount = 0;
    let firstTouchAtMs: number | undefined;
    let lastTouchAtMs: number | undefined;
    let nextTouchAt = touchEveryMs > 0 ? startedAt + touchEveryMs : Number.POSITIVE_INFINITY;
    let runtimeReadyObserved = false;
    const hasExpectedBounds = maxDiagnostics !== undefined || maxErrors !== undefined;
    while (!ready && Date.now() < deadline) {
        if (withinExpected(current.diagnostics, current.severityCounts) && (!requireRuntimeReady || runtimeReadyObserved)) {
            ready = true;
            break;
        }
        if (await runtimeReady()) {
            runtimeReadyObserved = true;
            current = readDiagnostics();
            if (!hasExpectedBounds || withinExpected(current.diagnostics, current.severityCounts)) {
                ready = true;
                break;
            }
        }
        if (touchEveryMs > 0 && touchCount < maxTouches && Date.now() >= nextTouchAt) {
            await touchDocument(activeEditor.document);
            touchCount++;
            lastTouchAtMs = Date.now() - startedAt;
            firstTouchAtMs = firstTouchAtMs ?? lastTouchAtMs;
            nextTouchAt = Date.now() + touchEveryMs;
        }
        await delay(120);
        current = readDiagnostics();
    }
    if (!ready) {
        current = readDiagnostics();
    }

    return {
        durationMs: Date.now() - startedAt,
        diagnosticCount: current.diagnostics.length,
        errorCount: current.severityCounts.get('error') ?? 0,
        warningCount: current.severityCounts.get('warning') ?? 0,
        runtimeReadyObserved,
        requireRuntimeReady,
        touchCount: touchEveryMs > 0 ? touchCount : undefined,
        firstTouchAtMs,
        lastTouchAtMs,
        readyWaitTimedOut: readyWaitMs > 0 ? !ready : undefined,
        expectedMaxDiagnostics: maxDiagnostics,
        expectedMaxErrors: maxErrors,
        withinExpectedDiagnostics: maxDiagnostics === undefined ? undefined : current.diagnostics.length <= maxDiagnostics,
        withinExpectedErrors: maxErrors === undefined ? undefined : (current.severityCounts.get('error') ?? 0) <= maxErrors,
        topMessages: current.diagnostics.slice(0, 20).map((diagnostic) => diagnostic.message)
    };
}

async function captureCompletionAtEditor(
    activeEditor: vscode.TextEditor,
    payload?: ReplayTypingProbe['payload'],
    trigger?: Pick<TypingProbePlan, 'triggerText' | 'triggerCharacter' | 'triggerStartOffset' | 'offset'> & {
        triggerTyping?: TriggerTypingReport;
    }
): Promise<CompletionCaptureReport> {
    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
    const document = activeEditor.document;
    const position = activeEditor.selection.active;
    const lineText = getLineText(document, position.line);
    const triggerCharacter = typeof payload?.triggerCharacter === 'string'
        ? payload.triggerCharacter
        : trigger?.triggerCharacter;
    const triggerKind = completionTriggerKind(triggerCharacter);
    const expectedLabels = Array.isArray(payload?.expectedLabels)
        ? payload.expectedLabels.filter((entry): entry is string => typeof entry === 'string')
        : undefined;
    const maxLabels = Math.max(1, Math.min(200, payload?.maxLabels ?? 50));

    const captureStartedAt = Date.now();
    const uiCoverageTriggerSource = completionUiCoverageTriggerSource(payload);
    const requestCounters: RequestCounterTrace = {
        beforeCapture: await readRequestCounterSnapshot(),
        nativeTriggerDelta: trigger?.triggerTyping?.requestDelta
    };
    let uiTrigger: UiTriggerReport | undefined;
    let queueQuiet: ProviderQueueQuietReport | undefined;
    if (uiCoverageTriggerSource === 'explicitSuggest') {
        requestCounters.beforeUiTrigger = await readRequestCounterSnapshot();
        uiTrigger = await runEditorUiTriggerCommand('editor.action.triggerSuggest', payload);
        requestCounters.afterUiTrigger = await readRequestCounterSnapshot();
        requestCounters.uiTriggerDelta = requestCounterDelta(
            requestCounters.beforeUiTrigger,
            requestCounters.afterUiTrigger
        );
    }
    await closeEditorUiWidgets();
    queueQuiet = await waitForProviderQueueQuiet(
        'completion',
        requestCounters.beforeCapture
    );
    attachQueueQuietRelativeTimes(queueQuiet, captureStartedAt);
    requestCounters.afterUiQueueQuiet = queueQuiet.after;
    requestCounters.uiQueueQuietDelta = queueQuiet.requestDelta;
    const uiProviderRequestSequence = providerRequestSequence(
        'completion',
        requestCounters.beforeCapture,
        requestCounters.afterUiQueueQuiet,
        captureStartedAt
    );
    const latestCompletionDebug = await readLastCompletionDebug();
    const completionDebugHistory = completionDebugHistoryFromSnapshot(latestCompletionDebug);
    const uiCoverage: UiCoverageReport = {
        measurementMode: 'uiCoverage',
        triggerSource: uiCoverageTriggerSource,
        nativeTriggerDelta: requestCounters.nativeTriggerDelta,
        uiTrigger,
        providerRequestSequence: uiProviderRequestSequence,
        latestCompletionDebug,
        completionDebugHistory,
        requestBurstCount: uiProviderRequestSequence.length,
        firstProviderRequestAtMs: uiProviderRequestSequence[0]?.relativeStartedAtMs,
        lastProviderRequestCompletedAtMs:
            uiProviderRequestSequence[uiProviderRequestSequence.length - 1]?.relativeCompletedAtMs,
        requestCounters: {
            before: requestCounters.beforeCapture,
            afterUiTrigger: requestCounters.afterUiTrigger,
            afterQueueQuiet: requestCounters.afterUiQueueQuiet,
            uiTriggerDelta: requestCounters.uiTriggerDelta,
            totalTriggerDelta: requestCounters.uiQueueQuietDelta
        },
        queueQuiet
    };
    let completionResult: vscode.CompletionList | vscode.CompletionItem[] | undefined;
    let error: string | undefined;
    requestCounters.beforeProvider = await readRequestCounterSnapshot();
    const providerStartedAt = Date.now();
    try {
        completionResult = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[] | undefined>(
            'vscode.executeCompletionItemProvider',
            document.uri,
            position,
            triggerCharacter
        );
    } catch (captureError) {
        error = captureError instanceof Error ? captureError.message : String(captureError);
    }
    const executeProviderDurationMs = Date.now() - providerStartedAt;
    requestCounters.afterProvider = await readRequestCounterSnapshot();
    requestCounters.providerDelta = requestCounterDelta(
        requestCounters.beforeProvider,
        requestCounters.afterProvider
    );
    const providerVerification: ProviderVerificationReport = {
        measurementMode: 'providerVerification',
        durationMs: executeProviderDurationMs,
        requestCounters: {
            before: requestCounters.beforeProvider,
            after: requestCounters.afterProvider,
            delta: requestCounters.providerDelta
        },
        clientProviderTiming: providerTimingFromSnapshot(requestCounters.afterProvider, 'completion'),
        uiQueueQuietTimedOut: queueQuiet.timedOut
    };
    const items = getCompletionItems(completionResult);
    const labels = items
        .map((item) => completionLabelToString((item as unknown as { label?: unknown }).label))
        .filter((label) => label.length > 0);
    const topLabels = labels.slice(0, maxLabels);
    const expectedPresent = expectedLabels
        ? Object.fromEntries(expectedLabels.map((label) => [label, labels.includes(label)]))
        : undefined;

    let lastCompletionDebug: unknown | undefined;
    lastCompletionDebug = await readLastCompletionDebug();

    let directServerCompletion: CompletionCaptureReport['directServerCompletion'] | undefined;
    requestCounters.beforeDirectServer = await readRequestCounterSnapshot();
    const directStartedAt = Date.now();
    try {
        const directResult = await vscode.commands.executeCommand<any>('nsf._sendServerRequest', {
            method: 'textDocument/completion',
            params: {
                textDocument: { uri: document.uri.toString() },
                position: { line: position.line, character: position.character },
                context: triggerCharacter
                    ? { triggerKind: 2, triggerCharacter }
                    : { triggerKind: 1 }
            }
        });
        const directItems = getCompletionItems(directResult as vscode.CompletionList | vscode.CompletionItem[] | undefined);
        const directLabels = directItems
            .map((item) => completionLabelToString((item as unknown as { label?: unknown }).label))
            .filter((label) => label.length > 0);
        const directTopLabels = directLabels.slice(0, maxLabels);
        directServerCompletion = {
            durationMs: Date.now() - directStartedAt,
            triggerKind,
            itemCount: directLabels.length,
            topLabels: directTopLabels,
            expectedPresent: expectedLabels
                ? Object.fromEntries(expectedLabels.map((label) => [label, directLabels.includes(label)]))
                : undefined
        };
    } catch (captureError) {
        directServerCompletion = {
            durationMs: Date.now() - directStartedAt,
            triggerKind,
            itemCount: 0,
            topLabels: [],
            error: captureError instanceof Error ? captureError.message : String(captureError)
        };
    }
    requestCounters.afterDirectServer = await readRequestCounterSnapshot();
    requestCounters.directServerDelta = requestCounterDelta(
        requestCounters.beforeDirectServer,
        requestCounters.afterDirectServer
    );
    requestCounters.totalDelta = requestCounterDelta(
        requestCounters.beforeCapture,
        requestCounters.afterDirectServer ?? requestCounters.afterProvider
    );

    return {
        measurementMode: 'separated-ui-provider',
        uiCoverageTriggerSource,
        durationMs: Date.now() - captureStartedAt,
        executeProviderDurationMs,
        triggerKind,
        triggerCharacter,
        triggerText: trigger?.triggerText,
        triggerStartOffset: trigger?.triggerStartOffset,
        triggerEndOffset: trigger?.offset,
        itemCount: labels.length,
        topLabels,
        expectedLabels,
        expectedPresent,
        line: position.line,
        character: position.character,
        lineText,
        uiTrigger,
        uiCoverage,
        providerVerification,
        requestCounters,
        error,
        lastCompletionDebug,
        directServerCompletion
    };
}

async function captureSignatureHelpAtEditor(
    activeEditor: vscode.TextEditor,
    payload?: ReplayTypingProbe['payload'],
    trigger?: Pick<TypingProbePlan, 'triggerText' | 'triggerCharacter' | 'triggerStartOffset' | 'offset'> & {
        triggerTyping?: TriggerTypingReport;
    }
): Promise<SignatureHelpCaptureReport> {
    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
    const document = activeEditor.document;
    const position = activeEditor.selection.active;
    const lineText = getLineText(document, position.line);

    const triggerCharacter = typeof payload?.triggerCharacter === 'string'
        ? payload.triggerCharacter
        : trigger?.triggerCharacter;
    const retrigger = typeof payload?.retrigger === 'boolean' ? payload.retrigger : undefined;
    const expectedSubstrings = Array.isArray(payload?.expectedSubstrings)
        ? payload.expectedSubstrings.filter((entry): entry is string => typeof entry === 'string')
        : undefined;
    const maxSignatures = Math.max(1, Math.min(50, payload?.maxSignatures ?? 10));

    const captureStartedAt = Date.now();
    const requestCounters: RequestCounterTrace = {
        beforeCapture: await readRequestCounterSnapshot(),
        nativeTriggerDelta: trigger?.triggerTyping?.requestDelta
    };
    let uiTrigger: UiTriggerReport | undefined;
    let queueQuiet: ProviderQueueQuietReport | undefined;
    if (payload?.triggerParameterHintsUi === true) {
        requestCounters.beforeUiTrigger = await readRequestCounterSnapshot();
        uiTrigger = await runEditorUiTriggerCommand('editor.action.triggerParameterHints', payload);
        requestCounters.afterUiTrigger = await readRequestCounterSnapshot();
        requestCounters.uiTriggerDelta = requestCounterDelta(
            requestCounters.beforeUiTrigger,
            requestCounters.afterUiTrigger
        );
    }
    await closeEditorUiWidgets();
    queueQuiet = await waitForProviderQueueQuiet(
        'signatureHelp',
        requestCounters.beforeCapture
    );
    attachQueueQuietRelativeTimes(queueQuiet, captureStartedAt);
    requestCounters.afterUiQueueQuiet = queueQuiet.after;
    requestCounters.uiQueueQuietDelta = queueQuiet.requestDelta;
    const uiProviderRequestSequence = providerRequestSequence(
        'signatureHelp',
        requestCounters.beforeCapture,
        requestCounters.afterUiQueueQuiet,
        captureStartedAt
    );
    const uiCoverage: UiCoverageReport = {
        measurementMode: 'uiCoverage',
        nativeTriggerDelta: requestCounters.nativeTriggerDelta,
        uiTrigger,
        providerRequestSequence: uiProviderRequestSequence,
        requestBurstCount: uiProviderRequestSequence.length,
        firstProviderRequestAtMs: uiProviderRequestSequence[0]?.relativeStartedAtMs,
        lastProviderRequestCompletedAtMs:
            uiProviderRequestSequence[uiProviderRequestSequence.length - 1]?.relativeCompletedAtMs,
        requestCounters: {
            before: requestCounters.beforeCapture,
            afterUiTrigger: requestCounters.afterUiTrigger,
            afterQueueQuiet: requestCounters.afterUiQueueQuiet,
            uiTriggerDelta: requestCounters.uiTriggerDelta,
            totalTriggerDelta: requestCounters.uiQueueQuietDelta
        },
        queueQuiet
    };
    let signatureHelp: vscode.SignatureHelp | undefined;
    let error: string | undefined;
    requestCounters.beforeProvider = await readRequestCounterSnapshot();
    const providerStartedAt = Date.now();
    try {
        signatureHelp = await vscode.commands.executeCommand<vscode.SignatureHelp | undefined>(
            'vscode.executeSignatureHelpProvider',
            document.uri,
            position,
            triggerCharacter,
            retrigger
        );
    } catch (captureError) {
        error = captureError instanceof Error ? captureError.message : String(captureError);
    }
    const executeProviderDurationMs = Date.now() - providerStartedAt;
    requestCounters.afterProvider = await readRequestCounterSnapshot();
    requestCounters.providerDelta = requestCounterDelta(
        requestCounters.beforeProvider,
        requestCounters.afterProvider
    );
    const providerVerification: ProviderVerificationReport = {
        measurementMode: 'providerVerification',
        durationMs: executeProviderDurationMs,
        requestCounters: {
            before: requestCounters.beforeProvider,
            after: requestCounters.afterProvider,
            delta: requestCounters.providerDelta
        },
        clientProviderTiming: providerTimingFromSnapshot(requestCounters.afterProvider, 'signatureHelp'),
        uiQueueQuietTimedOut: queueQuiet.timedOut
    };
    requestCounters.totalDelta = requestCounterDelta(
        requestCounters.beforeCapture,
        requestCounters.afterProvider
    );
    const allLabels = (signatureHelp?.signatures ?? [])
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
        measurementMode: 'separated-ui-provider',
        durationMs: Date.now() - captureStartedAt,
        executeProviderDurationMs,
        triggerCharacter,
        triggerText: trigger?.triggerText,
        triggerStartOffset: trigger?.triggerStartOffset,
        triggerEndOffset: trigger?.offset,
        retrigger,
        signatureCount: allLabels.length,
        signatureLabels,
        activeSignature: signatureHelp?.activeSignature,
        activeParameter: signatureHelp?.activeParameter,
        expectedSubstrings,
        expectedMatched,
        line: position.line,
        character: position.character,
        lineText,
        uiTrigger,
        uiCoverage,
        providerVerification,
        requestCounters,
        error
    };
}

async function captureHoverAtEditor(
    activeEditor: vscode.TextEditor,
    payload?: Extract<ReplayStep, { kind: 'captureHover' }>['payload']
): Promise<HoverCaptureReport> {
    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
    const document = activeEditor.document;
    const position = activeEditor.selection.active;
    const lineText = getLineText(document, position.line);
    const expectedSubstrings = Array.isArray(payload?.expectedSubstrings)
        ? payload.expectedSubstrings.filter((entry): entry is string => typeof entry === 'string')
        : undefined;
    const maxContents = Math.max(1, Math.min(50, payload?.maxContents ?? 10));

    const startedAt = Date.now();
    let hovers: vscode.Hover[] | undefined;
    let error: string | undefined;
    try {
        hovers = await vscode.commands.executeCommand<vscode.Hover[] | undefined>(
            'vscode.executeHoverProvider',
            document.uri,
            position
        );
    } catch (captureError) {
        error = captureError instanceof Error ? captureError.message : String(captureError);
    }
    const topContents = hoverContentsToStrings(hovers, maxContents);
    const allText = topContents.join('\n');
    const expectedMatched = expectedSubstrings
        ? Object.fromEntries(expectedSubstrings.map((needle) => [needle, allText.includes(needle)]))
        : undefined;

    return {
        durationMs: Date.now() - startedAt,
        hoverCount: Array.isArray(hovers) ? hovers.length : 0,
        topContents,
        expectedSubstrings,
        expectedMatched,
        line: position.line,
        character: position.character,
        lineText,
        error
    };
}

async function captureLocationsAtEditor(
    activeEditor: vscode.TextEditor,
    command: 'vscode.executeDefinitionProvider' | 'vscode.executeReferenceProvider',
    payload?: Extract<ReplayStep, { kind: 'captureDefinition' | 'captureReferences' }>['payload']
): Promise<LocationCaptureReport> {
    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
    const document = activeEditor.document;
    const position = activeEditor.selection.active;
    const lineText = getLineText(document, position.line);
    const expectedUriSubstrings = Array.isArray(payload?.expectedUriSubstrings)
        ? payload.expectedUriSubstrings.filter((entry): entry is string => typeof entry === 'string')
        : undefined;
    const expectedTargetTexts = Array.isArray(payload?.expectedTargetTexts)
        ? payload.expectedTargetTexts.filter((entry): entry is string => typeof entry === 'string')
        : undefined;
    const expectedLineTextSubstrings = Array.isArray(payload?.expectedLineTextSubstrings)
        ? payload.expectedLineTextSubstrings.filter((entry): entry is string => typeof entry === 'string')
        : undefined;
    const expectedLocationMatches = Array.isArray(payload?.expectedLocationMatches)
        ? payload.expectedLocationMatches.filter((entry): entry is ReplayLocationExpectation => {
              if (!entry || typeof entry !== 'object') {
                  return false;
              }
              return (
                  typeof entry.uriSubstring === 'string' ||
                  typeof entry.targetText === 'string' ||
                  typeof entry.lineTextSubstring === 'string'
              );
          })
        : undefined;
    const minLocations = typeof payload?.minLocations === 'number' ? Math.max(0, payload.minLocations) : undefined;
    const maxLocations = typeof payload?.maxLocations === 'number' ? Math.max(0, payload.maxLocations) : undefined;
    const maxDurationMs = typeof payload?.maxDurationMs === 'number' ? Math.max(0, payload.maxDurationMs) : undefined;
    const maxTopLocations = Math.max(1, Math.min(200, maxLocations ?? 50));

    const startedAt = Date.now();
    let locations: unknown[] | undefined;
    let error: string | undefined;
    try {
        locations = await vscode.commands.executeCommand<unknown[] | undefined>(
            command,
            document.uri,
            position
        );
    } catch (captureError) {
        error = captureError instanceof Error ? captureError.message : String(captureError);
    }
    const durationMs = Date.now() - startedAt;
    const allLocations = Array.isArray(locations) ? locations.map(locationToString).filter((item) => item.length > 0) : [];
    const topLocations = allLocations.slice(0, maxTopLocations);
    const topLocationDetails = await locationDetails(locations, maxTopLocations);
    const expectedMatched = expectedUriSubstrings
        ? Object.fromEntries(
              expectedUriSubstrings.map((needle) => [
                  needle,
                  allLocations.some((location) => location.toLowerCase().includes(needle.toLowerCase()))
              ])
          )
        : undefined;
    const expectedTargetTextMatched = expectedTargetTexts
        ? Object.fromEntries(
              expectedTargetTexts.map((needle) => [
                  needle,
                  topLocationDetails.some((detail) => detail.targetText === needle)
              ])
          )
        : undefined;
    const expectedLineTextMatched = expectedLineTextSubstrings
        ? Object.fromEntries(
              expectedLineTextSubstrings.map((needle) => [
                  needle,
                  topLocationDetails.some((detail) => detail.lineText?.includes(needle) === true)
              ])
          )
        : undefined;
    const expectedLocationMatched = expectedLocationMatches
        ? Object.fromEntries(
              expectedLocationMatches.map((expectation, index) => [
                  locationExpectationKey(expectation, index),
                  topLocationDetails.some((detail) => locationDetailMatchesExpectation(detail, expectation))
              ])
          )
        : undefined;

    return {
        durationMs,
        locationCount: allLocations.length,
        topLocations,
        topLocationDetails,
        expectedUriSubstrings,
        expectedMatched,
        expectedTargetTexts,
        expectedTargetTextMatched,
        expectedLineTextSubstrings,
        expectedLineTextMatched,
        expectedLocationMatches,
        expectedLocationMatched,
        minLocations,
        maxLocations,
        maxDurationMs,
        withinExpectedMin: minLocations === undefined ? undefined : allLocations.length >= minLocations,
        withinExpectedMax: maxLocations === undefined ? undefined : allLocations.length <= maxLocations,
        withinExpectedDuration: maxDurationMs === undefined ? undefined : durationMs <= maxDurationMs,
        line: position.line,
        character: position.character,
        lineText,
        error
    };
}

async function captureDocumentSymbolsAtEditor(
    activeEditor: vscode.TextEditor,
    payload?: Extract<ReplayStep, { kind: 'captureDocumentSymbols' }>['payload']
): Promise<DocumentSymbolCaptureReport> {
    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
    const document = activeEditor.document;
    const expectedNames = Array.isArray(payload?.expectedNames)
        ? payload.expectedNames.filter((entry): entry is string => typeof entry === 'string')
        : undefined;
    const maxNames = Math.max(1, Math.min(200, payload?.maxNames ?? 80));
    const waitForReadyMs = Math.max(0, Math.min(30000, payload?.waitForReadyMs ?? 3000));

    const startedAt = Date.now();
    let symbols: Array<vscode.DocumentSymbol | vscode.SymbolInformation> | undefined;
    let error: string | undefined;
    let names: string[] = [];
    const deadline = Date.now() + waitForReadyMs;
    do {
        try {
            symbols = await vscode.commands.executeCommand<Array<vscode.DocumentSymbol | vscode.SymbolInformation> | undefined>(
                'vscode.executeDocumentSymbolProvider',
                document.uri
            );
            error = undefined;
        } catch (captureError) {
            error = captureError instanceof Error ? captureError.message : String(captureError);
            symbols = undefined;
        }
        names = flattenDocumentSymbolNames(symbols);
        const expectedReady = expectedNames === undefined || expectedNames.every((name) => names.includes(name));
        if (expectedReady && (expectedNames !== undefined || names.length > 0 || waitForReadyMs === 0)) {
            break;
        }
        if (Date.now() >= deadline) {
            break;
        }
        await delay(100);
    } while (true);
    const topNames = names.slice(0, maxNames);
    const expectedPresent = expectedNames
        ? Object.fromEntries(expectedNames.map((name) => [name, names.includes(name)]))
        : undefined;

    return {
        durationMs: Date.now() - startedAt,
        symbolCount: names.length,
        topNames,
        expectedNames,
        expectedPresent,
        error
    };
}

async function captureInlayHintsAtEditor(
    activeEditor: vscode.TextEditor,
    payload?: Extract<ReplayStep, { kind: 'captureInlayHints' }>['payload']
): Promise<InlayHintsCaptureReport> {
    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
    const document = activeEditor.document;
    const position = activeEditor.selection.active;
    const before = Math.max(0, Math.min(200, payload?.lineDeltaBefore ?? 0));
    const after = Math.max(0, Math.min(200, payload?.lineDeltaAfter ?? 0));
    const startLine = Math.max(0, position.line - before);
    const endLine = Math.min(document.lineCount - 1, position.line + after);
    const endCharacter = getLineText(document, endLine).length;
    const range = new vscode.Range(
        new vscode.Position(startLine, 0),
        new vscode.Position(endLine, endCharacter)
    );
    const expectedLabels = Array.isArray(payload?.expectedLabels)
        ? payload.expectedLabels.filter((entry): entry is string => typeof entry === 'string')
        : undefined;
    const minHints = typeof payload?.minHints === 'number' ? Math.max(0, payload.minHints) : undefined;
    const maxHints = typeof payload?.maxHints === 'number' ? Math.max(0, payload.maxHints) : undefined;
    const maxTopHints = Math.max(1, Math.min(200, maxHints ?? 80));

    const startedAt = Date.now();
    let hints: vscode.InlayHint[] | undefined;
    let error: string | undefined;
    try {
        hints = await vscode.commands.executeCommand<vscode.InlayHint[] | undefined>(
            'vscode.executeInlayHintProvider',
            document.uri,
            range
        );
    } catch (captureError) {
        error = captureError instanceof Error ? captureError.message : String(captureError);
    }
    const labels = (hints ?? []).map((hint) => inlayHintLabelToString(hint.label)).filter((label) => label.length > 0);
    const topLabels = labels.slice(0, maxTopHints);
    const expectedPresent = expectedLabels
        ? Object.fromEntries(expectedLabels.map((label) => [label, labels.includes(label)]))
        : undefined;

    return {
        durationMs: Date.now() - startedAt,
        hintCount: Array.isArray(hints) ? hints.length : 0,
        rangeStartLine: startLine,
        rangeEndLine: endLine,
        topLabels,
        expectedLabels,
        expectedPresent,
        minHints,
        maxHints,
        withinExpectedMin: minHints === undefined ? undefined : (hints?.length ?? 0) >= minHints,
        withinExpectedMax: maxHints === undefined ? undefined : (hints?.length ?? 0) <= maxHints,
        error
    };
}

async function capturePrepareRenameAtEditor(
    activeEditor: vscode.TextEditor,
    payload?: Extract<ReplayStep, { kind: 'capturePrepareRename' }>['payload']
): Promise<PrepareRenameCaptureReport> {
    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
    const document = activeEditor.document;
    const position = activeEditor.selection.active;
    const lineText = getLineText(document, position.line);
    const expectedPlaceholder = typeof payload?.expectedPlaceholder === 'string'
        ? payload.expectedPlaceholder
        : undefined;

    const startedAt = Date.now();
    let result: unknown;
    let error: string | undefined;
    try {
        result = await vscode.commands.executeCommand<unknown>(
            'vscode.prepareRename',
            document.uri,
            position
        );
    } catch (captureError) {
        error = captureError instanceof Error ? captureError.message : String(captureError);
    }
    const placeholder = typeof (result as { placeholder?: unknown } | undefined)?.placeholder === 'string'
        ? (result as { placeholder: string }).placeholder
        : undefined;

    return {
        durationMs: Date.now() - startedAt,
        placeholder,
        expectedPlaceholder,
        placeholderMatched: expectedPlaceholder === undefined ? undefined : placeholder === expectedPlaceholder,
        line: position.line,
        character: position.character,
        lineText,
        error
    };
}

async function captureRenameEditAtEditor(
    activeEditor: vscode.TextEditor,
    payload: Extract<ReplayStep, { kind: 'captureRenameEdit' }>['payload']
): Promise<RenameEditCaptureReport> {
    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
    const document = activeEditor.document;
    const position = activeEditor.selection.active;
    const lineText = getLineText(document, position.line);
    const newName = String(payload.newName ?? '').trim();
    const minChanges = typeof payload.minChanges === 'number' ? Math.max(0, payload.minChanges) : undefined;
    const maxChanges = typeof payload.maxChanges === 'number' ? Math.max(0, payload.maxChanges) : undefined;

    const startedAt = Date.now();
    let changeCount = 0;
    let error: string | undefined;
    try {
        const edit = await vscode.commands.executeCommand<vscode.WorkspaceEdit | undefined>(
            'vscode.executeDocumentRenameProvider',
            document.uri,
            position,
            newName
        );
        changeCount = edit ? countWorkspaceEdits(edit) : 0;
    } catch (captureError) {
        error = captureError instanceof Error ? captureError.message : String(captureError);
    }

    return {
        durationMs: Date.now() - startedAt,
        newName,
        changeCount,
        minChanges,
        maxChanges,
        withinExpectedMin: minChanges === undefined ? undefined : changeCount >= minChanges,
        withinExpectedMax: maxChanges === undefined ? undefined : changeCount <= maxChanges,
        line: position.line,
        character: position.character,
        lineText,
        error
    };
}

async function captureSemanticTokensAtEditor(
    activeEditor: vscode.TextEditor,
    payload?: Extract<ReplayStep, { kind: 'captureSemanticTokens' }>['payload']
): Promise<SemanticTokensCaptureReport> {
    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
    const document = activeEditor.document;
    const minDataLength = typeof payload?.minDataLength === 'number' ? Math.max(0, payload.minDataLength) : undefined;
    const maxDataLength = typeof payload?.maxDataLength === 'number' ? Math.max(0, payload.maxDataLength) : undefined;
    const waitForReadyMs = Math.max(0, Math.min(30000, payload?.waitForReadyMs ?? 3000));

    const startedAt = Date.now();
    let dataLength = 0;
    let error: string | undefined;
    const deadline = Date.now() + waitForReadyMs;
    do {
        try {
            const tokens = await vscode.commands.executeCommand<vscode.SemanticTokens | undefined>(
                'vscode.provideDocumentSemanticTokens',
                document.uri
            );
            dataLength = tokens?.data?.length ?? 0;
            error = undefined;
        } catch (captureError) {
            error = captureError instanceof Error ? captureError.message : String(captureError);
            dataLength = 0;
        }
        if ((minDataLength === undefined || dataLength >= minDataLength) && (dataLength > 0 || waitForReadyMs === 0)) {
            break;
        }
        if (Date.now() >= deadline) {
            break;
        }
        await delay(100);
    } while (true);

    return {
        durationMs: Date.now() - startedAt,
        dataLength,
        minDataLength,
        maxDataLength,
        withinExpectedMin: minDataLength === undefined ? undefined : dataLength >= minDataLength,
        withinExpectedMax: maxDataLength === undefined ? undefined : dataLength <= maxDataLength,
        error
    };
}

function createProbeStep(probe: ReplayTypingProbe): ReplayStep {
    return {
        kind: 'typeText',
        label: probe.label,
        payload: { text: '' },
        samplingWindow: probe.samplingWindow
    };
}

async function sampleTypingProbe(
    activeEditor: vscode.TextEditor,
    plan: TypingProbePlan,
    sourceText: string,
    triggerTyping?: TriggerTypingReport
): Promise<FullDocumentTypingProbeReport> {
    const probe = plan.probe;
    const baselineInternalStatus = probe.samplingWindow
        ? await vscode.commands.executeCommand<any>('nsf._getInternalStatus')
        : undefined;
    let completionPromise: Promise<CompletionCaptureReport> | undefined;
    let signaturePromise: Promise<SignatureHelpCaptureReport> | undefined;
    let diagnosticsPromise: Promise<DiagnosticsCaptureReport> | undefined;
    const triggerContext = {
        ...plan,
        triggerTyping
    };
    if (probe.kind === 'completion') {
        completionPromise = captureCompletionAtEditor(activeEditor, probe.payload, triggerContext);
    } else if (probe.kind === 'signatureHelp') {
        signaturePromise = captureSignatureHelpAtEditor(activeEditor, probe.payload, triggerContext);
    } else {
        diagnosticsPromise = captureDiagnosticsAtEditor(activeEditor, probe.payload);
    }
    const samples = await sampleReplayWindow(
        createProbeStep(probe),
        activeEditor.document.uri.toString(),
        baselineInternalStatus
    );
    return {
        label: probe.label,
        category: probe.category,
        kind: probe.kind,
        offset: plan.offset,
        line: lineAtOffset(sourceText, plan.offset),
        triggerText: plan.triggerText,
        triggerCharacter: plan.triggerCharacter,
        triggerStartOffset: plan.triggerStartOffset,
        triggerEndOffset: plan.offset,
        nativeTrigger: plan.nativeTrigger,
        triggerTyping,
        samples,
        completionCapture: completionPromise ? await completionPromise : undefined,
        signatureHelpCapture: signaturePromise ? await signaturePromise : undefined,
        diagnosticsCapture: diagnosticsPromise ? await diagnosticsPromise : undefined
    };
}

async function replaceDocumentText(
    document: vscode.TextDocument,
    text: string
): Promise<void> {
    const fullRange = new vscode.Range(
        document.positionAt(0),
        document.positionAt(document.getText().length)
    );
    const edit = new vscode.WorkspaceEdit();
    edit.replace(document.uri, fullRange, text);
    await vscode.workspace.applyEdit(edit);
}

async function restoreTypingCursorToDocumentEnd(document: vscode.TextDocument): Promise<vscode.TextEditor> {
    const editor = await vscode.window.showTextDocument(document, { preview: false });
    const position = document.positionAt(document.getText().length);
    editor.selection = new vscode.Selection(position, position);
    return editor;
}

function createTypingProbePlans(
    sourceText: string,
    rawProbes: ReplayTypingProbe[],
    defaults?: Extract<ReplayStep, { kind: 'typeDocumentFromDisk' }>['payload']
): TypingProbePlan[] {
    const defaultProbePayload: ReplayTypingProbe['payload'] = {
        triggerSuggestUi: defaults?.triggerSuggestUi,
        completionUiMode: defaults?.completionUiMode,
        triggerParameterHintsUi: defaults?.triggerParameterHintsUi,
        uiTriggerDelayMs: defaults?.uiTriggerDelayMs
    };
    return rawProbes
        .map((rawProbe) => {
            const probe: ReplayTypingProbe = {
                ...rawProbe,
                payload: {
                    ...defaultProbePayload,
                    ...(rawProbe.payload ?? {})
                }
            };
            const offset = findNthEndOffset(sourceText, probe.afterText, probe.occurrence ?? 1);
            const triggerText = typeof probe.triggerText === 'string' && probe.triggerText.length > 0
                ? probe.triggerText
                : undefined;
            const triggerCharacter = typeof probe.payload?.triggerCharacter === 'string'
                ? probe.payload.triggerCharacter
                : lastCharacter(triggerText);
            const nativeTrigger = typeof rawProbe.payload?.nativeTrigger === 'boolean'
                ? rawProbe.payload.nativeTrigger
                : (defaults?.nativeTrigger === true && probe.kind === 'completion');
            let triggerStartOffset = offset;
            let triggerError: string | undefined;

            if (offset < 0) {
                triggerError = `Probe text not found: ${probe.afterText}`;
            } else if (triggerText) {
                triggerStartOffset = offset - triggerText.length;
                if (triggerStartOffset < 0 || sourceText.slice(triggerStartOffset, offset) !== triggerText) {
                    triggerError = `Probe triggerText must be the suffix of afterText: ${triggerText}`;
                }
            } else if (probe.kind === 'completion') {
                triggerError = 'Completion probe must declare triggerText so the replay can type the trigger before capture.';
            }

            return {
                probe,
                offset,
                triggerText,
                triggerCharacter,
                triggerStartOffset,
                nativeTrigger,
                triggerError
            };
        })
        .sort((lhs, rhs) => {
            const lhsSortOffset = lhs.triggerStartOffset < 0 ? Number.MAX_SAFE_INTEGER : lhs.triggerStartOffset;
            const rhsSortOffset = rhs.triggerStartOffset < 0 ? Number.MAX_SAFE_INTEGER : rhs.triggerStartOffset;
            return lhsSortOffset - rhsSortOffset || lhs.offset - rhs.offset;
        });
}

function collectFullDocumentTypingAnomalies(report: FullDocumentTypingReport): string[] {
    const anomalies: string[] = [];
    if (!report.finalTextMatchesSource) {
        anomalies.push('typed-document-mismatch');
    }
    if (report.inlayContinuity?.transientDropDetected) {
        anomalies.push('inlay-hints-transient-drop');
    }
    if (report.inlayContinuity?.endedMissingAfterVisible) {
        anomalies.push('inlay-hints-ended-missing-after-visible');
    }
    for (const probe of report.probes) {
        const label = probe.label.replace(/\s+/g, '-').toLowerCase();
        if (probe.error) {
            anomalies.push(`typing-probe-error:${label}`);
        }
        if (probe.kind === 'completion') {
            if (!probe.triggerText) {
                anomalies.push(`completion-trigger-text-missing:${label}`);
            }
            if (probe.completionCapture?.triggerKind !== 'TriggerCharacter') {
                anomalies.push(`completion-trigger-character-not-used:${label}`);
            }
            for (const missing of expectedMissing(probe.completionCapture?.expectedPresent)) {
                anomalies.push(`completion-expected-missing:${label}:${missing}`);
            }
            if (probe.completionCapture?.error) {
                anomalies.push(`completion-capture-error:${label}`);
            }
            if (probe.completionCapture?.uiTrigger?.error) {
                anomalies.push(`completion-ui-trigger-error:${label}`);
            }
        }
        if (probe.kind === 'signatureHelp') {
            for (const missing of expectedMissing(probe.signatureHelpCapture?.expectedMatched)) {
                anomalies.push(`signature-expected-missing:${label}:${missing}`);
            }
            if (probe.signatureHelpCapture?.error) {
                anomalies.push(`signature-capture-error:${label}`);
            }
            if (probe.signatureHelpCapture?.uiTrigger?.error) {
                anomalies.push(`signature-ui-trigger-error:${label}`);
            }
        }
        if (probe.kind === 'diagnostics') {
            if (probe.diagnosticsCapture?.withinExpectedDiagnostics === false) {
                anomalies.push(`diagnostics-count-out-of-range:${label}`);
            }
            if (probe.diagnosticsCapture?.withinExpectedErrors === false) {
                anomalies.push(`diagnostics-errors-out-of-range:${label}`);
            }
        }
    }
    return anomalies;
}

function collectTopLevelCaptureAnomalies(report: ReplayStepReport): string[] {
    const anomalies: string[] = [];
    const label = report.stepLabel.replace(/\s+/g, '-').toLowerCase();
    for (const missing of expectedMissing(report.completionCapture?.expectedPresent)) {
        anomalies.push(`completion-expected-missing:${label}:${missing}`);
    }
    if (report.completionCapture?.error) {
        anomalies.push(`completion-capture-error:${label}`);
    }
    if (report.completionCapture?.uiTrigger?.error) {
        anomalies.push(`completion-ui-trigger-error:${label}`);
    }
    for (const missing of expectedMissing(report.signatureHelpCapture?.expectedMatched)) {
        anomalies.push(`signature-expected-missing:${label}:${missing}`);
    }
    if (report.signatureHelpCapture?.error) {
        anomalies.push(`signature-capture-error:${label}`);
    }
    if (report.signatureHelpCapture?.uiTrigger?.error) {
        anomalies.push(`signature-ui-trigger-error:${label}`);
    }
    for (const missing of expectedMissing(report.hoverCapture?.expectedMatched)) {
        anomalies.push(`hover-expected-missing:${label}:${missing}`);
    }
    if (report.hoverCapture?.error) {
        anomalies.push(`hover-capture-error:${label}`);
    }
    for (const missing of expectedMissing(report.definitionCapture?.expectedMatched)) {
        anomalies.push(`definition-expected-missing:${label}:${missing}`);
    }
    for (const missing of expectedMissing(report.definitionCapture?.expectedTargetTextMatched)) {
        anomalies.push(`definition-target-text-missing:${label}:${missing}`);
    }
    for (const missing of expectedMissing(report.definitionCapture?.expectedLineTextMatched)) {
        anomalies.push(`definition-line-text-missing:${label}:${missing}`);
    }
    for (const missing of expectedMissing(report.definitionCapture?.expectedLocationMatched)) {
        anomalies.push(`definition-location-match-missing:${label}:${missing}`);
    }
    if (report.definitionCapture?.withinExpectedMin === false) {
        anomalies.push(`definition-count-below-min:${label}`);
    }
    if (report.definitionCapture?.withinExpectedMax === false) {
        anomalies.push(`definition-count-above-max:${label}`);
    }
    if (report.definitionCapture?.withinExpectedDuration === false) {
        anomalies.push(`definition-duration-above-max:${label}`);
    }
    if (report.definitionCapture?.error) {
        anomalies.push(`definition-capture-error:${label}`);
    }
    for (const missing of expectedMissing(report.referencesCapture?.expectedMatched)) {
        anomalies.push(`references-expected-missing:${label}:${missing}`);
    }
    for (const missing of expectedMissing(report.referencesCapture?.expectedTargetTextMatched)) {
        anomalies.push(`references-target-text-missing:${label}:${missing}`);
    }
    for (const missing of expectedMissing(report.referencesCapture?.expectedLineTextMatched)) {
        anomalies.push(`references-line-text-missing:${label}:${missing}`);
    }
    for (const missing of expectedMissing(report.referencesCapture?.expectedLocationMatched)) {
        anomalies.push(`references-location-match-missing:${label}:${missing}`);
    }
    if (report.referencesCapture?.withinExpectedMin === false) {
        anomalies.push(`references-count-below-min:${label}`);
    }
    if (report.referencesCapture?.withinExpectedMax === false) {
        anomalies.push(`references-count-above-max:${label}`);
    }
    if (report.referencesCapture?.withinExpectedDuration === false) {
        anomalies.push(`references-duration-above-max:${label}`);
    }
    if (report.referencesCapture?.error) {
        anomalies.push(`references-capture-error:${label}`);
    }
    for (const missing of expectedMissing(report.documentSymbolCapture?.expectedPresent)) {
        anomalies.push(`document-symbol-expected-missing:${label}:${missing}`);
    }
    if (report.documentSymbolCapture?.error) {
        anomalies.push(`document-symbol-capture-error:${label}`);
    }
    for (const missing of expectedMissing(report.inlayHintsCapture?.expectedPresent)) {
        anomalies.push(`inlay-expected-missing:${label}:${missing}`);
    }
    if (report.inlayHintsCapture?.withinExpectedMin === false) {
        anomalies.push(`inlay-count-below-min:${label}`);
    }
    if (report.inlayHintsCapture?.withinExpectedMax === false) {
        anomalies.push(`inlay-count-above-max:${label}`);
    }
    if (report.inlayHintsCapture?.error) {
        anomalies.push(`inlay-capture-error:${label}`);
    }
    if (report.prepareRenameCapture?.placeholderMatched === false) {
        anomalies.push(`prepare-rename-placeholder-mismatch:${label}`);
    }
    if (report.prepareRenameCapture?.error) {
        anomalies.push(`prepare-rename-capture-error:${label}`);
    }
    if (report.renameEditCapture?.withinExpectedMin === false) {
        anomalies.push(`rename-edit-count-below-min:${label}`);
    }
    if (report.renameEditCapture?.withinExpectedMax === false) {
        anomalies.push(`rename-edit-count-above-max:${label}`);
    }
    if (report.renameEditCapture?.error) {
        anomalies.push(`rename-edit-capture-error:${label}`);
    }
    if (report.semanticTokensCapture?.withinExpectedMin === false) {
        anomalies.push(`semantic-tokens-count-below-min:${label}`);
    }
    if (report.semanticTokensCapture?.withinExpectedMax === false) {
        anomalies.push(`semantic-tokens-count-above-max:${label}`);
    }
    if (report.semanticTokensCapture?.error) {
        anomalies.push(`semantic-tokens-capture-error:${label}`);
    }
    for (const missing of expectedMissing(report.workspaceSymbolCapture?.expectedPresent)) {
        anomalies.push(`workspace-symbol-expected-missing:${label}:${missing}`);
    }
    if (report.workspaceSymbolCapture?.error) {
        anomalies.push(`workspace-symbol-capture-error:${label}`);
    }
    return anomalies;
}

async function runFullDocumentTypingStep(
    activeEditor: vscode.TextEditor,
    step: Extract<ReplayStep, { kind: 'typeDocumentFromDisk' }>
): Promise<FullDocumentTypingReport> {
    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
    const document = activeEditor.document;
    const sourceText = document.getText();
    const sourceLineCount = countLines(sourceText);
    const sourceCharacterCount = sourceText.length;
    const charactersPerEdit = clampInt(step.payload?.charactersPerEdit, 24, 1, 512);
    const checkpointEveryLines = clampInt(step.payload?.checkpointEveryLines, 40, 1, 10000);
    const checkpointSamplingDelaysMs = step.payload?.checkpointSamplingDelaysMs ?? [0, 80, 240, 640];
    const captureInlayContinuity = step.payload?.captureInlayContinuity === true;
    const rawProbes = Array.isArray(step.payload?.probes) ? step.payload?.probes : [];
    const probePlans = createTypingProbePlans(sourceText, rawProbes, step.payload);

    await replaceDocumentText(document, '');
    activeEditor.selection = new vscode.Selection(new vscode.Position(0, 0), new vscode.Position(0, 0));

    const startedAt = Date.now();
    const checkpoints: FullDocumentTypingCheckpointReport[] = [];
    const probes: FullDocumentTypingProbeReport[] = [];
    const inlayContinuitySamples: InlayContinuitySampleReport[] = [];
    let typedOffset = 0;
    let nextCheckpointLine = checkpointEveryLines;
    let probeIndex = 0;

    for (const plan of probePlans) {
        if (plan.triggerError) {
            probes.push({
                label: plan.probe.label,
                category: plan.probe.category,
                kind: plan.probe.kind,
                offset: plan.offset,
                line: lineAtOffset(sourceText, plan.offset),
                triggerText: plan.triggerText,
                triggerCharacter: plan.triggerCharacter,
                triggerStartOffset: plan.triggerStartOffset,
                triggerEndOffset: plan.offset,
                nativeTrigger: plan.nativeTrigger,
                samples: [],
                error: plan.triggerError
            });
        }
    }

    while (typedOffset < sourceText.length) {
        while (probeIndex < probePlans.length && probePlans[probeIndex].triggerError) {
            probeIndex++;
        }
        while (
            probeIndex < probePlans.length &&
            !probePlans[probeIndex].triggerError &&
            probePlans[probeIndex].triggerStartOffset < typedOffset &&
            probePlans[probeIndex].offset !== typedOffset
        ) {
            const plan = probePlans[probeIndex];
            probes.push({
                label: plan.probe.label,
                category: plan.probe.category,
                kind: plan.probe.kind,
                offset: plan.offset,
                line: lineAtOffset(sourceText, plan.offset),
                triggerText: plan.triggerText,
                triggerCharacter: plan.triggerCharacter,
                triggerStartOffset: plan.triggerStartOffset,
                triggerEndOffset: plan.offset,
                nativeTrigger: plan.nativeTrigger,
                samples: [],
                error: `Probe trigger was skipped before capture. typedOffset=${typedOffset}`
            });
            probeIndex++;
        }

        const nextProbeOffset = probeIndex < probePlans.length
            ? probePlans[probeIndex].triggerStartOffset
            : sourceText.length;
        const nextOffset = Math.min(sourceText.length, typedOffset + charactersPerEdit, nextProbeOffset);
        if (nextOffset > typedOffset) {
            await typeTextForTests(activeEditor, sourceText.slice(typedOffset, nextOffset));
            typedOffset = nextOffset;
            activeEditor = await restoreTypingCursorToDocumentEnd(document);
        }

        while (probeIndex < probePlans.length) {
            const plan = probePlans[probeIndex];
            let triggerTyping: TriggerTypingReport | undefined;
            if (plan.triggerError) {
                probeIndex++;
                continue;
            }
            if (plan.triggerStartOffset === typedOffset && plan.triggerText) {
                const triggerBefore = await readRequestCounterSnapshot();
                const triggerStartedAt = Date.now();
                for (const ch of plan.triggerText) {
                    if (plan.nativeTrigger) {
                        await typeWithEditorFocusForTests(activeEditor, ch);
                    } else {
                        await typeTextForTests(activeEditor, ch);
                        activeEditor = await restoreTypingCursorToDocumentEnd(document);
                    }
                    await delay(0);
                }
                const triggerDurationMs = Date.now() - triggerStartedAt;
                if (plan.nativeTrigger) {
                    activeEditor = vscode.window.activeTextEditor ?? activeEditor;
                }
                const triggerAfter = await readRequestCounterSnapshot();
                triggerTyping = {
                    durationMs: triggerDurationMs,
                    typedText: plan.triggerText,
                    nativeTrigger: plan.nativeTrigger,
                    before: triggerBefore,
                    after: triggerAfter,
                    requestDelta: requestCounterDelta(triggerBefore, triggerAfter),
                    providerRequestSequence:
                        plan.probe.kind === 'completion' || plan.probe.kind === 'signatureHelp'
                            ? providerRequestSequence(
                                plan.probe.kind,
                                triggerBefore,
                                triggerAfter,
                                triggerStartedAt
                            )
                            : undefined
                };
                typedOffset = plan.offset;
            } else if (plan.offset !== typedOffset) {
                break;
            }
            probes.push(await sampleTypingProbe(activeEditor, plan, sourceText, triggerTyping));
            await closeEditorUiWidgets();
            activeEditor = await restoreTypingCursorToDocumentEnd(document);
            probeIndex++;
        }

        const currentLine = lineAtOffset(sourceText, typedOffset);
        if (currentLine + 1 >= nextCheckpointLine || typedOffset >= sourceText.length) {
            const baselineInternalStatus = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
            const checkpointStep: ReplayStep = {
                kind: 'typeText',
                label: `${step.label} checkpoint line ${currentLine + 1}`,
                payload: { text: '' },
                samplingWindow: {
                    label: `full-file-line-${currentLine + 1}`,
                    delaysMs: checkpointSamplingDelaysMs,
                    captureRuntimeDebug: true,
                    captureInteractiveDebug: true
                }
            };
            const samples = await sampleReplayWindow(
                checkpointStep,
                activeEditor.document.uri.toString(),
                baselineInternalStatus
            );
            const inlayContinuitySample = captureInlayContinuity
                ? await captureInlayContinuityAtEditor(
                    activeEditor,
                    checkpointStep.label,
                    typedOffset,
                    currentLine
                )
                : undefined;
            if (inlayContinuitySample) {
                inlayContinuitySamples.push(inlayContinuitySample);
            }
            checkpoints.push({
                label: checkpointStep.label,
                offset: typedOffset,
                line: currentLine,
                samples,
                diagnostics: await captureDiagnosticsAtEditor(activeEditor),
                inlayContinuitySample
            });
            activeEditor = await restoreTypingCursorToDocumentEnd(document);
            while (currentLine + 1 >= nextCheckpointLine) {
                nextCheckpointLine += checkpointEveryLines;
            }
        }
    }

    const finalText = document.getText();
    const finalTextMatchesSource = finalText === sourceText;
    let firstMismatchOffset: number | undefined;
    let sourceMismatchSnippet: string | undefined;
    let finalMismatchSnippet: string | undefined;
    if (!finalTextMatchesSource) {
        const maxLength = Math.max(sourceText.length, finalText.length);
        for (let index = 0; index < maxLength; index++) {
            if (sourceText[index] !== finalText[index]) {
                firstMismatchOffset = index;
                break;
            }
        }
        const snippetStart = Math.max(0, (firstMismatchOffset ?? 0) - 40);
        const snippetEnd = Math.min(maxLength, (firstMismatchOffset ?? 0) + 80);
        sourceMismatchSnippet = sourceText.slice(snippetStart, snippetEnd);
        finalMismatchSnippet = finalText.slice(snippetStart, snippetEnd);
    }

    const report: FullDocumentTypingReport = {
        durationMs: Date.now() - startedAt,
        sourceLineCount,
        sourceCharacterCount,
        finalCharacterCount: finalText.length,
        finalTextMatchesSource,
        firstMismatchOffset,
        sourceMismatchSnippet,
        finalMismatchSnippet,
        charactersPerEdit,
        checkpointEveryLines,
        checkpointCount: checkpoints.length,
        probeCount: probes.length,
        inlayContinuity: captureInlayContinuity
            ? summarizeInlayContinuity(inlayContinuitySamples)
            : undefined,
        anomalies: [],
        checkpoints,
        probes
    };
    report.anomalies = collectFullDocumentTypingAnomalies(report);
    return report;
}

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
            const samplingDelays = resolveReplaySamplingDelays(step.samplingWindow);
            let baselineInternalStatus: unknown | undefined;
            if (samplingDelays.length > 0) {
                baselineInternalStatus = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
            }

            let completionCapture: CompletionCaptureReport | undefined;
            let signatureHelpCapture: SignatureHelpCaptureReport | undefined;
            let hoverCapture: HoverCaptureReport | undefined;
            let definitionCapture: LocationCaptureReport | undefined;
            let referencesCapture: LocationCaptureReport | undefined;
            let documentSymbolCapture: DocumentSymbolCaptureReport | undefined;
            let inlayHintsCapture: InlayHintsCaptureReport | undefined;
            let prepareRenameCapture: PrepareRenameCaptureReport | undefined;
            let renameEditCapture: RenameEditCaptureReport | undefined;
            let semanticTokensCapture: SemanticTokensCaptureReport | undefined;
            let workspaceSymbolCapture: WorkspaceSymbolCaptureReport | undefined;
            let fullDocumentTyping: FullDocumentTypingReport | undefined;
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
                case 'setActiveUnit': {
                    const resolved = await resolveReplayAnchor(step.target);
                    await vscode.commands.executeCommand('nsf._setActiveUnitForTests', resolved.uri.toString());
                    activeEditor = vscode.window.activeTextEditor ?? activeEditor;
                    recordDocumentSnapshot(activeEditor?.document);
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
                case 'typeDocumentFromDisk': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] typeDocumentFromDisk requires an active editor. Step: ${step.label}`);
                    }
                    activeEditor = await vscode.window.showTextDocument(activeEditor.document, { preview: false });
                    recordDocumentSnapshot(activeEditor.document);
                    fullDocumentTyping = await runFullDocumentTypingStep(activeEditor, step);
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

                    // Do not await here: sample windows should run while the provider is in-flight.
                    completionCapturePromise = captureCompletionAtEditor(activeEditor, step.payload);
                    break;
                }
                case 'captureSignatureHelp': {
                    if (!activeEditor) {
                        throw new Error(
                            `[real-replay] captureSignatureHelp requires an active editor. Step: ${step.label}`
                        );
                    }

                    // Do not await here: sample windows should run while the provider is in-flight.
                    signatureHelpCapturePromise = captureSignatureHelpAtEditor(activeEditor, step.payload);
                    break;
                }
                case 'captureHover': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] captureHover requires an active editor. Step: ${step.label}`);
                    }
                    hoverCapture = await captureHoverAtEditor(activeEditor, step.payload);
                    break;
                }
                case 'captureDefinition': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] captureDefinition requires an active editor. Step: ${step.label}`);
                    }
                    definitionCapture = await captureLocationsAtEditor(
                        activeEditor,
                        'vscode.executeDefinitionProvider',
                        step.payload
                    );
                    break;
                }
                case 'captureReferences': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] captureReferences requires an active editor. Step: ${step.label}`);
                    }
                    referencesCapture = await captureLocationsAtEditor(
                        activeEditor,
                        'vscode.executeReferenceProvider',
                        step.payload
                    );
                    break;
                }
                case 'captureDocumentSymbols': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] captureDocumentSymbols requires an active editor. Step: ${step.label}`);
                    }
                    documentSymbolCapture = await captureDocumentSymbolsAtEditor(activeEditor, step.payload);
                    break;
                }
                case 'captureInlayHints': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] captureInlayHints requires an active editor. Step: ${step.label}`);
                    }
                    inlayHintsCapture = await captureInlayHintsAtEditor(activeEditor, step.payload);
                    break;
                }
                case 'capturePrepareRename': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] capturePrepareRename requires an active editor. Step: ${step.label}`);
                    }
                    prepareRenameCapture = await capturePrepareRenameAtEditor(activeEditor, step.payload);
                    break;
                }
                case 'captureRenameEdit': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] captureRenameEdit requires an active editor. Step: ${step.label}`);
                    }
                    renameEditCapture = await captureRenameEditAtEditor(activeEditor, step.payload);
                    break;
                }
                case 'captureSemanticTokens': {
                    if (!activeEditor) {
                        throw new Error(`[real-replay] captureSemanticTokens requires an active editor. Step: ${step.label}`);
                    }
                    semanticTokensCapture = await captureSemanticTokensAtEditor(activeEditor, step.payload);
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
                    let workspaceIndexDebug: unknown | undefined;
                    try {
                        workspaceIndexDebug = await vscode.commands.executeCommand<any>(
                            'nsf._getWorkspaceIndexSymbolDebug',
                            { query, limit: maxNames }
                        );
                    } catch {
                        // ignore; provider capture should stay report-only.
                    }

                    workspaceSymbolCapture = {
                        durationMs,
                        query,
                        symbolCount: names.length,
                        topNames,
                        expectedNames,
                        expectedPresent,
                        error: captureError,
                        workspaceIndexDebug
                    };
                    break;
                }
                case 'waitForInternalStatus': {
                    const mode = step.payload?.mode === 'quiescent' ? 'quiescent' : 'idle';
                    const timeoutMs = Math.max(1000, step.payload?.timeoutMs ?? 180000);
                    if (mode === 'quiescent') {
                        await waitForClientQuiescent(step.label || 'replay waitForInternalStatus quiescent');
                    } else {
                        await waitForReplayServerIndexingIdle(
                            step.label || 'replay waitForInternalStatus idle',
                            timeoutMs
                        );
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
            const anomalies = detectReplayAnomalies(step, samples);
            if (fullDocumentTyping?.anomalies.length) {
                anomalies.push(...fullDocumentTyping.anomalies);
            }
            const stepReport: ReplayStepReport = {
                stepLabel: step.label,
                stepKind: step.kind,
                actionStartedAtMs,
                actionEndedAtMs: Date.now(),
                documentUri,
                samples,
                anomalies,
                completionCapture,
                signatureHelpCapture,
                hoverCapture,
                definitionCapture,
                referencesCapture,
                documentSymbolCapture,
                inlayHintsCapture,
                prepareRenameCapture,
                renameEditCapture,
                semanticTokensCapture,
                workspaceSymbolCapture,
                fullDocumentTyping
            };
            stepReport.anomalies.push(...collectTopLevelCaptureAnomalies(stepReport));
            stepReports.push(stepReport);
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
