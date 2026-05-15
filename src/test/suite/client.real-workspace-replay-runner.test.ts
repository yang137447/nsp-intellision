import * as assert from 'assert';
import * as vscode from 'vscode';

import { repoDescribe, waitForClientReady, waitForIndexingIdle } from './test_helpers';
import type { ReplayScript } from '../replay/real_workspace_replay_types';
import { buildReplayLatencySummary } from '../replay/real_workspace_replay_report_writer';
import { runReplayScript } from '../replay/real_workspace_replay_runner';
import { resolveReplayAnchor } from '../replay/real_workspace_replay_targets';
import { sampleReplayWindow } from '../replay/real_workspace_replay_sampler';

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

        const report = await runReplayScript(scriptBase);
        assert.strictEqual(report.scriptId, 'repo-runner-smoke');
        assert.strictEqual(report.steps.length, 2);
        assert.strictEqual(report.steps[1].samples.length, 3);
        const sampledBaseline = report.steps[1].samples[0].baselineInternalStatus;
        assert.ok(sampledBaseline, 'expected sampled pre-step internal status');
        report.steps[1].samples.forEach((sample) => {
            assert.strictEqual(
                sample.baselineInternalStatus?.completionRequestCount,
                sampledBaseline.completionRequestCount
            );
            assert.strictEqual(
                sample.baselineInternalStatus?.signatureHelpRequestCount,
                sampledBaseline.signatureHelpRequestCount
            );
        });

        const afterCleanup = await vscode.workspace.openTextDocument(resolvedAnchor.uri);
        assert.strictEqual(afterCleanup.getText(anchorRange), anchor.anchorText);
    });

    it('passes through an explicit baseline to sampled windows', async function () {
        const baseline = { completionRequestCount: 12345, signatureHelpRequestCount: 98765 };
        const step: ReplayScript['steps'][number] = {
            kind: 'typeText',
            label: 'inspect baseline',
            payload: { text: 'A' },
            samplingWindow: { label: 'baseline-window', delaysMs: [0] }
        };
        const samples = await sampleReplayWindow(step, undefined, baseline);
        assert.strictEqual(samples.length, 1);
        assert.strictEqual(samples[0].baselineInternalStatus?.completionRequestCount, baseline.completionRequestCount);
        assert.strictEqual(samples[0].baselineInternalStatus?.signatureHelpRequestCount, baseline.signatureHelpRequestCount);
    });

    it('records completion provider timings and request counters in capture reports', async function () {
        this.timeout(120000);

        await waitForClientReady('replay runner completion timing client ready');
        await waitForIndexingIdle('replay runner completion timing indexing idle');
        await vscode.commands.executeCommand('nsf._resetInternalStatus');

        const anchor = {
            workspaceFolderSuffix: 'nsp-intellision',
            relativePath: 'test_files/module_completion_current_doc.nsf',
            anchorText: 'return Comp',
            occurrence: 1,
            characterOffset: 'return Comp'.length
        };
        const script: ReplayScript = {
            id: 'repo-runner-completion-timing',
            title: 'Repo runner completion timing',
            workspaceHint: 'nsp-intellision',
            targetDocument: anchor,
            intent: 'Verify replay completion timing fields on repo fixtures.',
            tags: ['repo', 'smoke'],
            steps: [
                {
                    kind: 'openDocument',
                    label: 'open completion fixture',
                    target: anchor
                },
                {
                    kind: 'captureCompletion',
                    label: 'capture completion timing',
                    payload: {
                        expectedLabels: ['completionLocalColor'],
                        maxLabels: 20
                    }
                }
            ],
            cleanup: { restoreTouchedDocuments: true }
        };

        const report = await runReplayScript(script);
        const capture = report.steps[1].completionCapture;
        assert.ok(capture, 'expected completion capture report');
        assert.strictEqual(capture.measurementMode, 'separated-ui-provider');
        assert.strictEqual(capture.uiCoverageTriggerSource, 'nativeOnly');
        assert.strictEqual(capture.uiCoverage?.measurementMode, 'uiCoverage');
        assert.strictEqual(capture.uiCoverage?.triggerSource, 'nativeOnly');
        assert.strictEqual(capture.uiTrigger, undefined);
        assert.strictEqual(capture.providerVerification?.measurementMode, 'providerVerification');
        assert.ok(Array.isArray(capture.uiCoverage?.providerRequestSequence));
        assert.ok((capture.uiCoverage?.queueQuiet?.durationMs ?? -1) >= 0);
        assert.ok((capture.executeProviderDurationMs ?? -1) >= 0);
        assert.ok((capture.requestCounters?.providerDelta?.completionRequests ?? 0) > 0);
        assert.ok((capture.providerVerification?.requestCounters.delta?.completionRequests ?? 0) > 0);
        assert.ok(capture.requestCounters?.afterProvider?.completionLastProviderTiming);
        assert.strictEqual(capture.expectedPresent?.completionLocalColor, true);
        assert.ok((capture.directServerCompletion?.durationMs ?? -1) >= 0);
    });

    it('records full-document inlay continuity without transient drops', async function () {
        this.timeout(120000);

        await waitForClientReady('replay runner inlay continuity client ready');
        await waitForIndexingIdle('replay runner inlay continuity indexing idle');

        const anchor = {
            workspaceFolderSuffix: 'nsp-intellision',
            relativePath: 'test_files/module_inlay_hints.nsf',
            anchorText: 'float4 BlendColor',
            occurrence: 1,
            characterOffset: 0
        };
        const script: ReplayScript = {
            id: 'repo-runner-inlay-continuity',
            title: 'Repo runner inlay continuity',
            workspaceHint: 'nsp-intellision',
            targetDocument: anchor,
            intent: 'Verify full-document replay detects transient inlay hint disappearance.',
            tags: ['repo', 'inlay', 'continuity'],
            steps: [
                {
                    kind: 'openDocument',
                    label: 'open inlay fixture',
                    target: anchor
                },
                {
                    kind: 'typeDocumentFromDisk',
                    label: 'type inlay fixture from empty buffer',
                    payload: {
                        charactersPerEdit: 64,
                        checkpointEveryLines: 2,
                        checkpointSamplingDelaysMs: [0],
                        captureInlayContinuity: true
                    }
                }
            ],
            cleanup: { restoreTouchedDocuments: true }
        };

        const report = await runReplayScript(script);
        const fullDocumentTyping = report.steps[1].fullDocumentTyping;
        const continuity = fullDocumentTyping?.inlayContinuity;
        assert.ok(continuity?.enabled, 'expected inlay continuity report');
        assert.ok((continuity.sampleCount ?? 0) > 0, 'expected inlay continuity samples');
        assert.ok((continuity.nonEmptySampleCount ?? 0) > 0, 'expected at least one non-empty inlay sample');
        assert.strictEqual(continuity.transientDropDetected, false, JSON.stringify(continuity.drops));
        assert.strictEqual(continuity.endedMissingAfterVisible, false, JSON.stringify(continuity.drops));
        assert.ok(
            !(fullDocumentTyping?.anomalies ?? []).some((item) => item.startsWith('inlay-hints-')),
            JSON.stringify(fullDocumentTyping?.anomalies)
        );
    });

    it('summarizes replay probe latency and duplicated request paths', () => {
        const summary = buildReplayLatencySummary({
            scriptId: 'summary-fixture',
            steps: [
                {
                    fullDocumentTyping: {
                        probes: [
                            {
                                label: 'slow completion',
                                category: 'completion.symbol',
                                kind: 'completion',
                                line: 10,
                                triggerTyping: { durationMs: 12 },
                                completionCapture: {
                                    durationMs: 100,
                                    executeProviderDurationMs: 80,
                                    itemCount: 3,
                                    uiTrigger: { commandDurationMs: 5, durationMs: 20 },
                                    directServerCompletion: { durationMs: 4 },
                                    requestCounters: {
                                        nativeTriggerDelta: { completionRequests: 1, signatureHelpRequests: 0 },
                                        uiTriggerDelta: { completionRequests: 0, signatureHelpRequests: 0 },
                                        providerDelta: { completionRequests: 1, signatureHelpRequests: 0 },
                                        totalDelta: { completionRequests: 2, signatureHelpRequests: 0 },
                                        afterProvider: {
                                            completionLastProviderTiming: {
                                                totalMs: 77,
                                                lspRequestMs: 70,
                                                code2ProtocolMs: 1,
                                                protocol2CodeMs: 2
                                            }
                                        }
                                    }
                                }
                            },
                            {
                                label: 'fast completion',
                                kind: 'completion',
                                line: 20,
                                completionCapture: {
                                    durationMs: 25,
                                    executeProviderDurationMs: 20,
                                    itemCount: 1,
                                    directServerCompletion: { durationMs: 3 },
                                    requestCounters: {
                                        providerDelta: { completionRequests: 1, signatureHelpRequests: 0 },
                                        afterProvider: {
                                            completionLastProviderTiming: {
                                                totalMs: 18,
                                                lspRequestMs: 15
                                            }
                                        }
                                    }
                                }
                            },
                            {
                                label: 'separated completion',
                                kind: 'completion',
                                line: 25,
                                completionCapture: {
                                    measurementMode: 'separated-ui-provider',
                                    durationMs: 70,
                                    executeProviderDurationMs: 20,
                                    itemCount: 1,
                                    uiCoverage: {
                                        triggerSource: 'nativeOnly',
                                        requestBurstCount: 4,
                                        firstProviderRequestAtMs: 5,
                                        lastProviderRequestCompletedAtMs: 55,
                                        providerRequestSequence: [
                                            {
                                                sequence: 10,
                                                triggerKind: 2,
                                                triggerCharacter: 'd',
                                                line: 25,
                                                character: 7,
                                                prefixLength: 1,
                                                relativeStartedAtMs: 0,
                                                completionCoordinatorAction: 'coalescedBeforeLsp',
                                                completionCoordinatorSource: 'identifierPrefixAutoTrigger',
                                                completionCoordinatorPrefixLength: 1
                                            },
                                            {
                                                sequence: 11,
                                                triggerKind: 1,
                                                line: 25,
                                                character: 8,
                                                prefixLength: 2,
                                                relativeStartedAtMs: 10,
                                                completionCoordinatorAction: 'coalescedBeforeLsp',
                                                completionCoordinatorSource: 'identifierPrefixAutoTrigger',
                                                completionCoordinatorPrefixLength: 2
                                            },
                                            {
                                                sequence: 12,
                                                triggerKind: 2,
                                                triggerCharacter: 'f',
                                                line: 25,
                                                character: 9,
                                                prefixLength: 3,
                                                relativeStartedAtMs: 20,
                                                completionCoordinatorAction: 'coalescedBeforeLsp',
                                                completionCoordinatorSource: 'identifierPrefixAutoTrigger',
                                                completionCoordinatorPrefixLength: 3
                                            },
                                            {
                                                sequence: 13,
                                                documentVersion: 42,
                                                documentIsDirty: true,
                                                documentVersionAtNextStart: 43,
                                                documentIsDirtyAtNextStart: true,
                                                documentVersionAtLspStart: 43,
                                                documentIsDirtyAtLspStart: true,
                                                documentVersionAtProviderReturn: 44,
                                                documentIsDirtyAtProviderReturn: true,
                                                triggerKind: 1,
                                                line: 25,
                                                character: 9,
                                                prefixLength: 3,
                                                relativeStartedAtMs: 30,
                                                relativeCompletedAtMs: 55,
                                                relativeNextStartedAtMs: 34,
                                                relativeNextCompletedAtMs: 55,
                                                relativeLspRequestStartedAtMs: 35,
                                                relativeLspRequestCompletedAtMs: 50,
                                                lspRequestCompletedAtMs: 1050,
                                                totalMs: 25,
                                                nextWaitMs: 4,
                                                nextExecutionMs: 21,
                                                lspRequestMs: 15,
                                                lspStartDelayMs: 5,
                                                lspCompletionToProviderReturnMs: 5,
                                                lspRequestCount: 1,
                                                activeSameKindProviderCountAtStart: 2,
                                                activeSameKindNextCountAtStart: 1,
                                                completionCoordinatorAction: 'executed',
                                                completionCoordinatorSource: 'identifierPrefixAutoTrigger',
                                                completionCoordinatorPrefixLength: 3,
                                                completionDebugRequestId: 'completion:13:1000'
                                            }
                                        ],
                                        latestCompletionDebug: {
                                            nsfDebugRequestId: 'completion:12:900',
                                            documentFound: true,
                                            line: 24,
                                            character: 7,
                                            documentVersion: 41,
                                            requestDocumentVersion: 41,
                                            completionPrefix: 'ab',
                                            path: 'generic_completion_only',
                                            itemCount: 10,
                                            requestQueueWaitMs: 70,
                                            requestContextBuildMs: 40,
                                            handlerTotalMs: 30,
                                            interactiveCollectMs: 10,
                                            itemAssemblyMs: 10,
                                            responseWriteMs: 5,
                                            recent: [
                                                {
                                                    nsfDebugRequestId: 'completion:12:900',
                                                    documentFound: true,
                                                    line: 24,
                                                    character: 7,
                                                    documentVersion: 41,
                                                    requestDocumentVersion: 41,
                                                    completionPrefix: 'ab',
                                                    path: 'generic_completion_only',
                                                    itemCount: 10,
                                                    requestQueueWaitMs: 70,
                                                    requestContextBuildMs: 40,
                                                    handlerTotalMs: 30
                                                },
                                                {
                                                    nsfDebugRequestId: 'completion:13:1000',
                                                    clientSendStartedAtUnixMs: 1000,
                                                    serverReceivedAtUnixMs: 1001,
                                                    serverWorkerStartedAtUnixMs: 1002,
                                                    serverResponseWriteCompletedAtUnixMs: 1049,
                                                    serverDidChangeCompletedBeforeRequestCount: 9,
                                                    serverDidChangeOverlapClientSendCount: 1,
                                                    serverDidChangeOverlapClientSendMs: 0.8,
                                                    serverLastDidChangeDurationMs: 12,
                                                    serverLastDidChangeEndToRequestReceivedMs: 0.2,
                                                    documentFound: true,
                                                    line: 25,
                                                    character: 9,
                                                    documentVersion: 42,
                                                    requestDocumentVersion: 42,
                                                    completionPrefix: 'abc',
                                                    path: 'generic_completion_only',
                                                    itemCount: 12,
                                                    requestQueueWaitMs: 7,
                                                    requestContextBuildMs: 4,
                                                    handlerTotalMs: 3,
                                                    interactiveCollectMs: 1,
                                                    itemAssemblyMs: 1,
                                                    responseWriteMs: 0.5
                                                }
                                            ]
                                        },
                                        completionDebugHistory: [
                                            {
                                                nsfDebugRequestId: 'completion:12:900',
                                                documentFound: true,
                                                line: 24,
                                                character: 7,
                                                documentVersion: 41,
                                                requestDocumentVersion: 41,
                                                completionPrefix: 'ab',
                                                path: 'generic_completion_only',
                                                itemCount: 10,
                                                requestQueueWaitMs: 70,
                                                requestContextBuildMs: 40,
                                                handlerTotalMs: 30
                                            },
                                            {
                                                nsfDebugRequestId: 'completion:13:1000',
                                                clientSendStartedAtUnixMs: 1000,
                                                serverReceivedAtUnixMs: 1001,
                                                serverWorkerStartedAtUnixMs: 1002,
                                                serverResponseWriteCompletedAtUnixMs: 1049,
                                                serverDidChangeCompletedBeforeRequestCount: 9,
                                                serverDidChangeOverlapClientSendCount: 1,
                                                serverDidChangeOverlapClientSendMs: 0.8,
                                                serverLastDidChangeDurationMs: 12,
                                                serverLastDidChangeEndToRequestReceivedMs: 0.2,
                                                documentFound: true,
                                                line: 25,
                                                character: 9,
                                                documentVersion: 42,
                                                requestDocumentVersion: 42,
                                                completionPrefix: 'abc',
                                                path: 'generic_completion_only',
                                                itemCount: 12,
                                                requestQueueWaitMs: 7,
                                                requestContextBuildMs: 4,
                                                handlerTotalMs: 3,
                                                interactiveCollectMs: 1,
                                                itemAssemblyMs: 1,
                                                responseWriteMs: 0.5
                                            }
                                        ],
                                        queueQuiet: {
                                            durationMs: 160,
                                            timedOut: false,
                                            relativeStartedAtMs: 0,
                                            relativeCompletedAtMs: 160
                                        },
                                        requestCounters: {
                                            totalTriggerDelta: { completionRequests: 4, signatureHelpRequests: 0 }
                                        }
                                    },
                                    providerVerification: {
                                        durationMs: 20,
                                        uiQueueQuietTimedOut: false,
                                        requestCounters: {
                                            delta: { completionRequests: 1, signatureHelpRequests: 0 }
                                        },
                                        clientProviderTiming: {
                                            totalMs: 18,
                                            lspRequestMs: 15
                                        }
                                    },
                                    directServerCompletion: { durationMs: 3 },
                                    requestCounters: {
                                        nativeTriggerDelta: { completionRequests: 2, signatureHelpRequests: 0 },
                                        providerDelta: { completionRequests: 1, signatureHelpRequests: 0 },
                                        totalDelta: { completionRequests: 3, signatureHelpRequests: 0 }
                                    }
                                }
                            },
                            {
                                label: 'explicit overlap completion',
                                kind: 'completion',
                                line: 27,
                                completionCapture: {
                                    measurementMode: 'separated-ui-provider',
                                    durationMs: 60,
                                    executeProviderDurationMs: 20,
                                    triggerKind: 'Invoked',
                                    itemCount: 1,
                                    uiTrigger: { commandDurationMs: 3, durationMs: 8 },
                                    uiCoverage: {
                                        triggerSource: 'explicitSuggest',
                                        requestBurstCount: 1,
                                        providerRequestSequence: [
                                            {
                                                sequence: 20,
                                                triggerKind: 1,
                                                line: 27,
                                                character: 11,
                                                prefixLength: 0,
                                                relativeStartedAtMs: 0,
                                                relativeCompletedAtMs: 12,
                                                totalMs: 12,
                                                lspRequestMs: 10,
                                                completionCoordinatorAction: 'bypassedExplicit',
                                                completionCoordinatorSource: 'explicit'
                                            }
                                        ],
                                        queueQuiet: {
                                            durationMs: 30,
                                            timedOut: false,
                                            relativeStartedAtMs: 0,
                                            relativeCompletedAtMs: 30
                                        },
                                        requestCounters: {
                                            totalTriggerDelta: { completionRequests: 1, signatureHelpRequests: 0 }
                                        }
                                    },
                                    providerVerification: {
                                        durationMs: 20,
                                        uiQueueQuietTimedOut: false,
                                        requestCounters: {
                                            delta: { completionRequests: 1, signatureHelpRequests: 0 }
                                        },
                                        clientProviderTiming: {
                                            totalMs: 19,
                                            lspRequestMs: 10
                                        }
                                    },
                                    directServerCompletion: { durationMs: 2 },
                                    requestCounters: {
                                        uiTriggerDelta: { completionRequests: 1, signatureHelpRequests: 0 },
                                        providerDelta: { completionRequests: 1, signatureHelpRequests: 0 },
                                        totalDelta: { completionRequests: 2, signatureHelpRequests: 0 }
                                    }
                                }
                            },
                            {
                                label: 'signature',
                                kind: 'signatureHelp',
                                line: 30,
                                signatureHelpCapture: {
                                    durationMs: 40,
                                    executeProviderDurationMs: 30,
                                    signatureCount: 1,
                                    requestCounters: {
                                        providerDelta: { completionRequests: 0, signatureHelpRequests: 1 },
                                        afterProvider: {
                                            signatureHelpLastProviderTiming: {
                                                totalMs: 28,
                                                lspRequestMs: 26
                                            }
                                        }
                                    }
                                }
                            },
                            {
                                label: 'diagnostics',
                                kind: 'diagnostics'
                            }
                        ]
                    }
                }
            ]
        });

        assert.ok(summary, 'expected latency summary');
        assert.strictEqual(summary.probeCounts.total, 6);
        assert.strictEqual(summary.probeCounts.completion, 4);
        assert.strictEqual(summary.probeCounts.signatureHelp, 1);
        assert.strictEqual(summary.probeCounts.diagnostics, 1);
        assert.strictEqual(summary.completion?.duplicatedRequestProbeCount, 1);
        assert.strictEqual(summary.completion?.uiQueueQuietTimedOutProbeCount, 0);
        assert.strictEqual(summary.completion?.capture.maxMs, 100);
        assert.strictEqual(summary.completion?.directServer.maxMs, 4);
        assert.strictEqual(summary.completion?.slowest[0].label, 'slow completion');
        assert.strictEqual(summary.completion?.slowest[0].duplicatedRequestPath, true);
        const separated = summary.completion?.slowest.find((row) => row.label === 'separated completion');
        assert.strictEqual(separated?.duplicatedRequestPath, false);
        assert.strictEqual(separated?.uiCoverageTriggerSource, 'nativeOnly');
        assert.strictEqual(separated?.uiCoverageRequests, 4);
        assert.strictEqual(separated?.uiRequestBurstCount, 4);
        assert.strictEqual(separated?.explicitInvokeOverlapRequests, 0);
        assert.strictEqual(separated?.uiLatestVisibleProviderReturnMs, 55);
        assert.strictEqual(separated?.uiLatestVisibleProviderExecutionMs, 25);
        assert.strictEqual(separated?.uiLatestVisibleLspRequestMs, 15);
        assert.strictEqual(separated?.uiLatestVisibleNextWaitMs, 4);
        assert.strictEqual(separated?.uiLatestVisibleNextExecutionMs, 21);
        assert.strictEqual(separated?.uiLatestVisibleLspStartDelayMs, 5);
        assert.strictEqual(separated?.uiLatestVisibleLspCompletionToProviderReturnMs, 5);
        assert.strictEqual(separated?.uiLatestVisibleActiveProviderOverlapAtStart, 2);
        assert.strictEqual(separated?.uiLatestVisibleActiveNextOverlapAtStart, 1);
        assert.strictEqual(separated?.uiLatestVisibleServerHandlerMs, 3);
        assert.strictEqual(separated?.uiLatestVisibleClientResidualLspMs, 1);
        assert.strictEqual(separated?.uiLatestVisibleClientToServerReceivedMs, 1);
        assert.strictEqual(separated?.uiLatestVisibleServerResponseToClientResolveMs, 1);
        assert.strictEqual(separated?.uiLatestVisibleServerDidChangeOverlapMs, 0.8);
        assert.strictEqual(separated?.uiLatestVisibleServerDidChangeOverlapCount, 1);
        assert.strictEqual(separated?.uiLatestVisibleServerLastDidChangeDurationMs, 12);
        assert.strictEqual(separated?.uiLatestVisibleServerLastDidChangeGapMs, 0.2);
        assert.strictEqual(separated?.uiLatestVisibleHasServerDebug, true);
        assert.strictEqual(separated?.postLatestVisibleCleanupMs, 105);
        assert.strictEqual(separated?.postLatestVisibleProviderActivityMs, 0);
        assert.strictEqual(separated?.postLatestVisibleQuietGuardMs, 105);
        assert.strictEqual(separated?.uiProviderActivityDrainMs, 55);
        assert.strictEqual(separated?.uiQueueQuietGuardMs, 105);
        const explicitOverlap = summary.completion?.slowest.find((row) => row.label === 'explicit overlap completion');
        assert.strictEqual(explicitOverlap?.uiCoverageTriggerSource, 'explicitSuggest');
        assert.strictEqual(explicitOverlap?.explicitInvokeOverlapRequests, 1);
        assert.strictEqual(explicitOverlap?.uiLatestVisibleProviderReturnMs, 12);
        assert.strictEqual(explicitOverlap?.postLatestVisibleCleanupMs, 18);
        assert.strictEqual(explicitOverlap?.postLatestVisibleProviderActivityMs, 0);
        assert.strictEqual(explicitOverlap?.postLatestVisibleQuietGuardMs, 18);
        assert.strictEqual(summary.completion?.uiRequestBurst.maxMs, 4);
        assert.strictEqual(summary.completion?.uiLatestVisibleProviderReturn.maxMs, 55);
        assert.strictEqual(summary.completion?.uiLatestVisibleNextWait.maxMs, 4);
        assert.strictEqual(summary.completion?.uiLatestVisibleNextExecution.maxMs, 21);
        assert.strictEqual(summary.completion?.uiLatestVisibleLspStartDelay.maxMs, 5);
        assert.strictEqual(summary.completion?.uiLatestVisibleLspRequest.maxMs, 15);
        assert.strictEqual(summary.completion?.uiLatestVisibleLspCompletionToProviderReturn.maxMs, 5);
        assert.strictEqual(summary.completion?.uiLatestVisibleActiveProviderOverlapAtStart.maxMs, 2);
        assert.strictEqual(summary.completion?.uiLatestVisibleActiveNextOverlapAtStart.maxMs, 1);
        assert.strictEqual(summary.completion?.uiLatestVisibleServerHandler.maxMs, 3);
        assert.strictEqual(summary.completion?.uiLatestVisibleClientResidualLsp.maxMs, 1);
        assert.strictEqual(summary.completion?.uiLatestVisibleClientToServerReceived.maxMs, 1);
        assert.strictEqual(summary.completion?.uiLatestVisibleServerResponseToClientResolve.maxMs, 1);
        assert.strictEqual(summary.completion?.uiLatestVisibleServerDidChangeOverlap.maxMs, 0.8);
        assert.strictEqual(summary.completion?.uiLatestVisibleServerLastDidChangeDuration.maxMs, 12);
        assert.strictEqual(summary.completion?.uiLatestVisibleServerLastDidChangeGap.maxMs, 0.2);
        assert.strictEqual(summary.completion?.uiLatestVisibleWithServerDebugCount, 1);
        assert.strictEqual(summary.completion?.postLatestVisibleCleanup.maxMs, 105);
        assert.strictEqual(summary.completion?.postLatestVisibleProviderActivity.maxMs, 0);
        assert.strictEqual(summary.completion?.postLatestVisibleQuietGuard.maxMs, 105);
        assert.strictEqual(summary.completion?.uiQueueQuietGuard.maxMs, 105);
        const simulation = separated?.coalescingSimulation;
        assert.ok(simulation, 'expected completion coalescing simulation');
        assert.strictEqual(simulation.defaultDebounceWindowMs, 40);
        assert.strictEqual(simulation.simulatedReceivedRequests, 4);
        assert.strictEqual(simulation.simulatedExecutedRequests, 1);
        assert.strictEqual(simulation.simulatedCoalescedRequests, 3);
        assert.deepStrictEqual(simulation.simulatedRetainedSequences, [13]);
        assert.deepStrictEqual(simulation.simulatedDroppedSequences, [10, 11, 12]);
        assert.strictEqual(simulation.latestPrefixLength, 3);
        assert.strictEqual(simulation.wouldReduceBurstBy, 3);
        assert.strictEqual(simulation.eligibleRequestCount, 4);
        assert.strictEqual(simulation.bypassedRequestCount, 0);
        const window25 = simulation.windows.find((window) => window.debounceWindowMs === 25);
        assert.strictEqual(window25?.simulatedExecutedRequests, 2);
        assert.deepStrictEqual(window25?.simulatedRetainedSequences, [11, 13]);
        assert.strictEqual(summary.completion?.coalescingSimulation?.probeCount, 2);
        assert.strictEqual(summary.completion?.coalescingSimulation?.simulatedReceivedRequests, 5);
        assert.strictEqual(summary.completion?.coalescingSimulation?.simulatedExecutedRequests, 2);
        assert.strictEqual(summary.completion?.coalescingSimulation?.simulatedCoalescedRequests, 3);
        assert.strictEqual(summary.completion?.coalescingSimulation?.bypassedRequestCount, 1);
        assert.strictEqual(separated?.coordinatorActual?.receivedRequests, 4);
        assert.strictEqual(separated?.coordinatorActual?.executedLspRequests, 1);
        assert.strictEqual(separated?.coordinatorActual?.coalescedBeforeLspRequests, 3);
        assert.deepStrictEqual(separated?.coordinatorActual?.retainedSequences, [13]);
        assert.deepStrictEqual(separated?.coordinatorActual?.droppedSequences, [10, 11, 12]);
        assert.strictEqual(summary.completion?.coordinatorActual?.receivedRequests, 5);
        assert.strictEqual(summary.completion?.coordinatorActual?.executedLspRequests, 2);
        assert.strictEqual(summary.completion?.coordinatorActual?.coalescedBeforeLspRequests, 3);
        assert.strictEqual(summary.completion?.coordinatorActual?.bypassedExplicitRequests, 1);
        assert.strictEqual(separated?.uiExecutedAttribution?.executedRequestCount, 1);
        assert.deepStrictEqual(separated?.uiExecutedAttribution?.executedSequences, [13]);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedHasServerDebug, true);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedLspRequestMs, 15);
        assert.strictEqual(separated?.uiExecutedAttribution?.waitUntilLatestExecutedCompletedMs, 55);
        assert.strictEqual(separated?.uiExecutedAttribution?.postLatestExecutedQuietMs, 105);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedClientAttribution?.nextWaitMs, 4);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedClientAttribution?.nextExecutionMs, 21);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedClientAttribution?.lspStartDelayMs, 5);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedClientAttribution?.lspCompletionToProviderReturnMs, 5);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedClientAttribution?.activeSameKindProviderCountAtStart, 2);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedClientAttribution?.activeSameKindNextCountAtStart, 1);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedClientAttribution?.documentVersionAdvancedBeforeNext, true);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedClientAttribution?.documentVersionAdvancedBeforeLsp, true);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedClientAttribution?.documentVersionAdvancedDuringLsp, true);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.requestQueueWaitMs, 7);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.requestContextBuildMs, 4);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.handlerTotalMs, 3);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.clientResidualLspMs, 1);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.clientDocumentVersion, 42);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.clientDocumentIsDirty, true);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.clientDebugRequestId, 'completion:13:1000');
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.nsfDebugRequestId, 'completion:13:1000');
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.matchedByDebugRequestId, true);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.serverDidChangeCompletedBeforeRequestCount, 9);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.serverDidChangeOverlapClientSendCount, 1);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.serverDidChangeOverlapClientSendMs, 0.8);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.serverLastDidChangeDurationMs, 12);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.serverLastDidChangeEndToRequestReceivedMs, 0.2);
        assert.strictEqual(separated?.uiExecutedAttribution?.latestExecutedServerAttribution?.matchesLatestExecutedPosition, true);
        assert.strictEqual(summary.completion?.uiExecutedLspRequest.maxMs, 15);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.executedRequestCount, 1);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.postLatestExecutedQuiet.maxMs, 105);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.clientNextWait.maxMs, 4);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.clientNextExecution.maxMs, 21);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.clientLspStartDelay.maxMs, 5);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.clientLspCompletionToProviderReturn.maxMs, 5);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.clientActiveProviderOverlapAtStart.maxMs, 2);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.clientActiveNextOverlapAtStart.maxMs, 1);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.documentAdvancedBeforeNextCount, 1);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.documentAdvancedBeforeLspCount, 1);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.documentAdvancedDuringLspCount, 1);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.serverQueueWait.maxMs, 7);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.serverContextBuild.maxMs, 4);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.serverHandler.maxMs, 3);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.clientResidualLsp.maxMs, 1);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.serverDidChangeOverlapClientSend.maxMs, 0.8);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.serverLastDidChangeDuration.maxMs, 12);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.serverLastDidChangeEndToRequestReceived.maxMs, 0.2);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.latestExecutedWithServerDebugCount, 1);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.serverDebugRequestIdMatchedCount, 1);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.serverDebugRequestIdUnmatchedCount, 0);
        assert.strictEqual(summary.completion?.uiExecutedAttribution?.serverDebugRequestIdFallbackCount, 0);
        const byTriggerSource = summary.completion?.uiCoverageByTriggerSource ?? [];
        const nativeSummary = byTriggerSource.find((entry) => entry.triggerSource === 'nativeOnly');
        const explicitSummary = byTriggerSource.find((entry) => entry.triggerSource === 'explicitSuggest');
        assert.strictEqual(nativeSummary?.probeCount, 2);
        assert.strictEqual(nativeSummary?.uiQueueQuiet.maxMs, 160);
        assert.strictEqual(nativeSummary?.uiLatestVisibleProviderReturn.maxMs, 55);
        assert.strictEqual(nativeSummary?.uiLatestVisibleLspStartDelay.maxMs, 5);
        assert.strictEqual(nativeSummary?.uiLatestVisibleLspRequest.maxMs, 15);
        assert.strictEqual(nativeSummary?.uiLatestVisibleClientResidualLsp.maxMs, 1);
        assert.strictEqual(nativeSummary?.uiLatestVisibleClientToServerReceived.maxMs, 1);
        assert.strictEqual(nativeSummary?.uiLatestVisibleServerResponseToClientResolve.maxMs, 1);
        assert.strictEqual(nativeSummary?.uiLatestVisibleServerDidChangeOverlap.maxMs, 0.8);
        assert.strictEqual(nativeSummary?.uiLatestVisibleServerLastDidChangeDuration.maxMs, 12);
        assert.strictEqual(nativeSummary?.uiLatestVisibleServerLastDidChangeGap.maxMs, 0.2);
        assert.strictEqual(nativeSummary?.postLatestVisibleCleanup.maxMs, 105);
        assert.strictEqual(nativeSummary?.postLatestVisibleProviderActivity.maxMs, 0);
        assert.strictEqual(nativeSummary?.postLatestVisibleQuietGuard.maxMs, 105);
        assert.strictEqual(nativeSummary?.uiQueueQuietGuard.maxMs, 105);
        assert.strictEqual(nativeSummary?.explicitInvokeOverlapRequests, 0);
        assert.strictEqual(nativeSummary?.latestExecutedWithServerDebugCount, 1);
        assert.strictEqual(explicitSummary?.probeCount, 2);
        assert.strictEqual(explicitSummary?.uiQueueQuiet.maxMs, 30);
        assert.strictEqual(explicitSummary?.uiLatestVisibleProviderReturn.maxMs, 12);
        assert.strictEqual(explicitSummary?.postLatestVisibleCleanup.maxMs, 18);
        assert.strictEqual(explicitSummary?.postLatestVisibleProviderActivity.maxMs, 0);
        assert.strictEqual(explicitSummary?.postLatestVisibleQuietGuard.maxMs, 18);
        assert.strictEqual(explicitSummary?.explicitInvokeOverlapRequests, 1);
        assert.strictEqual(explicitSummary?.latestExecutedWithServerDebugCount, 0);
        assert.strictEqual(summary.signatureHelp?.clientLspRequest.maxMs, 26);
    });

    it('summarizes top-level signature capture latency and quiet guard split', () => {
        const summary = buildReplayLatencySummary({
            scriptId: 'top-level-signature-summary-fixture',
            steps: [
                {
                    stepLabel: 'capture top-level signature help',
                    stepKind: 'captureSignatureHelp',
                    signatureHelpCapture: {
                        measurementMode: 'separated-ui-provider',
                        durationMs: 240,
                        executeProviderDurationMs: 20,
                        signatureCount: 1,
                        uiCoverage: {
                            requestBurstCount: 2,
                            firstProviderRequestAtMs: 10,
                            lastProviderRequestCompletedAtMs: 90,
                            providerRequestSequence: [
                                {
                                    sequence: 1,
                                    relativeStartedAtMs: 10,
                                    relativeCompletedAtMs: 50,
                                    totalMs: 40,
                                    nextWaitMs: 2,
                                    nextExecutionMs: 38,
                                    lspStartDelayMs: 3,
                                    lspRequestMs: 35,
                                    lspCompletionToProviderReturnMs: 1,
                                    activeSameKindProviderCountAtStart: 0,
                                    activeSameKindNextCountAtStart: 0
                                },
                                {
                                    sequence: 2,
                                    relativeStartedAtMs: 60,
                                    relativeCompletedAtMs: 90,
                                    totalMs: 30,
                                    nextWaitMs: 1,
                                    nextExecutionMs: 29,
                                    lspStartDelayMs: 2,
                                    lspRequestMs: 25,
                                    lspCompletionToProviderReturnMs: 2,
                                    activeSameKindProviderCountAtStart: 1,
                                    activeSameKindNextCountAtStart: 0
                                }
                            ],
                            queueQuiet: {
                                durationMs: 180,
                                timedOut: false,
                                relativeStartedAtMs: 0,
                                relativeCompletedAtMs: 250
                            },
                            requestCounters: {
                                totalTriggerDelta: { completionRequests: 0, signatureHelpRequests: 2 }
                            }
                        },
                        providerVerification: {
                            durationMs: 20,
                            uiQueueQuietTimedOut: false,
                            requestCounters: {
                                delta: { completionRequests: 0, signatureHelpRequests: 1 }
                            },
                            clientProviderTiming: {
                                totalMs: 18,
                                lspRequestMs: 17
                            }
                        },
                        requestCounters: {
                            providerDelta: { completionRequests: 0, signatureHelpRequests: 1 },
                            totalDelta: { completionRequests: 0, signatureHelpRequests: 3 }
                        }
                    }
                }
            ]
        });

        assert.ok(summary, 'expected latency summary for top-level signature capture');
        assert.strictEqual(summary.probeCounts.total, 1);
        assert.strictEqual(summary.probeCounts.signatureHelp, 1);
        assert.strictEqual(summary.signatureHelp?.capture.maxMs, 240);
        assert.strictEqual(summary.signatureHelp?.executeProvider.maxMs, 20);
        assert.strictEqual(summary.signatureHelp?.clientLspRequest.maxMs, 17);
        assert.strictEqual(summary.signatureHelp?.uiRequestBurst.maxMs, 2);
        assert.strictEqual(summary.signatureHelp?.uiLatestVisibleProviderReturn.maxMs, 90);
        assert.strictEqual(summary.signatureHelp?.uiLatestVisibleProviderExecution.maxMs, 30);
        assert.strictEqual(summary.signatureHelp?.uiLatestVisibleNextWait.maxMs, 1);
        assert.strictEqual(summary.signatureHelp?.uiLatestVisibleNextExecution.maxMs, 29);
        assert.strictEqual(summary.signatureHelp?.uiLatestVisibleLspStartDelay.maxMs, 2);
        assert.strictEqual(summary.signatureHelp?.uiLatestVisibleLspRequest.maxMs, 25);
        assert.strictEqual(summary.signatureHelp?.uiLatestVisibleLspCompletionToProviderReturn.maxMs, 2);
        assert.strictEqual(summary.signatureHelp?.uiLatestVisibleActiveProviderOverlapAtStart.maxMs, 1);
        assert.strictEqual(summary.signatureHelp?.uiLatestVisibleActiveNextOverlapAtStart.maxMs, 0);
        assert.strictEqual(summary.signatureHelp?.postLatestVisibleCleanup.maxMs, 160);
        assert.strictEqual(summary.signatureHelp?.postLatestVisibleProviderActivity.maxMs, 0);
        assert.strictEqual(summary.signatureHelp?.postLatestVisibleQuietGuard.maxMs, 160);
        assert.strictEqual(summary.signatureHelp?.uiQueueQuietGuard.maxMs, 160);
        assert.strictEqual(summary.signatureHelp?.slowest[0].label, 'capture top-level signature help');
    });
});
