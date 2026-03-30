import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	type SymbolLike,
	flattenSymbolNames,
	getWorkspaceRoot,
	hoverToText,
	openFixture,
	positionOf,
	repoDescribe,
	waitFor,
	waitForHoverText
} from '../test_helpers';

export function registerDeferredDocSemanticTokenTests(): void {
	repoDescribe('NSF client integration: Deferred Doc Runtime / Semantic Tokens', () => {
		it('provides semantic tokens for documents and ranges', async () => {
			const document = await openFixture('module_semantic_tokens.nsf');
			await vscode.commands.executeCommand('workbench.action.closeActiveEditor');

			const legend = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokensLegend>(
						'vscode.provideDocumentSemanticTokensLegend',
						document.uri
					),
				(value) => Array.isArray(value?.tokenTypes) && value.tokenTypes.length > 0,
				'semantic token legend'
			);
			assert.ok(Array.isArray(legend.tokenTypes) && legend.tokenTypes.length > 0);

			const fullTokens = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokens>(
						'vscode.provideDocumentSemanticTokens',
						document.uri
					),
				(value) => Boolean(value) && value.data.length > 0,
				'full semantic tokens'
			);
			assert.ok(fullTokens.data.length > 0);
			assert.strictEqual(fullTokens.data.length % 5, 0);

			const tokenTypeIndices: number[] = [];
			for (let i = 0; i + 4 < fullTokens.data.length; i += 5) {
				tokenTypeIndices.push(fullTokens.data[i + 3]);
			}
			const commentIndex = legend.tokenTypes.indexOf('comment');
			const stringIndex = legend.tokenTypes.indexOf('string');
			const keywordIndex = legend.tokenTypes.indexOf('keyword');
			assert.ok(keywordIndex >= 0 && tokenTypeIndices.includes(keywordIndex));
			if (commentIndex >= 0) {
				assert.ok(!tokenTypeIndices.includes(commentIndex));
			}
			if (stringIndex >= 0) {
				assert.ok(!tokenTypeIndices.includes(stringIndex));
			}

			const range = new vscode.Range(new vscode.Position(0, 0), new vscode.Position(6, 0));
			const rangeTokens = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokens>(
						'vscode.provideDocumentRangeSemanticTokens',
						document.uri,
						range
					),
				(value) => Boolean(value) && value.data.length > 0,
				'range semantic tokens'
			);
			assert.strictEqual(rangeTokens.data.length % 5, 0);
			assert.ok(rangeTokens.data.length <= fullTokens.data.length);
		});

		it('highlights HLSL attributes as keywords in semantic tokens', async () => {
			const document = await openFixture('module_diagnostics_hlsl_attributes.nsf');
			await vscode.commands.executeCommand('workbench.action.closeActiveEditor');

			const legend = await vscode.commands.executeCommand<vscode.SemanticTokensLegend>(
				'vscode.provideDocumentSemanticTokensLegend',
				document.uri
			);
			const keywordIndex = legend.tokenTypes.indexOf('keyword');
			assert.ok(keywordIndex >= 0);

			const fullTokens = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokens>(
						'vscode.provideDocumentSemanticTokens',
						document.uri
					),
				(value) => Boolean(value) && value.data.length > 0,
				'attribute semantic tokens'
			);

			type DecodedToken = { line: number; start: number; length: number; tokenType: number };
			const decoded: DecodedToken[] = [];
			let line = 0;
			let start = 0;
			const data = fullTokens.data;
			for (let i = 0; i + 4 < data.length; i += 5) {
				const deltaLine = data[i];
				const deltaStart = data[i + 1];
				line += deltaLine;
				start = deltaLine === 0 ? start + deltaStart : deltaStart;
				decoded.push({ line, start, length: data[i + 2], tokenType: data[i + 3] });
			}

			const pos = positionOf(document, 'unroll', 1, 0);
			const found = decoded.find(
				(token) => token.line === pos.line && token.start === pos.character && token.length === 'unroll'.length
			);
			assert.ok(found, 'Expected semantic token for unroll attribute.');
			assert.strictEqual(found!.tokenType, keywordIndex);
		});

		it('highlights discard as a keyword in semantic tokens', async () => {
			const document = await openFixture('module_keyword_discard.nsf');
			await vscode.commands.executeCommand('workbench.action.closeActiveEditor');

			const legend = await vscode.commands.executeCommand<vscode.SemanticTokensLegend>(
				'vscode.provideDocumentSemanticTokensLegend',
				document.uri
			);
			const keywordIndex = legend.tokenTypes.indexOf('keyword');
			assert.ok(keywordIndex >= 0);

			const fullTokens = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokens>(
						'vscode.provideDocumentSemanticTokens',
						document.uri
					),
				(value) => Boolean(value) && value.data.length > 0,
				'discard semantic tokens'
			);

			type DecodedToken = { line: number; start: number; length: number; tokenType: number };
			const decoded: DecodedToken[] = [];
			let line = 0;
			let start = 0;
			const data = fullTokens.data;
			for (let i = 0; i + 4 < data.length; i += 5) {
				const deltaLine = data[i];
				const deltaStart = data[i + 1];
				line += deltaLine;
				start = deltaLine === 0 ? start + deltaStart : deltaStart;
				decoded.push({ line, start, length: data[i + 2], tokenType: data[i + 3] });
			}

			const pos = positionOf(document, 'discard', 1, 0);
			const found = decoded.find(
				(token) => token.line === pos.line && token.start === pos.character && token.length === 'discard'.length
			);
			assert.ok(found, 'Expected semantic token for discard keyword.');
			assert.strictEqual(found!.tokenType, keywordIndex);
		});
	});
}

