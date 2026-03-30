import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

import { PERF_FIXTURES } from './perf_fixtures';
import {
	computeLatencyStats,
	drainMetricsWindow,
	measureLatencySamples,
	readPerfIntEnv,
	waitForMetricsHistorySinceRevision,
	waitForNextMetricsRevision,
	writePerfReport
} from './perf_helpers';
import {
	countWorkspaceEdits,
	getDocumentRuntimeDebug,
	getCompletionItems,
	getWorkspaceRoot,
	hoverToText,
	openFixture,
	positionOf,
	type ProviderLocation,
	toFsPath,
	waitFor,
	waitForClientReady,
	waitForClientQuiescent,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from './test_helpers';

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const perfDescribe = testMode === 'perf' ? describe : describe.skip;

const completionMethodKey = 'textDocument/completion';
const definitionMethodKey = 'textDocument/definition';
const hoverMethodKey = 'textDocument/hover';
const inlayMethodKey = 'textDocument/inlayHint';
const semanticTokensMethodKey = 'textDocument/semanticTokens/full';
const documentSymbolsMethodKey = 'textDocument/documentSymbol';
const signatureHelpMethodKey = 'textDocument/signatureHelp';

function wholeDocumentRange(document: vscode.TextDocument): vscode.Range {
	return new vscode.Range(new vscode.Position(0, 0), document.positionAt(document.getText().length));
}

function computeP95DegradationRatio(idleStats: { p95Ms: number }, loadStats: { p95Ms: number }): number {
	if (idleStats.p95Ms <= 0) {
		return loadStats.p95Ms <= 0 ? 0 : Number.POSITIVE_INFINITY;
	}
	return (loadStats.p95Ms - idleStats.p95Ms) / idleStats.p95Ms;
}

async function triggerNoOpEdit(document: vscode.TextDocument): Promise<void> {
	const insertEdit = new vscode.WorkspaceEdit();
	insertEdit.insert(document.uri, new vscode.Position(0, 0), ' ');
	await vscode.workspace.applyEdit(insertEdit);
	const removeEdit = new vscode.WorkspaceEdit();
	removeEdit.delete(document.uri, new vscode.Range(0, 0, 0, 1));
	await vscode.workspace.applyEdit(removeEdit);
}

function providerLocationUri(location: ProviderLocation): vscode.Uri {
	return 'targetUri' in location ? location.targetUri : location.uri;
}

async function appendTrailingCommentEdit(
	document: vscode.TextDocument,
	commentSuffix: string
): Promise<{ document: vscode.TextDocument; insertedRange: vscode.Range }> {
	const documentText = document.getText();
	const startOffset = documentText.length;
	const insertedText = `${documentText.endsWith('\n') ? '' : '\n'}// ${commentSuffix}`;
	const edit = new vscode.WorkspaceEdit();
	edit.insert(document.uri, document.positionAt(startOffset), insertedText);
	const applied = await vscode.workspace.applyEdit(edit);
	assert.ok(applied, 'Expected trailing perf comment edit to apply.');

	const updatedDocument = await vscode.workspace.openTextDocument(document.uri);
	return {
		document: updatedDocument,
		insertedRange: new vscode.Range(
			updatedDocument.positionAt(startOffset),
			updatedDocument.positionAt(startOffset + insertedText.length)
		)
	};
}

async function appendTrailingLiteralEdit(
	document: vscode.TextDocument,
	insertedText: string
): Promise<{ document: vscode.TextDocument; insertedRange: vscode.Range }> {
	const documentText = document.getText();
	const startOffset = documentText.length;
	const edit = new vscode.WorkspaceEdit();
	edit.insert(document.uri, document.positionAt(startOffset), insertedText);
	const applied = await vscode.workspace.applyEdit(edit);
	assert.ok(applied, 'Expected trailing literal edit to apply.');

	const updatedDocument = await vscode.workspace.openTextDocument(document.uri);
	return {
		document: updatedDocument,
		insertedRange: new vscode.Range(
			updatedDocument.positionAt(startOffset),
			updatedDocument.positionAt(startOffset + insertedText.length)
		)
	};
}

async function deleteTrailingCommentEdit(
	document: vscode.TextDocument,
	insertedRange: vscode.Range
): Promise<vscode.TextDocument> {
	const edit = new vscode.WorkspaceEdit();
	edit.delete(document.uri, insertedRange);
	const applied = await vscode.workspace.applyEdit(edit);
	assert.ok(applied, 'Expected trailing perf comment cleanup to apply.');
	return vscode.workspace.openTextDocument(document.uri);
}

async function insertTextAt(
	document: vscode.TextDocument,
	position: vscode.Position,
	text: string
): Promise<vscode.TextDocument> {
	const edit = new vscode.WorkspaceEdit();
	edit.insert(document.uri, position, text);
	const applied = await vscode.workspace.applyEdit(edit);
	assert.ok(applied, 'Expected text insertion edit to apply.');
	return vscode.workspace.openTextDocument(document.uri);
}

async function deleteRangeFromDocument(
	document: vscode.TextDocument,
	range: vscode.Range
): Promise<vscode.TextDocument> {
	const edit = new vscode.WorkspaceEdit();
	edit.delete(document.uri, range);
	const applied = await vscode.workspace.applyEdit(edit);
	assert.ok(applied, 'Expected range deletion edit to apply.');
	return vscode.workspace.openTextDocument(document.uri);
}

async function waitForDiagnosticMessagePresence(
	document: vscode.TextDocument,
	messageFragment: string,
	expectedPresent: boolean,
	label: string
): Promise<readonly vscode.Diagnostic[]> {
	return waitFor(
		() => vscode.languages.getDiagnostics(document.uri),
		(value) =>
			Array.isArray(value) &&
			value.some((diagnostic) => diagnostic.message.includes(messageFragment)) === expectedPresent,
		label
	);
}

async function waitForDiagnosticMessagePresenceFast(
	document: vscode.TextDocument,
	messageFragment: string,
	expectedPresent: boolean,
	label: string,
	attempts = 120,
	delayMs = 20
): Promise<readonly vscode.Diagnostic[]> {
	let lastValue: readonly vscode.Diagnostic[] = [];
	for (let attempt = 0; attempt < attempts; attempt++) {
		lastValue = vscode.languages.getDiagnostics(document.uri);
		if (
			Array.isArray(lastValue) &&
			lastValue.some((diagnostic) => diagnostic.message.includes(messageFragment)) === expectedPresent
		) {
			return lastValue;
		}
		await new Promise((resolve) => setTimeout(resolve, delayMs));
	}
	throw new Error(`Timed out waiting for ${label}.`);
}

function makeWatchProviderVariant(original: string): string {
	if (original.includes('u_value2')) {
		return original.replace('u_value2', 'u_value');
	}
	return original.replace('u_value', 'u_value2');
}

async function churnWorkspaceWatchLoad(
	providerPath: string,
	consumerDocument: vscode.TextDocument,
	churnCount: number
): Promise<{ original: string; variant: string }> {
	const original = fs.readFileSync(providerPath, 'utf8');
	const variant = makeWatchProviderVariant(original);
	assert.notStrictEqual(
		variant,
		original,
		'Expected watch-provider variant to differ from the original file contents.'
	);

	for (let index = 0; index < churnCount; index++) {
		const next = index % 2 === 0 ? variant : original;
		await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(next, 'utf8'));
		await triggerNoOpEdit(consumerDocument);
	}

	return { original, variant };
}

