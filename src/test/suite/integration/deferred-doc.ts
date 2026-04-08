import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	type SymbolLike,
	flattenSymbolNames,
	getWorkspaceRoot,
	getDocumentRuntimeDebug,
	hoverToText,
	openFixture,
	positionOf,
	repoDescribe,
	waitFor,
	waitForClientQuiescent,
	waitForIndexingIdle,
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

		it('reports deferred artifact state in document runtime debug', async function () {
			this.timeout(120000);
			await vscode.commands.executeCommand('nsf.restartServer');
			const document = await openFixture('module_semantic_tokens.nsf');

			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokens>(
						'vscode.provideDocumentSemanticTokens',
						document.uri
					),
				(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
				'semantic tokens for deferred debug surface'
			);

			const [runtime] = await waitFor(
				() => getDocumentRuntimeDebug([document.uri.toString()]),
				(entries) => Boolean(entries[0]?.hasDeferredDocSnapshot),
				'deferred snapshot in runtime debug after full semantic tokens'
			);
			assert.ok(runtime?.hasDeferredDocSnapshot, 'Expected a deferred snapshot to exist.');
			assert.strictEqual(typeof runtime?.deferredHasSemanticSnapshot, 'boolean');
			assert.strictEqual(typeof runtime?.deferredHasSemanticTokensFull, 'boolean');
			assert.strictEqual(typeof runtime?.deferredHasDocumentSymbols, 'boolean');
			assert.strictEqual(typeof runtime?.deferredHasFullDiagnostics, 'boolean');
			assert.strictEqual(typeof runtime?.deferredHasInlayHintsFull, 'boolean');
			assert.strictEqual(typeof runtime?.deferredSemanticTokensRangeCacheCount, 'number');
			assert.strictEqual(typeof runtime?.deferredInlayRangeCacheCount, 'number');
		});

		it('keeps range semantic token requests lazy and avoids eager full deferred artifacts', async function () {
			this.timeout(120000);
			await vscode.commands.executeCommand('nsf.restartServer');
			const absolutePath = path.join(getWorkspaceRoot(), 'test_files', 'module_semantic_tokens.nsf');
			const document = await vscode.workspace.openTextDocument(absolutePath);
			const range = new vscode.Range(new vscode.Position(0, 0), new vscode.Position(6, 0));

			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokens>(
						'vscode.provideDocumentRangeSemanticTokens',
						document.uri,
						range
					),
				(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
				'range semantic tokens without eager deferred full build'
			);

			const [runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
			assert.ok(runtime?.hasDeferredDocSnapshot, 'Expected deferred snapshot after range request.');
			assert.strictEqual(runtime?.deferredHasSemanticSnapshot, true);
			assert.ok(
				(runtime?.deferredSemanticTokensRangeCacheCount ?? 0) > 0,
				'Expected range cache to store at least one entry.'
			);
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

		it('retains non-overlapping semantic token range caches and drops overlapping ones on edit', async function () {
			this.timeout(120000);
			await waitForIndexingIdle('indexing idle before semantic token range cache isolation');
			const workspaceRoot = getWorkspaceRoot();
			const sourcePath = path.join(workspaceRoot, 'test_files', 'module_perf_large_current_doc.nsf');
			const tempPath = path.join(os.tmpdir(), `tmp_semantic_range_cache_${Date.now()}_${Math.random().toString(16).slice(2)}.nsf`);
			fs.writeFileSync(tempPath, fs.readFileSync(sourcePath, 'utf8'), 'utf8');
			try {
				let document = await vscode.workspace.openTextDocument(tempPath);
				await vscode.window.showTextDocument(document, { preview: false });
				const range = new vscode.Range(new vscode.Position(40, 0), new vscode.Position(70, 0));

				await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.SemanticTokens>(
							'vscode.provideDocumentRangeSemanticTokens',
							document.uri,
							range
						),
					(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
					'seed semantic token range cache'
				);

				const seededEntries = await waitFor(
					() => getDocumentRuntimeDebug([document.uri.toString()]),
					(entries) => (entries[0]?.deferredSemanticTokensRangeCacheCount ?? 0) >= 1,
					'seeded semantic token range cache entry'
				);
				let runtime = seededEntries[0];
				const seededSemanticRangeCount = runtime?.deferredSemanticTokensRangeCacheCount ?? 0;
				assert.ok(seededSemanticRangeCount >= 1, 'Expected at least one seeded semantic token range cache entry.');

				const nearInsert = new vscode.WorkspaceEdit();
				nearInsert.insert(document.uri, new vscode.Position(50, 0), " \n");
				assert.ok(await vscode.workspace.applyEdit(nearInsert));
				document = await vscode.workspace.openTextDocument(document.uri);

				[runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
				assert.ok(
					(runtime?.deferredSemanticTokensRangeCacheCount ?? 0) < seededSemanticRangeCount,
					'Overlapping edit should clear at least one semantic token range cache entry.'
				);
			} finally {
				if (fs.existsSync(tempPath)) {
					fs.unlinkSync(tempPath);
				}
			}
		});

		it('materializes full deferred artifacts on demand without discarding range caches', async function () {
			this.timeout(120000);
			await waitForIndexingIdle('indexing idle before deferred full artifact isolation');
			const document = await openFixture('module_semantic_tokens_fresh.nsf');
			const range = new vscode.Range(new vscode.Position(0, 0), new vscode.Position(6, 0));

			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokens>(
						'vscode.provideDocumentRangeSemanticTokens',
						document.uri,
						range
					),
				(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
				'seed semantic token range cache before full artifact build'
			);

			const seededEntries = await waitFor(
				() => getDocumentRuntimeDebug([document.uri.toString()]),
				(entries) => (entries[0]?.deferredSemanticTokensRangeCacheCount ?? 0) >= 1,
				'semantic token range cache visible in runtime debug'
			);
			let runtime = seededEntries[0];
			const seededSemanticRangeCount = runtime?.deferredSemanticTokensRangeCacheCount ?? 0;
			assert.ok(seededSemanticRangeCount >= 1, 'Expected range cache to start with at least one entry.');

			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokens>(
						'vscode.provideDocumentSemanticTokens',
						document.uri
					),
				(value) => Boolean(value) && (value?.data.length ?? 0) > 0,
				'full semantic tokens after range cache seed'
			);
			await waitForClientQuiescent(
				'client quiescent before document symbols after range cache seed'
			);
			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.DocumentSymbol[]>(
						'nsf._sendServerRequest',
						{
							method: 'textDocument/documentSymbol',
							params: {
								textDocument: { uri: document.uri.toString() }
							}
						}
					),
				(value) => Array.isArray(value) && value.length > 0,
				'document symbols after range cache seed'
			);
			await waitFor(
				() => getDocumentRuntimeDebug([document.uri.toString()]),
				(entries) => entries[0]?.deferredHasDocumentSymbols === true,
				'document symbols reflected in runtime debug after range cache seed'
			);

			[runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
			assert.strictEqual(runtime?.deferredHasSemanticTokensFull, true);
			assert.strictEqual(runtime?.deferredHasDocumentSymbols, true);
			assert.strictEqual(runtime?.deferredSemanticTokensRangeCacheCount, seededSemanticRangeCount);
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

		it('retains non-overlapping inlay range caches and clears them on overlapping edits', async function () {
			this.timeout(120000);
			await vscode.commands.executeCommand('nsf.restartServer');
			let document = await openFixture('main.nsf');
			const range = new vscode.Range(new vscode.Position(168, 0), new vscode.Position(214, 0));

			await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.InlayHint[]>(
						'vscode.executeInlayHintProvider',
						document.uri,
						range
					),
				(value) => Array.isArray(value) && value.length > 0,
				'seed inlay range cache'
			);

			let [runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
			const seededInlayRangeCount = runtime?.deferredInlayRangeCacheCount ?? 0;
			assert.ok(seededInlayRangeCount >= 1, 'Expected at least one seeded inlay range cache entry.');

			const nearInsert = new vscode.WorkspaceEdit();
			nearInsert.insert(document.uri, new vscode.Position(171, 0), "// near edit\n");
			assert.ok(await vscode.workspace.applyEdit(nearInsert));
			document = await vscode.workspace.openTextDocument(document.uri);

			[runtime] = await getDocumentRuntimeDebug([document.uri.toString()]);
			assert.ok(
				(runtime?.deferredInlayRangeCacheCount ?? 0) < seededInlayRangeCount,
				'Overlapping edit should clear at least one inlay range cache entry.'
			);
		});

		it('provides nested builtin inlay hints for member-access arguments', async () => {
			const document = await openFixture('module_inlay_member_nested_builtin.nsf');
			const range = new vscode.Range(new vscode.Position(0, 0), document.positionAt(document.getText().length));
			const hints = await waitFor(
				() =>
					vscode.commands.executeCommand<any[]>(
						'vscode.executeInlayHintProvider',
						document.uri,
						range
					),
				(value) => Array.isArray(value) && value.length > 0,
				'nested builtin inlay hints'
			);
			const labelsByLine = hints
				.map((hint) => ({ line: hint?.position?.line, label: hint?.label }))
				.map((item) => {
					const label = typeof item.label === 'string' ? item.label : '';
					return `${item.line}:${label}`;
				})
				.filter((value) => value.endsWith(':'));
			assert.ok(labelsByLine.includes('2:x:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('2:y:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('2:s:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('2:x:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('2:min:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('2:max:'), labelsByLine.join('\n'));
		});

		it('provides inlay hints across the real-workspace lower-block pattern', async () => {
			const document = await openFixture('module_inlay_real_block_like.nsf');
			const range = new vscode.Range(new vscode.Position(0, 0), document.positionAt(document.getText().length));
			const hints = await waitFor(
				() =>
					vscode.commands.executeCommand<any[]>(
						'vscode.executeInlayHintProvider',
						document.uri,
						range
					),
				(value) => Array.isArray(value) && value.length > 0,
				'real-block-like inlay hints'
			);
			const labelsByLine = hints
				.map((hint) => ({ line: hint?.position?.line, label: hint?.label }))
				.map((item) => {
					const label = typeof item.label === 'string' ? item.label : '';
					return `${item.line}:${label}`;
				})
				.filter((value) => value.endsWith(':'));
			assert.ok(labelsByLine.includes('13:UV:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('16:s:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('21:x:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('21:y:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('21:s:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('21:min:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('21:max:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('23:s:'), labelsByLine.join('\n'));
			assert.ok(labelsByLine.includes('23:uv:'), labelsByLine.join('\n'));
		});

		it('provides binary inlay hints for pass stencil state values', async () => {
			const document = await openFixture('module_pass_stencil_states.nsf');
			const range = new vscode.Range(new vscode.Position(0, 0), document.positionAt(document.getText().length));
			const hints = await waitFor(
				() =>
					vscode.commands.executeCommand<any[]>(
						'vscode.executeInlayHintProvider',
						document.uri,
						range
					),
				(value) => {
					if (!Array.isArray(value) || value.length === 0) {
						return false;
					}
					const labels = value
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
						});
					return (
						labels.includes('0b11110000') &&
						labels.includes('0b0000000000001111') &&
						labels.includes('0b0000000011110000')
					);
				},
				'stencil binary inlay hints'
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
			assert.ok(labels.includes('0b11110000'), labels.join('\n'));
			assert.ok(labels.includes('0b0000000000001111'), labels.join('\n'));
			assert.ok(labels.includes('0b0000000011110000'), labels.join('\n'));
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
