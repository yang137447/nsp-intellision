import * as assert from 'assert';
import * as vscode from 'vscode';

import {
	computeLatencyStats,
	drainMetricsWindow,
	measureLatencySamples,
	readPerfIntEnv,
	waitForNextMetricsRevision,
	writePerfReport
} from './perf_helpers';
import { openFixture, positionOf, waitFor, waitForIndexingIdle } from './test_helpers';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const perfDescribe = testMode === 'perf' ? describe : describe.skip;

perfDescribe('NSF perf baseline: Signature help metrics', () => {
	it('captures fast-path signature-help metrics at idle', async function () {
		this.timeout(180000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		const interactiveDocument = await openFixture('module_signature_help.nsf');
		const interactivePosition = positionOf(
			interactiveDocument,
			'SigTarget(uv, 2.0, 3',
			1,
			'SigTarget(uv, '.length
		);
		const builtinDocument = await openFixture('module_signature_help_builtin.nsf');
		const builtinCallOffset = builtinDocument.getText().indexOf('lerp(');
		assert.ok(builtinCallOffset >= 0, 'Expected lerp call in builtin signature fixture.');
		const builtinPosition = builtinDocument.positionAt(builtinCallOffset + 'lerp('.length);

		await waitForIndexingIdle('perf signature-help metrics indexing idle');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					interactiveDocument.uri,
					interactivePosition
				),
			(value) => Boolean(value) && (value?.signatures.length ?? 0) > 0,
			'perf interactive signature help warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					builtinDocument.uri,
					builtinPosition
				),
			(value) => Boolean(value) && (value?.signatures.length ?? 0) > 0,
			'perf builtin signature help warmup'
		);

		const drained = await drainMetricsWindow('perf signature-help metrics');
		const interactiveRun = await measureLatencySamples(iterations, async () => {
			const help = await vscode.commands.executeCommand<vscode.SignatureHelp>(
				'vscode.executeSignatureHelpProvider',
				interactiveDocument.uri,
				interactivePosition
			);
			assert.ok(help && help.signatures.length > 0);
			assert.ok(help.signatures[help.activeSignature].label.includes('SigTarget'));
			return help.signatures.length;
		});
		const builtinRun = await measureLatencySamples(iterations, async () => {
			const help = await vscode.commands.executeCommand<vscode.SignatureHelp>(
				'vscode.executeSignatureHelpProvider',
				builtinDocument.uri,
				builtinPosition
			);
			assert.ok(help && help.signatures.length > 0);
			assert.ok(help.signatures[0].label.includes('lerp'));
			return help.signatures.length;
		});

		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf signature-help metrics flush'
		);
		const report = {
			scenario: 'm3-signature-help-metrics',
			fixtures: [
				{ id: 'PFX-S1', label: 'CurrentDocSignature', primaryDocument: 'module_signature_help.nsf' },
				{ id: 'PFX-S2', label: 'BuiltinSignature', primaryDocument: 'module_signature_help_builtin.nsf' }
			],
			loadMode: 'Idle',
			iterations,
			wallClock: {
				interactiveSignatureHelp: computeLatencyStats(interactiveRun.samples),
				builtinSignatureHelp: computeLatencyStats(builtinRun.samples)
			},
			metrics
		};
		writePerfReport('m3-signature-help-metrics', report);

		const signatureHelp = metrics.payload?.signatureHelp;
		assert.strictEqual(signatureHelp?.interactiveOverloadSamples ?? 0, 0);
		assert.ok((signatureHelp?.builtinSignatureSamples ?? 0) >= iterations);
		assert.ok((signatureHelp?.responseWriteSamples ?? 0) >= iterations * 2);
		assert.ok(
			(signatureHelp?.responseWriteAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
			`Expected signatureHelp responseWrite avg <= 10ms. Actual=${signatureHelp?.responseWriteAvgMs ?? 'n/a'}`
		);
	});

	it('captures fast-path signature-help metrics under background deferred load', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_METRIC_ITERATIONS', 4, 1, 20);
		const spamCount = readPerfIntEnv('NSF_PERF_BG_SPAM_COUNT', 24, 1, 120);
		const interactiveDocument = await openFixture('module_signature_help.nsf');
		const interactivePosition = positionOf(
			interactiveDocument,
			'SigTarget(uv, 2.0, 3',
			1,
			'SigTarget(uv, '.length
		);
		const backgroundDocument = await openFixture('module_perf_large_current_doc.nsf');

		await waitForIndexingIdle('perf signature-help load-bg metrics indexing idle');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					interactiveDocument.uri,
					interactivePosition
				),
			(value) => Boolean(value) && (value?.signatures.length ?? 0) > 0,
			'perf signature-help load-bg warmup'
		);

		const drained = await drainMetricsWindow('perf signature-help load-bg metrics');
		const spamPromise = vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
			'nsf._spamInlayRequests',
			{
				uri: backgroundDocument.uri.toString(),
				startLine: 0,
				startCharacter: 0,
				endLine: backgroundDocument.lineCount,
				endCharacter: 0,
				count: spamCount
			}
		);
		const interactiveRun = await measureLatencySamples(iterations, async () => {
			const help = await vscode.commands.executeCommand<vscode.SignatureHelp>(
				'vscode.executeSignatureHelpProvider',
				interactiveDocument.uri,
				interactivePosition
			);
			assert.ok(help && help.signatures.length > 0);
			assert.ok(help.signatures[help.activeSignature].label.includes('SigTarget'));
			return help.signatures.length;
		});
		const spamResult = await spamPromise;
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf signature-help load-bg metrics flush'
		);
		const report = {
			scenario: 'm3-signature-help-load-bg-metrics',
			fixtures: [
				{ id: 'PFX-S1', label: 'CurrentDocSignature', primaryDocument: 'module_signature_help.nsf' },
				{ id: 'PFX-3', label: 'LargeCurrentDoc', primaryDocument: 'module_perf_large_current_doc.nsf' }
			],
			loadMode: 'Load-BG',
			iterations,
			spamCount,
			spamResult,
			wallClock: {
				interactiveSignatureHelp: computeLatencyStats(interactiveRun.samples)
			},
			metrics
		};
		writePerfReport('m3-signature-help-load-bg-metrics', report);

		const signatureHelp = metrics.payload?.signatureHelp;
		assert.strictEqual(
			(spamResult?.completed ?? 0) + (spamResult?.cancelled ?? 0) + (spamResult?.failed ?? 0),
			spamCount
		);
		assert.ok((metrics.payload?.methods?.['textDocument/inlayHint']?.count ?? 0) > 0);
		assert.strictEqual(signatureHelp?.interactiveOverloadSamples ?? 0, 0);
		assert.ok((signatureHelp?.responseWriteSamples ?? 0) >= iterations);
		assert.ok(
			(signatureHelp?.responseWriteAvgMs ?? Number.POSITIVE_INFINITY) <= 10,
			`Expected signatureHelp responseWrite avg <= 10ms under Load-BG. Actual=${signatureHelp?.responseWriteAvgMs ?? 'n/a'}`
		);
	});
});