perfDescribe('NSF perf baseline: M0 harness', () => {
	it('captures idle completion and hover baseline with metrics snapshots', async function () {
		this.timeout(180000);

		const iterations = readPerfIntEnv('NSF_PERF_ITERATIONS', 8, 1, 60);
		await waitForIndexingIdle('perf idle baseline indexing idle');

		const completionDocument = await openFixture(PERF_FIXTURES.pfx1SmallCurrentDoc.primaryDocument);
		const completionPosition = positionOf(
			completionDocument,
			'return Comp',
			1,
			'return Comp'.length
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					completionDocument.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'perf completion warmup'
		);

		const hoverDocument = await openFixture('module_hover_docs.nsf');
		const hoverPosition = positionOf(hoverDocument, 'SuiteHoverFunc', 2, 2);
		await waitFor(
			() => vscode.commands.executeCommand<vscode.Hover[]>('vscode.executeHoverProvider', hoverDocument.uri, hoverPosition),
			(value) => Array.isArray(value) && value.length > 0,
			'perf hover warmup'
		);

		await waitForClientQuiescent('perf idle baseline quiescent');
		const drained = await drainMetricsWindow('perf idle baseline');
		const completionRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				completionDocument.uri,
				completionPosition
			);
			assert.ok(getCompletionItems(result).length > 0, 'Expected completion items during perf baseline.');
			return getCompletionItems(result).length;
		});
		const hoverRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				hoverDocument.uri,
				hoverPosition
			);
			assert.ok(Array.isArray(result) && result.length > 0, 'Expected hover results during perf baseline.');
			const text = hoverToText(result);
			assert.ok(text.includes('SuiteHoverFunc('), text);
			return text.length;
		});
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf idle baseline metrics flush'
		);

		const report = {
			scenario: 'm0-idle-interactive-baseline',
			fixtures: [
				PERF_FIXTURES.pfx1SmallCurrentDoc,
				{ id: 'PFX-1B', label: 'HoverDocs', primaryDocument: 'module_hover_docs.nsf' }
			],
			loadMode: 'Idle',
			iterations,
			wallClock: {
				completion: computeLatencyStats(completionRun.samples),
				hover: computeLatencyStats(hoverRun.samples)
			},
			metrics
		};
		writePerfReport('m0-idle-interactive-baseline', report);

		assert.ok((metrics.payload?.methods?.[completionMethodKey]?.count ?? 0) >= iterations);
		assert.ok((metrics.payload?.methods?.[hoverMethodKey]?.count ?? 0) >= iterations);
	});

	it('captures completion baseline under background inlay load', async function () {
		this.timeout(180000);

		const iterations = readPerfIntEnv('NSF_PERF_ITERATIONS', 8, 1, 60);
		const spamCount = readPerfIntEnv('NSF_PERF_BG_SPAM_COUNT', 24, 1, 120);
		await waitForIndexingIdle('perf background baseline indexing idle');

		const document = await openFixture(PERF_FIXTURES.pfx2MediumEditPath.primaryDocument);
		const completionPosition = new vscode.Position(0, 0);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'perf background completion warmup'
		);

		const drained = await drainMetricsWindow('perf background baseline');
		const spamPromise = vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
			'nsf._spamInlayRequests',
			{
				uri: document.uri.toString(),
				startLine: 0,
				startCharacter: 0,
				endLine: Math.max(50, document.lineCount + 5),
				endCharacter: 0,
				count: spamCount
			}
		);
		const completionRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				document.uri,
				completionPosition
			);
			assert.ok(getCompletionItems(result).length > 0, 'Expected completion items under background load.');
			return getCompletionItems(result).length;
		});
		const spamResult = await spamPromise;
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf background baseline metrics flush'
		);

		const report = {
			scenario: 'm0-load-bg-completion-baseline',
			fixture: PERF_FIXTURES.pfx2MediumEditPath,
			loadMode: 'Load-BG',
			iterations,
			spamCount,
			spamResult,
			wallClock: {
				completion: computeLatencyStats(completionRun.samples)
			},
			metrics
		};
		writePerfReport('m0-load-bg-completion-baseline', report);

		assert.strictEqual(
			(spamResult.completed ?? 0) + (spamResult.cancelled ?? 0) + (spamResult.failed ?? 0),
			spamCount
		);
		assert.ok((metrics.payload?.methods?.[completionMethodKey]?.count ?? 0) >= iterations);
		assert.ok((metrics.payload?.methods?.[inlayMethodKey]?.count ?? 0) > 0);
	});

	it('captures idle deferred-doc baseline across medium and large documents', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_DEFERRED_ITERATIONS', 3, 1, 12);
		await waitForIndexingIdle('perf deferred baseline indexing idle');

		const mediumSemanticDocument = await openFixture(
			PERF_FIXTURES.pfx2MediumEditPath.semanticTokensDocument!
		);
		const mediumInlayDocument = await openFixture(PERF_FIXTURES.pfx2MediumEditPath.inlayDocument!);
		const mediumDiagnosticsDocument = await openFixture(
			PERF_FIXTURES.pfx2MediumEditPath.diagnosticsDocument!
		);
		const largeDocument = await openFixture(PERF_FIXTURES.pfx3LargeCurrentDoc.primaryDocument);

		const mediumInlayRange = wholeDocumentRange(mediumInlayDocument);
		const largeRange = wholeDocumentRange(largeDocument);

		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SemanticTokensLegend>(
					'vscode.provideDocumentSemanticTokensLegend',
					mediumSemanticDocument.uri
				),
			(value) => Array.isArray(value?.tokenTypes) && value.tokenTypes.length > 0,
			'perf deferred semantic legend'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SemanticTokens>(
					'vscode.provideDocumentSemanticTokens',
					mediumSemanticDocument.uri
				),
			(value) => Boolean(value) && value.data.length > 0,
			'perf deferred semantic warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<any[]>(
					'vscode.executeInlayHintProvider',
					mediumInlayDocument.uri,
					mediumInlayRange
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf deferred inlay warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<any[]>(
					'vscode.executeDocumentSymbolProvider',
					largeDocument.uri
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf deferred document symbols warmup'
		);

		await waitForClientQuiescent('perf deferred baseline quiescent');
		const drained = await drainMetricsWindow('perf deferred baseline');
		const mediumSemanticRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentSemanticTokens',
				mediumSemanticDocument.uri
			);
			assert.ok(result && result.data.length > 0, 'Expected semantic tokens for medium deferred fixture.');
			return result.data.length;
		});
		const largeSemanticRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentSemanticTokens',
				largeDocument.uri
			);
			assert.ok(result && result.data.length > 0, 'Expected semantic tokens for large deferred fixture.');
			return result.data.length;
		});
		const mediumInlayRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeInlayHintProvider',
				mediumInlayDocument.uri,
				mediumInlayRange
			);
			assert.ok(Array.isArray(result) && result.length > 0, 'Expected inlay hints for medium deferred fixture.');
			return result.length;
		});
		const largeInlayRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeInlayHintProvider',
				largeDocument.uri,
				largeRange
			);
			assert.ok(Array.isArray(result) && result.length > 0, 'Expected inlay hints for large deferred fixture.');
			return result.length;
		});
		const largeDocumentSymbolsRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeDocumentSymbolProvider',
				largeDocument.uri
			);
			assert.ok(Array.isArray(result) && result.length > 0, 'Expected document symbols for large deferred fixture.');
			return result.length;
		});

		await triggerNoOpEdit(mediumDiagnosticsDocument);
		await triggerNoOpEdit(largeDocument);
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf deferred baseline metrics flush'
		);

		const report = {
			scenario: 'm0-idle-deferred-baseline',
			fixtures: [PERF_FIXTURES.pfx2MediumEditPath, PERF_FIXTURES.pfx3LargeCurrentDoc],
			loadMode: 'Idle',
			iterations,
			wallClock: {
				semanticTokensMedium: computeLatencyStats(mediumSemanticRun.samples),
				semanticTokensLarge: computeLatencyStats(largeSemanticRun.samples),
				inlayMedium: computeLatencyStats(mediumInlayRun.samples),
				inlayLarge: computeLatencyStats(largeInlayRun.samples),
				documentSymbolsLarge: computeLatencyStats(largeDocumentSymbolsRun.samples)
			},
			metrics
		};
		writePerfReport('m0-idle-deferred-baseline', report);

		assert.ok((metrics.payload?.methods?.[semanticTokensMethodKey]?.count ?? 0) >= iterations * 2);
		assert.ok((metrics.payload?.methods?.[inlayMethodKey]?.count ?? 0) >= iterations * 2);
		assert.ok(largeDocumentSymbolsRun.samples.length >= iterations);
		assert.ok((metrics.payload?.diagnostics?.count ?? 0) > 0);
	});

	it('captures cross-file file-watch diagnostic refresh baseline', async function () {
		this.timeout(180000);

		await waitForIndexingIdle('perf file-watch baseline indexing idle');
		const providerPath = path.join(getWorkspaceRoot(), 'test_files', 'watch_provider.nsf');
		const original = fs.readFileSync(providerPath, 'utf8');
		const consumerDocument = await openFixture(PERF_FIXTURES.pfx5ReverseIncludeImpact.primaryDocument);

		try {
			await waitFor(
				() => vscode.languages.getDiagnostics(consumerDocument.uri),
				(value) => Array.isArray(value),
				'perf file-watch initial diagnostics'
			);

			const drained = await drainMetricsWindow('perf file-watch baseline');
			const startedAt = process.hrtime.bigint();
			await vscode.workspace.fs.writeFile(
				vscode.Uri.file(providerPath),
				Buffer.from(original.replace('u_value', 'u_value2'), 'utf8')
			);
			await triggerNoOpEdit(consumerDocument);
			const diagnostics = await waitFor(
				() => vscode.languages.getDiagnostics(consumerDocument.uri),
				(value) =>
					Array.isArray(value) &&
					value.some((diagnostic) => diagnostic.message.includes('Undefined identifier: u_value.')),
				'perf file-watch updated diagnostics'
			);
			const elapsedMs = Number(process.hrtime.bigint() - startedAt) / 1_000_000;
			const metrics = await waitForNextMetricsRevision(
				drained.revision ?? 0,
				'perf file-watch metrics flush'
			);

			const report = {
				scenario: 'm0-load-ws-file-watch-baseline',
				fixture: PERF_FIXTURES.pfx5ReverseIncludeImpact,
				loadMode: 'Load-WS',
				wallClock: {
					diagnosticsRefreshMs: elapsedMs
				},
				diagnosticsCount: diagnostics.length,
				metrics
			};
			writePerfReport('m0-load-ws-file-watch-baseline', report);

			assert.ok((metrics.payload?.diagnostics?.count ?? 0) > 0);
		} finally {
			await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(original, 'utf8'));
		}
	});
});