export function registerDeferredDocInlayTests(): void {
	repoDescribe('NSF client integration: Deferred Doc Runtime / Inlay Hints', () => {
		it('provides inlay hints for user and built-in function parameters', async () => {
			const document = await openFixture('module_inlay_hints.nsf');
			const range = new vscode.Range(new vscode.Position(0, 0), document.positionAt(document.getText().length));
			const hints = await waitFor(
				() =>
					vscode.commands.executeCommand<any[]>(
						'vscode.executeInlayHintProvider',
						document.uri,
						range
					),
				(value) => Array.isArray(value) && value.length > 0,
				'inlay hints'
			);
			const labels = hints
				.map((hint) => hint?.label)
				.map((label) => {
					if (typeof label === 'string') {
						return label;
					}
					if (Array.isArray(label)) {
						return label
							.map((part) => (part && typeof part.value === 'string' ? part.value : ''))
							.join('');
					}
					return '';
				})
				.filter((label) => label.length > 0);
			assert.ok(labels.includes('baseColor:'), 'Expected parameter hint baseColor:.');
			assert.ok(labels.includes('amount:'), 'Expected parameter hint amount:.');
			assert.ok(labels.includes('bias:'), 'Expected parameter hint bias:.');
			assert.ok(labels.includes('x:'), 'Expected built-in parameter hint x:.');
			assert.ok(labels.includes('y:'), 'Expected built-in parameter hint y:.');
			assert.ok(labels.includes('s:'), 'Expected built-in parameter hint s:.');
		});

		it('keeps hover responsive while inlay requests are queued', async () => {
			const workspaceRoot = getWorkspaceRoot();
			const heavyFilePath = path.join(workspaceRoot, 'test_files', 'module_inlay_heavy_runtime.nsf');
			const repeatedCalls = Array.from({ length: 900 }, (_, index) => {
				const a = ((index % 9) + 1) / 10;
				const b = (((index + 3) % 9) + 1) / 10;
				return `  sum += BlendColor(src, ${a.toFixed(1)}, ${b.toFixed(1)}).x;`;
			}).join('\n');
			const heavyContent = [
				'float4 BlendColor(float4 baseColor, float amount, float bias) {',
				'  return baseColor * amount + bias;',
				'}',
				'',
				'float4 MainPS(float2 uv : TEXCOORD0) : SV_Target {',
				'  float4 src = float4(uv, 0.0, 1.0);',
				'  float sum = 0.0;',
				repeatedCalls,
				'  return src + sum;',
				'}'
			].join('\n');
			try {
				fs.writeFileSync(heavyFilePath, heavyContent, 'utf8');
				const document = await vscode.workspace.openTextDocument(heavyFilePath);
				await vscode.window.showTextDocument(document, { preview: false });
				const fullRange = new vscode.Range(new vscode.Position(0, 0), document.positionAt(document.getText().length));

				const inlayBurst = Array.from({ length: 18 }, () =>
					vscode.commands.executeCommand<any[]>('vscode.executeInlayHintProvider', document.uri, fullRange)
				);
				const hoverPosition = positionOf(document, 'BlendColor', 2, 2);
				const hoverLatencyMs: number[] = [];
				for (let i = 0; i < 5; i++) {
					const hoverStart = Date.now();
					const hovers = await waitFor(
						() =>
							vscode.commands.executeCommand<vscode.Hover[]>(
								'vscode.executeHoverProvider',
								document.uri,
								hoverPosition
							),
						(value) => Array.isArray(value) && value.length > 0,
						'hover under inlay load'
					);
					hoverLatencyMs.push(Date.now() - hoverStart);
					assert.ok(hovers.length > 0);
				}
				const sortedLatency = [...hoverLatencyMs].sort((a, b) => a - b);
				const p80Index = Math.min(
					sortedLatency.length - 1,
					Math.max(0, Math.ceil(sortedLatency.length * 0.8) - 1)
				);
				const p80Latency = sortedLatency[p80Index];
				const maxLatency = sortedLatency[sortedLatency.length - 1];
				console.log(
					`[hover-under-inlay] samples=${hoverLatencyMs.join(',')} p80=${p80Latency} max=${maxLatency}`
				);
				assert.ok(p80Latency < 6000, `Expected p80 hover latency < 6000ms under inlay load, got ${p80Latency}ms.`);
				assert.ok(maxLatency < 9000, `Expected max hover latency < 9000ms under inlay load, got ${maxLatency}ms.`);
				const settled = await Promise.all(inlayBurst.map((request) => request.then(() => true, () => false)));
				const fulfilledCount = settled.filter((item) => item).length;
				assert.ok(fulfilledCount > 0, 'Expected at least one inlay request to complete under load.');
			} finally {
				if (fs.existsSync(heavyFilePath)) {
					fs.unlinkSync(heavyFilePath);
				}
			}
		});

		it('handles same-uri inlay bursts with cancellation-safe responses', async () => {
			const document = await openFixture('module_inlay_hints.nsf');
			const burst = await vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
				'nsf._spamInlayRequests',
				{
					uri: document.uri.toString(),
					startLine: 0,
					startCharacter: 0,
					endLine: document.lineCount,
					endCharacter: 0,
					count: 30
				}
			);
			assert.ok(Boolean(burst));
			assert.ok((burst?.completed ?? 0) > 0, 'Expected at least one inlay request to complete.');
			assert.strictEqual(burst?.failed ?? 0, 0);
			assert.ok((burst?.cancelled ?? 0) >= 0);
		});
	});
}

