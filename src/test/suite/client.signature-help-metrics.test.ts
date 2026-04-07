import * as assert from 'assert';
import * as vscode from 'vscode';

import { drainMetricsWindow, waitForNextMetricsRevision } from './perf_helpers';
import { openFixture, positionOf, repoDescribe, waitFor, waitForIndexingIdle } from './test_helpers';

async function requestSignatureHelpAndFlushMetrics(
	document: vscode.TextDocument,
	position: vscode.Position,
	label: string
) {
	const drained = await drainMetricsWindow(label);
	await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.SignatureHelp>(
				'vscode.executeSignatureHelpProvider',
				document.uri,
				position
			),
		(value) => Boolean(value) && (value?.signatures.length ?? 0) > 0,
		`${label} signature help`
	);
	return waitForNextMetricsRevision(drained.revision ?? 0, `${label} metrics flush`);
}

repoDescribe('NSF client integration: Signature Help Metrics', () => {
	it('exports builtin and response-write timings for signature-help fast paths', async function () {
		this.timeout(90000);

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

		const drained = await drainMetricsWindow('signature-help metrics');
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					interactiveDocument.uri,
					interactivePosition
				),
			(value) => Boolean(value) && (value?.signatures.length ?? 0) > 0,
			'interactive signature help metrics warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					builtinDocument.uri,
					builtinPosition
				),
			(value) => Boolean(value) && (value?.signatures.length ?? 0) > 0,
			'builtin signature help metrics warmup'
		);

		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'signature-help metrics flush'
		);
		const signatureHelp = metrics.payload?.signatureHelp;
		assert.ok(
			(signatureHelp?.interactiveOverloadSamples ?? 0) === 0,
			`Expected fast-path signature help to skip interactiveOverloadSamples. Actual=${JSON.stringify(signatureHelp)}`
		);
		assert.ok(
			(signatureHelp?.builtinSignatureSamples ?? 0) > 0,
			`Expected builtinSignatureSamples > 0. Actual=${JSON.stringify(signatureHelp)}`
		);
		assert.ok(
			(signatureHelp?.responseWriteSamples ?? 0) > 0,
			`Expected responseWriteSamples > 0. Actual=${JSON.stringify(signatureHelp)}`
		);
	});

	it('avoids interactive-overload timing for builtin-only signature help targets', async function () {
		this.timeout(90000);

		const document = await openFixture('module_signature_help_builtin.nsf');
		const callOffset = document.getText().indexOf('lerp(');
		assert.ok(callOffset >= 0, 'Expected lerp call in builtin signature fixture.');
		const position = document.positionAt(callOffset + 'lerp('.length);

		await waitForIndexingIdle('builtin signature help fast-path indexing idle');
		const metrics = await requestSignatureHelpAndFlushMetrics(
			document,
			position,
			'builtin signature help fast path'
		);
		const signatureHelp = metrics.payload?.signatureHelp;
		assert.strictEqual(
			signatureHelp?.interactiveOverloadSamples ?? 0,
			0,
			`Expected builtin-only signature help to skip interactive overload timing. Actual=${JSON.stringify(signatureHelp)}`
		);
		assert.ok(
			(signatureHelp?.builtinSignatureSamples ?? 0) > 0,
			`Expected builtinSignatureSamples > 0. Actual=${JSON.stringify(signatureHelp)}`
		);
	});

	it('avoids interactive-overload timing for single-target current-doc signature help', async function () {
		this.timeout(90000);

		const document = await openFixture('module_signature_help.nsf');
		const position = positionOf(
			document,
			'SigTarget(uv, 2.0, 3',
			1,
			'SigTarget(uv, '.length
		);

		await waitForIndexingIdle('current-doc signature help fast-path indexing idle');
		const metrics = await requestSignatureHelpAndFlushMetrics(
			document,
			position,
			'current-doc signature help fast path'
		);
		const signatureHelp = metrics.payload?.signatureHelp;
		assert.strictEqual(
			signatureHelp?.interactiveOverloadSamples ?? 0,
			0,
			`Expected single-target current-doc signature help to skip interactive overload timing. Actual=${JSON.stringify(signatureHelp)}`
		);
		assert.ok(
			(signatureHelp?.responseWriteSamples ?? 0) > 0,
			`Expected responseWriteSamples > 0. Actual=${JSON.stringify(signatureHelp)}`
		);
	});

	it('keeps overloaded current-doc signature help on the current-unit fast path', async function () {
		this.timeout(90000);

		const document = await openFixture('module_signature_help_overload_benchmark.nsf');
		const position = positionOf(
			document,
			'OverloadBench(v3, 2.0, 1',
			1,
			'OverloadBench(v3, '.length
		);

		await waitForIndexingIdle('overload signature help metrics indexing idle');
		const metrics = await requestSignatureHelpAndFlushMetrics(
			document,
			position,
			'overload signature help'
		);
		const signatureHelp = metrics.payload?.signatureHelp;
		assert.strictEqual(
			signatureHelp?.interactiveOverloadSamples ?? 0,
			0,
			`Expected overloaded current-doc signature help to stay on the current-unit fast path. Actual=${JSON.stringify(signatureHelp)}`
		);
	});
});