perfDescribe('NSF perf baseline: M2 immediate syntax runtime', () => {
	it('keeps unmatched-bracket immediate diagnostics within budget at idle', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_M2_ITERATIONS', 6, 2, 40);
		await waitForIndexingIdle('perf m2 idle indexing idle');

		let document = await openFixture('module_diagnostics_unmatched_brackets.nsf');
		const diagnosticMessage = 'Unterminated bracket';
		document = await insertTextAt(document, positionOf(document, 'uv;', 1, 'uv'.length), ')');
		await waitForDiagnosticMessagePresenceFast(
			document,
			diagnosticMessage,
			false,
			'perf m2 initial diagnostics cleared'
		);

		try {
			const drained = await drainMetricsWindow('perf m2 idle immediate syntax');
			const samples: number[] = [];
			for (let index = 0; index < iterations; index++) {
				const deleteStart = positionOf(document, 'uv);', 1, 'uv'.length);
				const startedAt = process.hrtime.bigint();
				document = await deleteRangeFromDocument(
					document,
					new vscode.Range(deleteStart, deleteStart.translate(0, 1))
				);
				await waitForDiagnosticMessagePresenceFast(
					document,
					diagnosticMessage,
					true,
					`perf m2 idle unmatched bracket ${index}`,
					320,
					20
				);
				samples.push(Number(process.hrtime.bigint() - startedAt) / 1_000_000);

				document = await insertTextAt(document, positionOf(document, 'uv;', 1, 'uv'.length), ')');
				await waitForDiagnosticMessagePresenceFast(
					document,
					diagnosticMessage,
					false,
					`perf m2 idle unmatched bracket cleared ${index}`,
					320,
					20
				);
				await waitForClientQuiescent(`perf m2 idle cycle settled ${index}`);
			}

			const metrics = await waitForNextMetricsRevision(
				drained.revision ?? 0,
				'perf m2 idle immediate syntax metrics flush'
			);
			const stats = computeLatencyStats(samples);
			const report = {
				scenario: 'm2-idle-immediate-syntax-bracket',
				fixture: { id: 'PFX-2D', label: 'ImmediateSyntaxBracket', primaryDocument: 'module_diagnostics_unmatched_brackets.nsf' },
				loadMode: 'Idle',
				iterations,
				wallClock: {
					immediateSyntax: stats
				},
				metrics
			};
			writePerfReport('m2-idle-immediate-syntax-bracket', report);

			assert.ok(
				stats.p95Ms > 0,
				`Expected immediate syntax diagnostics to produce latency samples. Actual=${stats.p95Ms.toFixed(1)}ms`
			);
			assert.ok((metrics.payload?.diagnostics?.count ?? 0) > 0);
		} finally {
			if (document.getText().includes('uv);')) {
				document = await deleteRangeFromDocument(
					document,
					new vscode.Range(
						positionOf(document, 'uv);', 1, 'uv'.length),
						positionOf(document, 'uv);', 1, 'uv'.length).translate(0, 1)
					)
				);
			}
		}
	});

	it('keeps unmatched-bracket immediate diagnostics stable under background deferred load', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_M2_BG_ITERATIONS', 6, 2, 32);
		const spamCount = readPerfIntEnv('NSF_PERF_BG_SPAM_COUNT', 36, 4, 160);
		const warmupSpamCount = readPerfIntEnv('NSF_PERF_M2_BG_WARMUP_SPAM_COUNT', 6, 1, 40);
		const diagnosticMessage = 'Unterminated bracket';
		await waitForIndexingIdle('perf m2 load-bg indexing idle');

		let document = await openFixture('module_diagnostics_unmatched_brackets.nsf');
		document = await insertTextAt(document, positionOf(document, 'uv;', 1, 'uv'.length), ')');
		await waitForDiagnosticMessagePresenceFast(
			document,
			diagnosticMessage,
			false,
			'perf m2 load-bg initial diagnostics cleared'
		);

		const idleSamples: number[] = [];
		for (let index = 0; index < iterations; index++) {
			const deleteStart = positionOf(document, 'uv);', 1, 'uv'.length);
			const startedAt = process.hrtime.bigint();
			document = await deleteRangeFromDocument(
				document,
				new vscode.Range(deleteStart, deleteStart.translate(0, 1))
			);
				await waitForDiagnosticMessagePresenceFast(
					document,
					diagnosticMessage,
					true,
					`perf m2 idle baseline unmatched bracket ${index}`,
					320,
					20
				);
			idleSamples.push(Number(process.hrtime.bigint() - startedAt) / 1_000_000);

			document = await insertTextAt(document, positionOf(document, 'uv;', 1, 'uv'.length), ')');
				await waitForDiagnosticMessagePresenceFast(
					document,
					diagnosticMessage,
					false,
					`perf m2 idle baseline unmatched bracket cleared ${index}`,
					320,
					20
				);
		}
		const idleStats = computeLatencyStats(idleSamples);

		const backgroundDocument = await openFixture(PERF_FIXTURES.pfx3LargeCurrentDoc.primaryDocument);
		const backgroundEndLine = Math.min(
			backgroundDocument.lineCount - 1,
			Math.max(80, Math.min(backgroundDocument.lineCount - 1, 160))
		);
		const backgroundRange = new vscode.Range(
			new vscode.Position(0, 0),
			new vscode.Position(backgroundEndLine, 0)
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<any[]>(
					'vscode.executeInlayHintProvider',
					backgroundDocument.uri,
					backgroundRange
				),
			(value) => Array.isArray(value),
			'perf m2 background inlay warmup'
		);
		const warmupSpamResult = await vscode.commands.executeCommand<{
			completed: number;
			cancelled: number;
			failed: number;
		}>('nsf._spamInlayRequests', {
			uri: backgroundDocument.uri.toString(),
			startLine: 0,
			startCharacter: 0,
			endLine: backgroundEndLine,
			endCharacter: 0,
			count: warmupSpamCount
		});
		assert.strictEqual(
			(warmupSpamResult?.completed ?? 0) +
				(warmupSpamResult?.cancelled ?? 0) +
				(warmupSpamResult?.failed ?? 0),
			warmupSpamCount
		);
		await waitForClientQuiescent('perf m2 load-bg baseline quiescent');
		const drained = await drainMetricsWindow('perf m2 load-bg immediate syntax');
		const spamPromise = vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
			'nsf._spamInlayRequests',
			{
				uri: backgroundDocument.uri.toString(),
				startLine: 0,
				startCharacter: 0,
				// Mirror the real provider's visible-range + prefetch shape instead of
				// flooding the entire large document with one giant response payload.
				endLine: backgroundEndLine,
				endCharacter: 0,
				count: spamCount
			}
		);
		// Let the background request flood enter a steady-state window before we
		// start timing immediate syntax edits against it.
		await new Promise((resolve) => setTimeout(resolve, 40));

		const loadSamples: number[] = [];
		try {
			for (let index = 0; index < iterations; index++) {
				const deleteStart = positionOf(document, 'uv);', 1, 'uv'.length);
				const startedAt = process.hrtime.bigint();
				document = await deleteRangeFromDocument(
					document,
					new vscode.Range(deleteStart, deleteStart.translate(0, 1))
				);
				await waitForDiagnosticMessagePresenceFast(
					document,
					diagnosticMessage,
					true,
					`perf m2 load-bg unmatched bracket ${index}`,
					320,
					20
				);
				loadSamples.push(Number(process.hrtime.bigint() - startedAt) / 1_000_000);

				document = await insertTextAt(document, positionOf(document, 'uv;', 1, 'uv'.length), ')');
				await waitForDiagnosticMessagePresenceFast(
					document,
					diagnosticMessage,
					false,
					`perf m2 load-bg unmatched bracket cleared ${index}`,
					320,
					20
				);
			}

			const spamResult = await spamPromise;
			const metrics = await waitForNextMetricsRevision(
				drained.revision ?? 0,
				'perf m2 load-bg immediate syntax metrics flush'
			);
			const loadStats = computeLatencyStats(loadSamples);
			const p95AbsoluteIncreaseMs = loadStats.p95Ms - idleStats.p95Ms;
			const report = {
				scenario: 'm2-load-bg-immediate-syntax-bracket',
				fixtures: [
					{ id: 'PFX-2D', label: 'ImmediateSyntaxBracket', primaryDocument: 'module_diagnostics_unmatched_brackets.nsf' },
					PERF_FIXTURES.pfx3LargeCurrentDoc
				],
				loadMode: 'Load-BG',
				iterations,
				spamCount,
				spamResult,
				wallClock: {
					idleImmediateSyntax: idleStats,
					loadImmediateSyntax: loadStats
				},
				p95AbsoluteIncreaseMs,
				metrics
			};
			writePerfReport('m2-load-bg-immediate-syntax-bracket', report);

			assert.strictEqual(
				(spamResult.completed ?? 0) + (spamResult.cancelled ?? 0) + (spamResult.failed ?? 0),
				spamCount
			);
			assert.ok(
				loadStats.p95Ms > 0,
				`Expected background immediate syntax diagnostics to produce latency samples. Actual=${loadStats.p95Ms.toFixed(1)}ms`
			);
			assert.ok(
				Number.isFinite(p95AbsoluteIncreaseMs),
				`Expected background immediate syntax comparison to produce a finite delta. Delta=${p95AbsoluteIncreaseMs.toFixed(1)}ms`
			);
			assert.ok((metrics.payload?.diagnostics?.count ?? 0) > 0);
		} finally {
			if (document.getText().includes('uv);')) {
				document = await deleteRangeFromDocument(
					document,
					new vscode.Range(
						positionOf(document, 'uv);', 1, 'uv'.length),
						positionOf(document, 'uv);', 1, 'uv'.length).translate(0, 1)
					)
				);
			}
		}
	});
});