export function registerDeferredDocDocumentSymbolTests(): void {
	repoDescribe('NSF client integration: Deferred Doc Runtime / Document Symbols', () => {
		it('provides document symbols without duplicate call-site symbols', async () => {
			const document = await openFixture('module_suite.nsf');
			const symbols = await waitFor(
				() =>
					vscode.commands.executeCommand<SymbolLike[]>(
						'vscode.executeDocumentSymbolProvider',
						document.uri
					),
				(value) => Array.isArray(value) && value.length > 0,
				'document symbols'
			);

			const names = flattenSymbolNames(symbols);
			assert.ok(names.includes('SuiteInput'));
			assert.ok(names.includes('SuiteLocalTint'));
			assert.ok(names.includes('main_ps'));
			assert.ok(names.includes('SuiteTechnique'));
			assert.ok(names.includes('SuitePass'));
			assert.strictEqual(
				names.filter((name) => name === 'SuiteLocalTint').length,
				1,
				'Expected only the declaration of SuiteLocalTint to appear as a document symbol.'
			);
		});

		it('handles same-uri document symbol bursts with cancellation-safe responses', async () => {
			const document = await openFixture('module_perf_large_current_doc.nsf');
			const burst = await vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
				'nsf._spamDocumentSymbolRequests',
				{
					uri: document.uri.toString(),
					count: 24
				}
			);

			assert.ok(Boolean(burst));
			assert.ok((burst?.completed ?? 0) > 0, 'Expected at least one document symbol request to complete.');
			assert.strictEqual(burst?.failed ?? 0, 0);
			assert.ok((burst?.cancelled ?? 0) >= 0);
		});

		it('keeps interactive hover stable while document symbols are queued', async () => {
			const heavyDocument = await openFixture('module_perf_large_current_doc.nsf');
			const hoverDocument = await openFixture('module_hover_docs.nsf');
			const hoverPosition = positionOf(hoverDocument, 'SuiteHoverFunc', 2, 2);

			const burstPromise = vscode.commands.executeCommand<{ completed: number; cancelled: number; failed: number }>(
				'nsf._spamDocumentSymbolRequests',
				{
					uri: heavyDocument.uri.toString(),
					count: 24
				}
			);

			const hoverTexts: string[] = [];
			for (let attempt = 0; attempt < 5; attempt++) {
				const hovers = await waitForHoverText(
					hoverDocument,
					hoverPosition,
					(text) => text.includes('SuiteHoverFunc doc line 1') && text.includes('SuiteHoverFunc('),
					'document symbol background hover'
				);
				hoverTexts.push(hoverToText(hovers));
			}

			const burst = await burstPromise;
			assert.ok(hoverTexts.every((text) => text.includes('SuiteHoverFunc doc line 1')));
			assert.ok(hoverTexts.every((text) => !text.includes('Include context ambiguous')));
			assert.strictEqual(burst?.failed ?? 0, 0);
		});
	});
}