perfDescribe('NSF perf baseline: M3 current-doc interactive runtime', () => {
	it('captures idle current-doc interactive latency across member completion, signature help, hover, and definition', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_ITERATIONS', 8, 1, 60);
		await waitForIndexingIdle('perf m3 idle indexing idle');

		const memberDocument = await openFixture('module_struct_completion.nsf');
		const memberPosition = positionOf(memberDocument, 'instance.', 1, 'instance.'.length);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					memberDocument.uri,
					memberPosition
				),
			(value) => {
				const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
				return labels.has('color') && labels.has('value');
			},
			'perf m3 member completion warmup'
		);

		const signatureDocument = await openFixture('module_signature_help.nsf');
		const signaturePosition = positionOf(signatureDocument, 'SigTarget(uv, 2.0, 3', 1, 'SigTarget(uv, '.length);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					signatureDocument.uri,
					signaturePosition
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'perf m3 signature help warmup'
		);

		const hoverDocument = await openFixture('module_hover_local_type_assignment.nsf');
		const hoverPosition = positionOf(hoverDocument, 'main_color2', 2, 2);
		await waitFor(
			() => vscode.commands.executeCommand<vscode.Hover[]>('vscode.executeHoverProvider', hoverDocument.uri, hoverPosition),
			(value) => Array.isArray(value) && hoverToText(value).includes('Type: half3'),
			'perf m3 hover warmup'
		);

		const definitionDocument = await openFixture('module_decls.nsf');
		const definitionPosition = positionOf(definitionDocument, 'SuiteParamMatrix', 2, 2);
		await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					definitionDocument.uri,
					definitionPosition
				),
			(value) =>
				Array.isArray(value) &&
				value.some((location) => providerLocationUri(location).toString() === definitionDocument.uri.toString()),
			'perf m3 definition warmup'
		);

		const drained = await drainMetricsWindow('perf m3 idle interactive');
		const memberRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				memberDocument.uri,
				memberPosition
			);
			const items = getCompletionItems(result);
			const labels = new Set(items.map((item) => item.label.toString()));
			assert.ok(labels.has('color'), 'Expected current-doc member completion to include color.');
			assert.ok(labels.has('value'), 'Expected current-doc member completion to include value.');
			return items.length;
		});
		const signatureRun = await measureLatencySamples(iterations, async () => {
			const help = await vscode.commands.executeCommand<vscode.SignatureHelp>(
				'vscode.executeSignatureHelpProvider',
				signatureDocument.uri,
				signaturePosition
			);
			assert.ok(help && help.signatures.length > 0, 'Expected signature help during M3 perf scenario.');
			assert.ok(help.signatures[help.activeSignature].label.includes('SigTarget'));
			return help.signatures.length;
		});
		const hoverRun = await measureLatencySamples(iterations, async () => {
			const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
				'vscode.executeHoverProvider',
				hoverDocument.uri,
				hoverPosition
			);
			assert.ok(Array.isArray(hovers) && hovers.length > 0, 'Expected hover results during M3 perf scenario.');
			const text = hoverToText(hovers);
			assert.ok(text.includes('Type: half3'), text);
			return text.length;
		});
		const definitionRun = await measureLatencySamples(iterations, async () => {
			const locations = await vscode.commands.executeCommand<ProviderLocation[]>(
				'vscode.executeDefinitionProvider',
				definitionDocument.uri,
				definitionPosition
			);
			assert.ok(Array.isArray(locations) && locations.length > 0, 'Expected definition results during M3 perf scenario.');
			assert.ok(
				locations.some((location) => providerLocationUri(location).toString() === definitionDocument.uri.toString()),
				'Expected current-doc definition result to stay in the same file.'
			);
			return locations.length;
		});
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf m3 idle interactive metrics flush'
		);

		const report = {
			scenario: 'm3-idle-current-doc-interactive',
			fixtures: [
				PERF_FIXTURES.pfx1SmallCurrentDoc,
				{ id: 'PFX-1C', label: 'StructCompletion', primaryDocument: 'module_struct_completion.nsf' },
				{ id: 'PFX-1D', label: 'SignatureHelp', primaryDocument: 'module_signature_help.nsf' },
				{ id: 'PFX-2B', label: 'HoverLocalType', primaryDocument: 'module_hover_local_type_assignment.nsf' },
				{ id: 'PFX-2C', label: 'CurrentDocDefinition', primaryDocument: 'module_decls.nsf' }
			],
			loadMode: 'Idle',
			iterations,
			wallClock: {
				memberCompletion: computeLatencyStats(memberRun.samples),
				signatureHelp: computeLatencyStats(signatureRun.samples),
				hover: computeLatencyStats(hoverRun.samples),
				definition: computeLatencyStats(definitionRun.samples)
			},
			metrics
		};
		writePerfReport('m3-idle-current-doc-interactive', report);

		assert.ok((metrics.payload?.methods?.[completionMethodKey]?.count ?? 0) >= iterations);
		assert.ok((metrics.payload?.methods?.[signatureHelpMethodKey]?.count ?? 0) >= iterations);
		assert.ok((metrics.payload?.methods?.[hoverMethodKey]?.count ?? 0) >= iterations);
		assert.ok((metrics.payload?.methods?.[definitionMethodKey]?.count ?? 0) >= iterations);
		assert.ok((metrics.payload?.interactiveRuntime?.mergeCurrentDocHits ?? 0) > 0);
	});

	it('captures current-doc snapshot reuse under background deferred-doc load', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_INTERACTIVE_ITERATIONS', 8, 1, 40);
		const spamCount = readPerfIntEnv('NSF_PERF_BG_SPAM_COUNT', 24, 1, 120);
		await waitForIndexingIdle('perf m3 load-bg indexing idle');

		let interactiveDocument = await openFixture(PERF_FIXTURES.pfx1SmallCurrentDoc.primaryDocument);
		const completionPosition = positionOf(
			interactiveDocument,
			'return Comp',
			1,
			'return Comp'.length
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					interactiveDocument.uri,
					completionPosition
				),
			(value) => {
				const labels = new Set(getCompletionItems(value).map((item) => item.label.toString()));
				return labels.has('CompletionDocHelper') && labels.has('completionLocalColor');
			},
			'perf m3 snapshot reuse warmup'
		);

		const backgroundDocument = await openFixture(PERF_FIXTURES.pfx3LargeCurrentDoc.primaryDocument);
		const drained = await drainMetricsWindow('perf m3 load-bg snapshot reuse');
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

		const completionSamples: number[] = [];
		for (let index = 0; index < iterations; index++) {
			const editResult = await appendTrailingCommentEdit(interactiveDocument, `m3 perf touch ${index}`);
			interactiveDocument = editResult.document;

			const startedAt = process.hrtime.bigint();
			const completionResult = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				interactiveDocument.uri,
				completionPosition
			);
			const elapsedMs = Number(process.hrtime.bigint() - startedAt) / 1_000_000;
			const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
			assert.ok(labels.has('CompletionDocHelper'));
			assert.ok(labels.has('CompletionDocEntry'));
			assert.ok(labels.has('CompletionDocGlobal'));
			assert.ok(labels.has('completionLocalColor'));
			completionSamples.push(elapsedMs);

			interactiveDocument = await deleteTrailingCommentEdit(interactiveDocument, editResult.insertedRange);
		}

		const spamResult = await spamPromise;
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf m3 load-bg snapshot reuse metrics flush'
		);

		const report = {
			scenario: 'm3-load-bg-current-doc-snapshot-reuse',
			fixtures: [PERF_FIXTURES.pfx1SmallCurrentDoc, PERF_FIXTURES.pfx3LargeCurrentDoc],
			loadMode: 'Load-BG',
			iterations,
			spamCount,
			spamResult,
			wallClock: {
				completionAfterNeutralEdits: computeLatencyStats(completionSamples)
			},
			metrics
		};
		writePerfReport('m3-load-bg-current-doc-snapshot-reuse', report);

		assert.strictEqual(
			(spamResult?.completed ?? 0) + (spamResult?.cancelled ?? 0) + (spamResult?.failed ?? 0),
			spamCount
		);
		assert.strictEqual(metrics.payload?.interactiveRuntime?.noSnapshotAvailable ?? 0, 0);
		assert.ok((metrics.payload?.methods?.[completionMethodKey]?.count ?? 0) >= iterations);
		assert.ok(
			(metrics.payload?.interactiveRuntime?.analysisKeyHits ?? 0) +
				(metrics.payload?.interactiveRuntime?.lastGoodServed ?? 0) +
				(metrics.payload?.interactiveRuntime?.incrementalPromoted ?? 0) >
				0,
			'Expected M3 perf scenario to reuse current or last-good interactive snapshots.'
		);
		assert.ok(
			(metrics.payload?.interactiveRuntime?.mergeCurrentDocHits ?? 0) +
				(metrics.payload?.interactiveRuntime?.mergeLastGoodHits ?? 0) >=
				iterations,
			'Expected current-doc or last-good interactive merges during M3 perf scenario.'
		);
		assert.ok(
			(metrics.payload?.methods?.[inlayMethodKey]?.count ?? 0) > 0,
			'Expected background inlay load during the M3 snapshot reuse scenario.'
		);
	});
});

perfDescribe('NSF perf baseline: M4 shared analysis context and deferred-doc runtime', () => {
	it('keeps active-unit switch interactive results within budget', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_M4_ITERATIONS', 4, 2, 20);
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
			await waitForClientReady('perf m4 client ready');
			await waitForIndexingIdle('perf m4 active-unit indexing idle');

			const unitADocument = await openFixture('include_context/units/multi_context_symbol_a.nsf');
			const unitBDocument = await openFixture('include_context/units/multi_context_symbol_b.nsf');
			const sharedDocument = await openFixture(PERF_FIXTURES.pfx4ActiveUnitAmbiguous.primaryDocument);
			const sharedPosition = positionOf(sharedDocument, 'UnitSpecificTone', 1, 2);

			const waitForDefinition = async (expectedFileName: string, label: string) =>
				waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							sharedDocument.uri,
							sharedPosition
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) => path.basename(toFsPath(location)) === expectedFileName),
					label
				);

			const waitForHover = async (label: string) =>
				waitFor(
					() =>
						vscode.commands.executeCommand<vscode.Hover[]>(
							'vscode.executeHoverProvider',
							sharedDocument.uri,
							sharedPosition
						),
					(value) => Array.isArray(value) && hoverToText(value).includes('UnitSpecificTone('),
					label
				);

			try {
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', unitADocument.uri.toString());
				await waitForDefinition('multi_context_symbol_a.nsf', 'perf m4 warmup definition unit A');
				await waitForHover('perf m4 warmup hover unit A');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', unitBDocument.uri.toString());
				await waitForDefinition('multi_context_symbol_b.nsf', 'perf m4 warmup definition unit B');
				await waitForHover('perf m4 warmup hover unit B');

				const drained = await drainMetricsWindow('perf m4 active-unit switch');
				const definitionSamples: number[] = [];
				const hoverSamples: number[] = [];
				for (let index = 0; index < iterations; index++) {
					const targetDocument = index % 2 === 0 ? unitADocument : unitBDocument;
					const expectedFileName =
						index % 2 === 0 ? 'multi_context_symbol_a.nsf' : 'multi_context_symbol_b.nsf';

					const definitionStartedAt = process.hrtime.bigint();
					await vscode.commands.executeCommand('nsf._setActiveUnitForTests', targetDocument.uri.toString());
					await waitForDefinition(expectedFileName, `perf m4 definition switch ${index}`);
					definitionSamples.push(Number(process.hrtime.bigint() - definitionStartedAt) / 1_000_000);

					const hoverStartedAt = process.hrtime.bigint();
					await waitForHover(`perf m4 hover switch ${index}`);
					hoverSamples.push(Number(process.hrtime.bigint() - hoverStartedAt) / 1_000_000);
				}

				const metrics = await waitForNextMetricsRevision(
					drained.revision ?? 0,
					'perf m4 active-unit switch metrics flush'
				);
				const definitionStats = computeLatencyStats(definitionSamples);
				const hoverStats = computeLatencyStats(hoverSamples);
				const report = {
					scenario: 'm4-active-unit-shared-context-switch',
					fixture: PERF_FIXTURES.pfx4ActiveUnitAmbiguous,
					loadMode: 'Idle',
					iterations,
					wallClock: {
						definition: definitionStats,
						hover: hoverStats
					},
					metrics
				};
				writePerfReport('m4-active-unit-shared-context-switch', report);

				assert.ok(
					definitionStats.p95Ms <= 250,
					`Expected active-unit switch definition p95 <= 250ms. Actual=${definitionStats.p95Ms.toFixed(1)}ms`
				);
				assert.ok(
					hoverStats.p95Ms <= 250,
					`Expected active-unit switch hover p95 <= 250ms. Actual=${hoverStats.p95Ms.toFixed(1)}ms`
				);
				assert.ok((metrics.payload?.methods?.[definitionMethodKey]?.count ?? 0) >= iterations);
				assert.ok((metrics.payload?.methods?.[hoverMethodKey]?.count ?? 0) >= iterations);
			} finally {
				await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
			}
		});
	});

	it('keeps deferred semantic tokens and inlay budgets within target on medium and large docs', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_M4_DEFERRED_ITERATIONS', 4, 2, 20);
		await waitForIndexingIdle('perf m4 deferred-doc indexing idle');

		const mediumSemanticDocument = await openFixture(PERF_FIXTURES.pfx2MediumEditPath.semanticTokensDocument!);
		const mediumInlayDocument = await openFixture(PERF_FIXTURES.pfx2MediumEditPath.inlayDocument!);
		const largeDocument = await openFixture(PERF_FIXTURES.pfx3LargeCurrentDoc.primaryDocument);
		const mediumInlayRange = wholeDocumentRange(mediumInlayDocument);
		const largeRange = wholeDocumentRange(largeDocument);

		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SemanticTokensLegend>(
					'vscode.provideDocumentSemanticTokensLegend',
					mediumSemanticDocument.uri
				),
			(value) => Array.isArray(value?.tokenTypes) && value.tokenTypes.length > 0,
			'perf m4 semantic legend'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SemanticTokens>(
					'vscode.provideDocumentSemanticTokens',
					mediumSemanticDocument.uri
				),
			(value) => Boolean(value) && value.data.length > 0,
			'perf m4 medium semantic warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SemanticTokens>(
					'vscode.provideDocumentSemanticTokens',
					largeDocument.uri
				),
			(value) => Boolean(value) && value.data.length > 0,
			'perf m4 large semantic warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<any[]>(
					'vscode.executeInlayHintProvider',
					mediumInlayDocument.uri,
					mediumInlayRange
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf m4 medium inlay warmup'
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<any[]>(
					'vscode.executeInlayHintProvider',
					largeDocument.uri,
					largeRange
				),
			(value) => Array.isArray(value) && value.length > 0,
			'perf m4 large inlay warmup'
		);

		// Prime the deferred caches twice more before starting measured samples.
		for (let index = 0; index < 2; index++) {
			await vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentSemanticTokens',
				mediumSemanticDocument.uri
			);
			await vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentSemanticTokens',
				largeDocument.uri
			);
			await vscode.commands.executeCommand<any[]>(
				'vscode.executeInlayHintProvider',
				mediumInlayDocument.uri,
				mediumInlayRange
			);
			await vscode.commands.executeCommand<any[]>(
				'vscode.executeInlayHintProvider',
				largeDocument.uri,
				largeRange
			);
		}

		const drained = await drainMetricsWindow('perf m4 deferred-doc budgets');
		const mediumSemanticRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentSemanticTokens',
				mediumSemanticDocument.uri
			);
			assert.ok(result && result.data.length > 0, 'Expected medium semantic tokens during M4 perf scenario.');
			return result.data.length;
		});
		const largeSemanticRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentSemanticTokens',
				largeDocument.uri
			);
			assert.ok(result && result.data.length > 0, 'Expected large semantic tokens during M4 perf scenario.');
			return result.data.length;
		});
		const mediumInlayRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeInlayHintProvider',
				mediumInlayDocument.uri,
				mediumInlayRange
			);
			assert.ok(Array.isArray(result) && result.length > 0, 'Expected medium inlay hints during M4 perf scenario.');
			return result.length;
		});
		const largeInlayRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<any[]>(
				'vscode.executeInlayHintProvider',
				largeDocument.uri,
				largeRange
			);
			assert.ok(Array.isArray(result) && result.length > 0, 'Expected large inlay hints during M4 perf scenario.');
			return result.length;
		});
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf m4 deferred-doc budgets metrics flush'
		);

		const mediumSemanticStats = computeLatencyStats(mediumSemanticRun.samples);
		const largeSemanticStats = computeLatencyStats(largeSemanticRun.samples);
		const mediumInlayStats = computeLatencyStats(mediumInlayRun.samples);
		const largeInlayStats = computeLatencyStats(largeInlayRun.samples);
		const report = {
			scenario: 'm4-idle-deferred-doc-budgets',
			fixtures: [PERF_FIXTURES.pfx2MediumEditPath, PERF_FIXTURES.pfx3LargeCurrentDoc],
			loadMode: 'Idle',
			iterations,
			wallClock: {
				semanticTokensMedium: mediumSemanticStats,
				semanticTokensLarge: largeSemanticStats,
				inlayMedium: mediumInlayStats,
				inlayLarge: largeInlayStats
			},
			metrics
		};
		writePerfReport('m4-idle-deferred-doc-budgets', report);

		assert.ok(
			mediumSemanticStats.p95Ms <= 400,
			`Expected medium semantic tokens p95 <= 400ms. Actual=${mediumSemanticStats.p95Ms.toFixed(1)}ms`
		);
		assert.ok(
			largeSemanticStats.p95Ms <= 700 ||
				(largeSemanticStats.maxMs <= 900 && largeSemanticStats.avgMs <= 550),
			`Expected large semantic tokens within budget. p95=${largeSemanticStats.p95Ms.toFixed(1)}ms avg=${largeSemanticStats.avgMs.toFixed(1)}ms max=${largeSemanticStats.maxMs.toFixed(1)}ms`
		);
		assert.ok(
			mediumInlayStats.p95Ms <= 500,
			`Expected medium inlay hints p95 <= 500ms. Actual=${mediumInlayStats.p95Ms.toFixed(1)}ms`
		);
		assert.ok(
			largeInlayStats.p95Ms <= 800,
			`Expected large inlay hints p95 <= 800ms. Actual=${largeInlayStats.p95Ms.toFixed(1)}ms`
		);
		assert.ok((metrics.payload?.methods?.[semanticTokensMethodKey]?.count ?? 0) >= iterations * 2);
		assert.ok((metrics.payload?.methods?.[inlayMethodKey]?.count ?? 0) >= iterations * 2);
	});
});

perfDescribe('NSF perf baseline: M1 scheduling isolation', () => {
	it('keeps completion within budget under background deferred pressure', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_M1_ITERATIONS', 10, 4, 80);
		const spamCount = readPerfIntEnv('NSF_PERF_BG_SPAM_COUNT', 36, 4, 160);
		const warmupSpamCount = readPerfIntEnv('NSF_PERF_M1_BG_WARMUP_SPAM_COUNT', 6, 1, 40);
		await waitForIndexingIdle('perf m1 load-bg indexing idle');

		const document = await openFixture(PERF_FIXTURES.pfx2MediumEditPath.primaryDocument);
		const completionPosition = new vscode.Position(0, 0);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'perf m1 completion warmup'
		);

		const idleRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				document.uri,
				completionPosition
			);
			assert.ok(getCompletionItems(result).length > 0, 'Expected idle completion items for M1 baseline.');
			return getCompletionItems(result).length;
		});
		const idleStats = computeLatencyStats(idleRun.samples);

		const warmupSpamResult = await vscode.commands.executeCommand<{
			completed: number;
			cancelled: number;
			failed: number;
		}>('nsf._spamInlayRequests', {
			uri: document.uri.toString(),
			startLine: 0,
			startCharacter: 0,
			endLine: Math.max(50, document.lineCount + 5),
			endCharacter: 0,
			count: warmupSpamCount
		});
		assert.strictEqual(
			(warmupSpamResult?.completed ?? 0) +
				(warmupSpamResult?.cancelled ?? 0) +
				(warmupSpamResult?.failed ?? 0),
			warmupSpamCount
		);

		const drained = await drainMetricsWindow('perf m1 load-bg completion');
		const spamPromise = vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
			'nsf._spamInlayRequests',
			{
				uri: document.uri.toString(),
				startLine: 0,
				startCharacter: 0,
				endLine: Math.max(50, document.lineCount + 5),
				endCharacter: 0,
				count: spamCount
			}
		);
		await new Promise((resolve) => setTimeout(resolve, 40));
		const loadRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				document.uri,
				completionPosition
			);
			assert.ok(getCompletionItems(result).length > 0, 'Expected completion items under M1 background load.');
			return getCompletionItems(result).length;
		});
		const spamResult = await spamPromise;
		const metrics = await waitForNextMetricsRevision(
			drained.revision ?? 0,
			'perf m1 load-bg completion metrics flush'
		);
		const loadStats = computeLatencyStats(loadRun.samples);
		const p95DegradationRatio = computeP95DegradationRatio(idleStats, loadStats);
		const p95AbsoluteIncreaseMs = loadStats.p95Ms - idleStats.p95Ms;

		const report = {
			scenario: 'm1-load-bg-completion-isolation',
			fixture: PERF_FIXTURES.pfx2MediumEditPath,
			loadMode: 'Load-BG',
			iterations,
			spamCount,
			spamResult,
			wallClock: {
				idleCompletion: idleStats,
				loadCompletion: loadStats
			},
			p95DegradationRatio,
			metrics
		};
		writePerfReport('m1-load-bg-completion-isolation', report);

		assert.strictEqual(
			(spamResult.completed ?? 0) + (spamResult.cancelled ?? 0) + (spamResult.failed ?? 0),
			spamCount
		);
		assert.ok(loadStats.p95Ms <= 100, `Expected completion p95 <= 100ms under Load-BG. Actual=${loadStats.p95Ms.toFixed(1)}ms`);
		assert.ok(
			p95DegradationRatio <= 0.15 || p95AbsoluteIncreaseMs <= 8,
			`Expected completion p95 degradation <= 15% or absolute increase <= 8ms. Idle=${idleStats.p95Ms.toFixed(1)}ms Load=${loadStats.p95Ms.toFixed(1)}ms Ratio=${(p95DegradationRatio * 100).toFixed(1)}% Delta=${p95AbsoluteIncreaseMs.toFixed(1)}ms`
		);
		assert.ok((metrics.payload?.methods?.[completionMethodKey]?.count ?? 0) >= iterations);
		assert.ok((metrics.payload?.methods?.[inlayMethodKey]?.count ?? 0) > 0);
		assert.strictEqual(metrics.payload?.interactiveRuntime?.noSnapshotAvailable ?? 0, 0);
	});

	it('keeps hover within budget while workspace-summary churn runs', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_M1_WS_ITERATIONS', 8, 4, 40);
		const churnCount = readPerfIntEnv('NSF_PERF_WS_CHURN_COUNT', 8, 2, 40);
		await waitForIndexingIdle('perf m1 load-ws indexing idle');

		const hoverDocument = await openFixture('module_hover_docs.nsf');
		const hoverPosition = positionOf(hoverDocument, 'SuiteHoverFunc', 2, 2);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					hoverDocument.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && hoverToText(value).includes('SuiteHoverFunc('),
			'perf m1 hover warmup'
		);

		const providerPath = path.join(getWorkspaceRoot(), 'test_files', 'watch_provider.nsf');
		const consumerDocument = await openFixture(PERF_FIXTURES.pfx5ReverseIncludeImpact.primaryDocument);
		await waitFor(
			() => vscode.languages.getDiagnostics(consumerDocument.uri),
			(value) => Array.isArray(value),
			'perf m1 file-watch consumer diagnostics'
		);

		let originalProviderText = '';
		try {
			const drained = await drainMetricsWindow('perf m1 load-ws hover');
			const churnPromise = churnWorkspaceWatchLoad(providerPath, consumerDocument, churnCount).then((result) => {
				originalProviderText = result.original;
				return result;
			});

			const hoverSamples: number[] = [];
			for (let index = 0; index < iterations; index++) {
				const startedAt = process.hrtime.bigint();
				const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					hoverDocument.uri,
					hoverPosition
				);
				const elapsedMs = Number(process.hrtime.bigint() - startedAt) / 1_000_000;
				assert.ok(Array.isArray(hovers) && hovers.length > 0, 'Expected hover results during workspace-summary churn.');
				const text = hoverToText(hovers);
				assert.ok(text.includes('SuiteHoverFunc('), text);
				hoverSamples.push(elapsedMs);
				await new Promise((resolve) => setTimeout(resolve, 60));
			}

			await churnPromise;
			const metrics = await waitForNextMetricsRevision(
				drained.revision ?? 0,
				'perf m1 load-ws hover metrics flush'
			);
			const hoverStats = computeLatencyStats(hoverSamples);

			const report = {
				scenario: 'm1-load-ws-hover-isolation',
				fixtures: [
					{ id: 'PFX-1B', label: 'HoverDocs', primaryDocument: 'module_hover_docs.nsf' },
					PERF_FIXTURES.pfx5ReverseIncludeImpact
				],
				loadMode: 'Load-WS',
				iterations,
				churnCount,
				wallClock: {
					hover: hoverStats
				},
				metrics
			};
			writePerfReport('m1-load-ws-hover-isolation', report);

			assert.ok(hoverStats.p95Ms <= 120, `Expected hover p95 <= 120ms under Load-WS. Actual=${hoverStats.p95Ms.toFixed(1)}ms`);
			assert.ok((metrics.payload?.methods?.[hoverMethodKey]?.count ?? 0) >= iterations);
			assert.ok((metrics.payload?.diagnostics?.count ?? 0) > 0);
			assert.ok(
				(metrics.payload?.deferredDocRuntime?.buildCount ?? 0) > 0,
				'Expected workspace-summary churn to keep deferred-doc work active while hover stays within budget.'
			);
		} finally {
			if (originalProviderText.length > 0) {
				await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(originalProviderText, 'utf8'));
			}
		}
	});

	it('records latest-only deferred merges during rapid same-document edit bursts', async function () {
		this.timeout(240000);

		const burstEdits = readPerfIntEnv('NSF_PERF_LATEST_ONLY_EDITS', 24, 6, 120);
		await waitForIndexingIdle('perf m1 latest-only indexing idle');
		await waitForClientQuiescent('perf m1 latest-only quiescent');

		let document = await openFixture(PERF_FIXTURES.pfx3LargeCurrentDoc.primaryDocument);
		const burstStartOffset = document.getText().length;
		const drained = await drainMetricsWindow('perf m1 latest-only burst');

		for (let index = 0; index < burstEdits; index++) {
			const editResult = await appendTrailingLiteralEdit(
				document,
				`\nfloat m1_latest_only_${index} = ${index}.0;`
			);
			document = editResult.document;
			if (index === 0) {
				await waitFor(
					() => getDocumentRuntimeDebug([document.uri.toString()]),
					(entries) =>
						Array.isArray(entries) &&
						entries.length > 0 &&
						(entries[0]?.version ?? -1) >= document.version,
					'perf m1 runtime catch-up after first edit'
				);
			}
		}

		const metricWindows = await waitForMetricsHistorySinceRevision(
			drained.revision ?? 0,
			(snapshots) =>
				snapshots.reduce(
					(sum, snapshot) =>
						sum + (snapshot.payload?.deferredDocRuntime?.scheduled ?? 0),
					0
				) > 0,
			'perf m1 latest-only burst metrics history'
		);
		const aggregateDeferredDocRuntime = metricWindows.reduce(
			(acc, snapshot) => {
				const deferredDocRuntime = snapshot.payload?.deferredDocRuntime;
				acc.scheduled += deferredDocRuntime?.scheduled ?? 0;
				acc.mergedLatestOnly += deferredDocRuntime?.mergedLatestOnly ?? 0;
				acc.droppedStale += deferredDocRuntime?.droppedStale ?? 0;
				acc.buildCount += deferredDocRuntime?.buildCount ?? 0;
				return acc;
			},
			{ scheduled: 0, mergedLatestOnly: 0, droppedStale: 0, buildCount: 0 }
		);
		const latestMetrics = metricWindows[metricWindows.length - 1];
		const latestOnlyMerged = aggregateDeferredDocRuntime.mergedLatestOnly;
		const latestOnlyDropped = aggregateDeferredDocRuntime.droppedStale;
		const latestOnlyCollapseRate =
			aggregateDeferredDocRuntime.scheduled > 0
				? (latestOnlyMerged + latestOnlyDropped) /
					aggregateDeferredDocRuntime.scheduled
				: 0;

		const report = {
			scenario: 'm1-latest-only-edit-burst',
			fixture: PERF_FIXTURES.pfx3LargeCurrentDoc,
			loadMode: 'Load-BG',
			burstEdits,
			latestOnlyCollapseRate,
			metricsWindowCount: metricWindows.length,
			aggregateDeferredDocRuntime,
			metrics: latestMetrics
		};
		writePerfReport('m1-latest-only-edit-burst', report);

		assert.ok(
			aggregateDeferredDocRuntime.scheduled > 0,
			'Expected rapid same-document edits to schedule deferred-doc work.'
		);
		assert.ok(
			latestOnlyMerged + latestOnlyDropped > 0,
			'Expected rapid same-document edits to trigger latest-only collapse or stale-drop.'
		);
		assert.ok(
			latestOnlyCollapseRate >= 0.25,
			`Expected latest-only collapse rate >= 25%. Actual=${(latestOnlyCollapseRate * 100).toFixed(1)}%`
		);

		const burstEndOffset = document.getText().length;
		if (burstEndOffset > burstStartOffset) {
			await deleteTrailingCommentEdit(
				document,
				new vscode.Range(document.positionAt(burstStartOffset), document.positionAt(burstEndOffset))
			);
		}
	});
});

perfDescribe('NSF perf baseline: M5 workspace summary and reverse-include', () => {
	it('captures idle references and rename latency through workspace summary', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_M5_ITERATIONS', 4, 1, 20);
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('perf m5 idle references indexing idle');
			const document = await openFixture('module_suite.nsf');
			const symbolPosition = positionOf(document, 'SuiteMakeSharedColor', 1, 2);

			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.Location[]>(
						'vscode.executeReferenceProvider',
						document.uri,
						symbolPosition
					),
				(value) => Array.isArray(value) && value.length >= 3,
				'perf m5 references warmup'
			);
			await waitFor(
				() =>
					vscode.commands.executeCommand<{ range: vscode.Range; placeholder: string }>(
						'vscode.prepareRename',
						document.uri,
						symbolPosition
					),
				(value) => Boolean(value?.placeholder),
				'perf m5 prepare rename warmup'
			);

			const drained = await drainMetricsWindow('perf m5 idle references rename');
			const referencesRun = await measureLatencySamples(iterations, async () => {
				const references = await vscode.commands.executeCommand<vscode.Location[]>(
					'vscode.executeReferenceProvider',
					document.uri,
					symbolPosition
				);
				assert.ok(Array.isArray(references) && references.length >= 3, 'Expected cross-file references for M5 perf scenario.');
				return references.length;
			});
			const prepareRenameRun = await measureLatencySamples(iterations, async () => {
				const prepareResult = await vscode.commands.executeCommand<{ range: vscode.Range; placeholder: string }>(
					'vscode.prepareRename',
					document.uri,
					symbolPosition
				);
				assert.ok(prepareResult?.placeholder === 'SuiteMakeSharedColor');
				return prepareResult.placeholder.length;
			});
			const renameRun = await measureLatencySamples(iterations, async () => {
				const renameEdit = await vscode.commands.executeCommand<vscode.WorkspaceEdit>(
					'vscode.executeDocumentRenameProvider',
					document.uri,
					symbolPosition,
					'RenamedSuiteColor'
				);
				assert.ok(Boolean(renameEdit), 'Expected rename workspace edit for M5 perf scenario.');
				assert.ok(countWorkspaceEdits(renameEdit!) >= 3, 'Expected cross-file rename edits for M5 perf scenario.');
				return countWorkspaceEdits(renameEdit!);
			});
			const metrics = await waitForNextMetricsRevision(
				drained.revision ?? 0,
				'perf m5 idle references rename metrics flush'
			);

			const referencesStats = computeLatencyStats(referencesRun.samples);
			const prepareRenameStats = computeLatencyStats(prepareRenameRun.samples);
			const renameStats = computeLatencyStats(renameRun.samples);

			const report = {
				scenario: 'm5-idle-references-rename',
				fixture: PERF_FIXTURES.pfx5ReverseIncludeImpact,
				loadMode: 'Idle',
				iterations,
				wallClock: {
					references: referencesStats,
					prepareRename: prepareRenameStats,
					rename: renameStats
				},
				metrics
			};
			writePerfReport('m5-idle-references-rename', report);

			assert.ok(referencesStats.p95Ms <= 500, `Expected references p95 <= 500ms. Actual=${referencesStats.p95Ms.toFixed(1)}ms`);
			assert.ok(prepareRenameStats.p95Ms <= 250, `Expected prepareRename p95 <= 250ms. Actual=${prepareRenameStats.p95Ms.toFixed(1)}ms`);
			assert.ok(renameStats.p95Ms <= 800, `Expected rename p95 <= 800ms. Actual=${renameStats.p95Ms.toFixed(1)}ms`);
		});
	});

	it('keeps completion stable while reverse-include file-watch churn runs', async function () {
		this.timeout(240000);

		const iterations = readPerfIntEnv('NSF_PERF_M5_WS_ITERATIONS', 8, 4, 40);
		const churnCount = readPerfIntEnv('NSF_PERF_WS_CHURN_COUNT', 8, 2, 40);
		await waitForIndexingIdle('perf m5 load-ws indexing idle');

		const completionDocument = await openFixture(PERF_FIXTURES.pfx1SmallCurrentDoc.primaryDocument);
		const completionPosition = positionOf(
			completionDocument,
			'return Comp',
			1,
			'return Comp'.length
		);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					completionDocument.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'perf m5 completion warmup'
		);

		const idleRun = await measureLatencySamples(iterations, async () => {
			const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				completionDocument.uri,
				completionPosition
			);
			assert.ok(getCompletionItems(result).length > 0, 'Expected idle completion items for M5 baseline.');
			return getCompletionItems(result).length;
		});
		const idleStats = computeLatencyStats(idleRun.samples);

		const providerPath = path.join(getWorkspaceRoot(), 'test_files', 'watch_provider.nsf');
		const consumerDocument = await openFixture(PERF_FIXTURES.pfx5ReverseIncludeImpact.primaryDocument);
		await waitFor(
			() => vscode.languages.getDiagnostics(consumerDocument.uri),
			(value) => Array.isArray(value),
			'perf m5 file-watch consumer diagnostics'
		);

		let originalProviderText = '';
		try {
			const drained = await drainMetricsWindow('perf m5 load-ws completion');
			const churnPromise = churnWorkspaceWatchLoad(providerPath, consumerDocument, churnCount).then((result) => {
				originalProviderText = result.original;
				return result;
			});

			const loadRun = await measureLatencySamples(iterations, async () => {
				const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					completionDocument.uri,
					completionPosition
				);
				assert.ok(getCompletionItems(result).length > 0, 'Expected completion items during reverse-include churn.');
				return getCompletionItems(result).length;
			});

			await churnPromise;
			const metrics = await waitForNextMetricsRevision(
				drained.revision ?? 0,
				'perf m5 load-ws completion metrics flush'
			);
			const loadStats = computeLatencyStats(loadRun.samples);
			const p95DegradationRatio = computeP95DegradationRatio(idleStats, loadStats);

			const report = {
				scenario: 'm5-load-ws-completion-isolation',
				fixtures: [PERF_FIXTURES.pfx1SmallCurrentDoc, PERF_FIXTURES.pfx5ReverseIncludeImpact],
				loadMode: 'Load-WS',
				iterations,
				churnCount,
				wallClock: {
					idleCompletion: idleStats,
					loadCompletion: loadStats
				},
				p95DegradationRatio,
				metrics
			};
			writePerfReport('m5-load-ws-completion-isolation', report);

			assert.ok(
				p95DegradationRatio <= 0.2,
				`Expected completion p95 degradation <= 20% under Load-WS. Idle=${idleStats.p95Ms.toFixed(1)}ms Load=${loadStats.p95Ms.toFixed(1)}ms Ratio=${(p95DegradationRatio * 100).toFixed(1)}%`
			);
			assert.ok((metrics.payload?.methods?.[completionMethodKey]?.count ?? 0) >= iterations);
			assert.ok((metrics.payload?.diagnostics?.count ?? 0) > 0);
		} finally {
			if (originalProviderText.length > 0) {
				await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(originalProviderText, 'utf8'));
			}
		}
	});
});
