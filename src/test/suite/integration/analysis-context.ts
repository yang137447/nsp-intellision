import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	type ProviderLocation,
	type SymbolLike,
	getCompletionItems,
	getDocumentRuntimeDebug,
	getWorkspaceRoot,
	hoverToText,
	openFixture,
	positionOf,
	repoDescribe,
	toFsPath,
	touchDocument,
	waitForActiveUnitAndVisibilityReadyForTests,
	waitForClientQuiescent,
	waitFor,
	waitForClientReady,
	waitForHoverText,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from '../test_helpers';

export function registerDeferredDocSharedKeyAnalysisContextTests(): void {
	repoDescribe('NSF client integration: Deferred Doc Runtime / Analysis Context / Shared Key', () => {
		it('shares one analysis key between interactive and deferred snapshots', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				await waitForIndexingIdle('indexing idle for shared analysis key');
				await openFixture('include_context/units/multi_context_symbol_a.nsf');
				const document = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
				const position = positionOf(document, 'UnitSpecificTone', 1, 2);

				await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							position
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) => path.basename(toFsPath(location)) === 'multi_context_symbol_a.nsf'),
					'interactive definition for shared analysis key'
				);
				await waitFor(
					() =>
						vscode.commands.executeCommand<SymbolLike[]>(
							'vscode.executeDocumentSymbolProvider',
							document.uri
						),
					(value) => Array.isArray(value) && value.length > 0,
					'deferred document symbols for shared analysis key'
				);

				const runtime = await waitFor(
					() => getDocumentRuntimeDebug([document.uri.toString()]),
					(entries) => {
						const entry = entries[0];
						return Boolean(
							entry?.exists &&
							entry.hasCurrentDocSemanticSnapshot &&
							entry.hasDeferredDocSnapshot &&
							entry.analysisFullFingerprint &&
							entry.currentDocSemanticAnalysisFullFingerprint &&
							entry.deferredAnalysisFullFingerprint
						);
					},
					'shared analysis key runtime debug'
				);

				const entry = runtime[0];
				assert.strictEqual(
					entry.analysisFullFingerprint,
					entry.currentDocSemanticAnalysisFullFingerprint
				);
				assert.strictEqual(entry.analysisFullFingerprint, entry.deferredAnalysisFullFingerprint);
				assert.strictEqual(
					entry.analysisStableFingerprint,
					entry.currentDocSemanticAnalysisStableFingerprint
				);
				assert.strictEqual(entry.analysisStableFingerprint, entry.deferredAnalysisStableFingerprint);
				assert.strictEqual(entry.workspaceSummaryVersion, entry.activeUnitWorkspaceSummaryVersion);
				assert.ok((entry.activeUnitIncludeClosureFingerprint?.length ?? 0) > 0);
			});
		});
	});
}

export function registerDeferredDocActiveUnitAnalysisContextTests(): void {
	repoDescribe('NSF client integration: Deferred Doc Runtime / Analysis Context / Active Unit', () => {
		it('updates shared interactive and deferred analysis context together when the active unit changes', async () => {
			try {
				await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
					await waitForIndexingIdle('indexing idle for active unit analysis context');
					await openFixture('include_context/units/multi_context_symbol_a.nsf');
					const document = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
					const position = positionOf(document, 'UnitSpecificTone', 1, 2);

					await waitFor(
						() =>
							vscode.commands.executeCommand<ProviderLocation[]>(
								'vscode.executeDefinitionProvider',
								document.uri,
								position
							),
						(value) =>
							Array.isArray(value) &&
							value.some((location) => path.basename(toFsPath(location)) === 'multi_context_symbol_a.nsf'),
						'definition for active unit A analysis context'
					);
					await waitFor(
						() =>
							vscode.commands.executeCommand<SymbolLike[]>(
								'vscode.executeDocumentSymbolProvider',
								document.uri
							),
						(value) => Array.isArray(value) && value.length > 0,
						'document symbols for active unit A analysis context'
					);
					const beforeEntries = await getDocumentRuntimeDebug([document.uri.toString()]);
					const before = beforeEntries[0];
					assert.ok(before?.analysisFullFingerprint);

					await openFixture('include_context/units/multi_context_symbol_b.nsf');
					await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
					await waitFor(
						() =>
							vscode.commands.executeCommand<ProviderLocation[]>(
								'vscode.executeDefinitionProvider',
								document.uri,
								position
							),
						(value) =>
							Array.isArray(value) &&
							value.some((location) => path.basename(toFsPath(location)) === 'multi_context_symbol_b.nsf'),
						'definition for active unit B analysis context'
					);
					await waitFor(
						() =>
							vscode.commands.executeCommand<SymbolLike[]>(
								'vscode.executeDocumentSymbolProvider',
								document.uri
							),
						(value) => Array.isArray(value) && value.length > 0,
						'document symbols for active unit B analysis context'
					);

					const afterEntries = await waitFor(
						() => getDocumentRuntimeDebug([document.uri.toString()]),
						(entries) => {
						const entry = entries[0];
						return Boolean(
							entry?.analysisFullFingerprint &&
							entry.analysisFullFingerprint !== before.analysisFullFingerprint &&
							entry.currentDocSemanticAnalysisFullFingerprint ===
								entry.analysisFullFingerprint &&
							entry.deferredAnalysisFullFingerprint === entry.analysisFullFingerprint
						);
					},
					'active unit switched shared analysis context'
				);

				const after = afterEntries[0];
				assert.notStrictEqual(after.analysisFullFingerprint, before.analysisFullFingerprint);
				assert.strictEqual(
					after.analysisFullFingerprint,
					after.currentDocSemanticAnalysisFullFingerprint
				);
				assert.strictEqual(after.analysisFullFingerprint, after.deferredAnalysisFullFingerprint);
				});
			} finally {
				await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
				await openFixture('module_suite.nsf');
			}
		});
	});
}

export function registerDeferredDocDefinesAnalysisContextTests(): void {
	repoDescribe('NSF client integration: Deferred Doc Runtime / Analysis Context / Defines', () => {
		it('updates shared interactive and deferred analysis context together when defines change', async () => {
			const configuration = vscode.workspace.getConfiguration('nsf');
			const previousDefines = configuration.inspect<string[]>('defines')?.workspaceValue;
			const document = await openFixture('module_analysis_context_defines.nsf');
			const hoverPosition = positionOf(document, 'value.shared', 1, 2);

			try {
				await waitForClientReady('client ready for defines analysis context');
				await configuration.update('defines', [], vscode.ConfigurationTarget.Workspace);
				await waitForHoverText(
					document,
					hoverPosition,
					(text) => text.includes('Type: AnalysisDefineInactive'),
					'hover for defines analysis context inactive branch'
				);
				await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.SemanticTokens>(
							'vscode.provideDocumentSemanticTokens',
							document.uri
						),
					(value) => Boolean(value) && value.data.length > 0,
					'semantic tokens for defines analysis context inactive branch'
				);
				const beforeEntries = await getDocumentRuntimeDebug([document.uri.toString()]);
				const before = beforeEntries[0];
				assert.ok(before?.analysisFullFingerprint);

				await configuration.update(
					'defines',
					['USE_ANALYSIS_CONTEXT_ACTIVE=1'],
					vscode.ConfigurationTarget.Workspace
				);
				if (process.env.NSF_LOG_ANALYSIS_CONTEXT_DEBUG === '1') {
					const runtimeAfterDefine = await getDocumentRuntimeDebug([document.uri.toString()]);
					let hoverDebugText = '';
					try {
						const hovers = await vscode.commands.executeCommand<vscode.Hover[]>(
							'vscode.executeHoverProvider',
							document.uri,
							hoverPosition
						);
						hoverDebugText = hoverToText(hovers ?? []);
					} catch (error) {
						hoverDebugText = `hover error: ${(error as Error).message ?? String(error)}`;
					}
					console.log(
						`[analysis-defines] ${JSON.stringify({
							runtimeAfterDefine: runtimeAfterDefine[0],
							hoverDebugText
						})}`
					);
				}
				await waitForHoverText(
					document,
					hoverPosition,
					(text) => text.includes('Type: AnalysisDefineActive'),
					'hover for defines analysis context active branch'
				);
				await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.SemanticTokens>(
							'vscode.provideDocumentSemanticTokens',
							document.uri
						),
					(value) => Boolean(value) && value.data.length > 0,
					'semantic tokens for defines analysis context active branch'
				);

				const afterEntries = await waitFor(
					() => getDocumentRuntimeDebug([document.uri.toString()]),
					(entries) => {
						const entry = entries[0];
						return Boolean(
							entry?.analysisFullFingerprint &&
							entry.analysisFullFingerprint !== before.analysisFullFingerprint &&
							entry.currentDocSemanticAnalysisFullFingerprint ===
								entry.analysisFullFingerprint &&
							entry.deferredAnalysisFullFingerprint === entry.analysisFullFingerprint
						);
					},
					'defines analysis context refresh'
				);

				const after = afterEntries[0];
				assert.notStrictEqual(after.analysisFullFingerprint, before.analysisFullFingerprint);
				assert.strictEqual(
					after.analysisFullFingerprint,
					after.currentDocSemanticAnalysisFullFingerprint
				);
				assert.strictEqual(after.analysisFullFingerprint, after.deferredAnalysisFullFingerprint);
			} finally {
				await configuration.update('defines', previousDefines, vscode.ConfigurationTarget.Workspace);
				await openFixture('module_suite.nsf');
			}
		});
	});
}

export function registerDeferredDocIncludeClosureAnalysisContextTests(): void {
	repoDescribe('NSF client integration: Deferred Doc Runtime / Analysis Context / Include Closure', () => {
		it('does not append include-context summary for same-document function hover in shared include files', async () => {
			try {
				await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
					await waitForIndexingIdle('indexing idle for shared include hover');
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');

					const symbolDocument = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
					const ambiguousDefinitionPosition = positionOf(symbolDocument, 'UnitSpecificTone', 1, 2);
					await waitFor(
						() =>
							vscode.commands.executeCommand<ProviderLocation[]>(
								'vscode.executeDefinitionProvider',
								symbolDocument.uri,
								ambiguousDefinitionPosition
							),
						(value) =>
							Array.isArray(value) &&
							value.some((location) => path.basename(toFsPath(location)) === 'multi_context_symbol_a.nsf') &&
							value.some((location) => path.basename(toFsPath(location)) === 'multi_context_symbol_b.nsf'),
						'ambiguous include-context definitions for shared symbol hover'
					);

					const document = await openFixture('include_context/shared/multi_context_parameter_common.hlsl');
					const hoverPosition = positionOf(document, 'MultiContextParameterTone', 1, 2);
					await waitForHoverText(
						document,
						hoverPosition,
						(text) =>
							text.includes('half4 MultiContextParameterTone') &&
							!text.includes('Candidate definitions') &&
							!text.includes('Candidate units') &&
							!text.includes('Include context ambiguous'),
						'same-document shared include function hover without include-context summary'
					);
				});
			} finally {
				await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
				await openFixture('module_suite.nsf');
			}
		});

		it('does not append include-context summary when all candidate units converge to one cross-file definition', async () => {
			try {
				await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
					await waitForIndexingIdle('indexing idle for unique cross-doc include hover');
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');

					const document = await openFixture('include_context/shared/multi_context_cross_doc_unique_common.hlsl');
					const hoverPosition = positionOf(document, 'MultiContextSharedFog', 2, 2);
					const hovers = await waitFor(
						() =>
							vscode.commands.executeCommand<vscode.Hover[]>(
								'vscode.executeHoverProvider',
								document.uri,
								hoverPosition
							),
						(value) => Array.isArray(value) && value.length > 0,
						'unique cross-doc shared include function hover'
					);
					const text = hoverToText(hovers);
					assert.ok(
						text.includes('half3 MultiContextSharedFog('),
						`Expected cross-doc unique hover signature. Actual hover:\n${text}`
					);
					assert.ok(
						text.includes('multi_context_cross_doc_unique_def.hlsl'),
						`Expected cross-doc unique hover to resolve to the shared definition file. Actual hover:\n${text}`
					);
					assert.ok(
						!text.includes('Candidate definitions') &&
							!text.includes('Include context ambiguous') &&
							!text.includes('Include context partially resolved'),
						`Expected no include-context summary for a unique cross-doc definition. Actual hover:\n${text}`
					);
				});
			} finally {
				await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
				await openFixture('module_suite.nsf');
			}
		});

		it('keeps grouped candidate definitions when shared include hover is truly ambiguous', async () => {
			try {
				await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
					await waitForIndexingIdle('indexing idle for ambiguous shared include hover');
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');

					const document = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
					const hoverPosition = positionOf(document, 'UnitSpecificTone', 1, 2);
					const hovers = await waitFor(
						() =>
							vscode.commands.executeCommand<vscode.Hover[]>(
								'vscode.executeHoverProvider',
								document.uri,
								hoverPosition
							),
						(value) => Array.isArray(value) && value.length > 0,
						'ambiguous shared include hover'
					);
					const text = hoverToText(hovers);
					assert.ok(
						text.includes('Include context ambiguous') &&
							text.includes('Candidate definitions') &&
							text.includes('multi_context_symbol_a.nsf') &&
							text.includes('multi_context_symbol_b.nsf'),
						`Expected grouped candidate definitions for an ambiguous include-context hover. Actual hover:\n${text}`
					);
				});
			} finally {
				await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
				await openFixture('module_suite.nsf');
			}
		});

		it('preserves a partial-resolution note when only some candidate units resolve the shared definition', async () => {
			try {
				await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
					await waitForIndexingIdle('indexing idle for partial include-context hover');
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');

					const document = await openFixture('include_context/shared/multi_context_partial_common.hlsl');
					const hoverPosition = positionOf(document, 'MultiContextPartialFog', 1, 2);
					const hovers = await waitFor(
						() =>
							vscode.commands.executeCommand<vscode.Hover[]>(
								'vscode.executeHoverProvider',
								document.uri,
								hoverPosition
							),
						(value) => Array.isArray(value) && value.length > 0,
						'partial include-context hover'
					);
					const text = hoverToText(hovers);
					assert.ok(
						text.includes('half3 MultiContextPartialFog') &&
							text.includes('multi_context_partial_def.hlsl') &&
							text.includes('Include context partially resolved') &&
							!text.includes('Candidate definitions'),
						`Expected a partial-resolution note without grouped candidate definitions. Actual hover:\n${text}`
					);
				});
			} finally {
				await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
				await openFixture('module_suite.nsf');
			}
		});

		it('updates shared interactive and deferred analysis context together when include closure changes', async () => {
			const configuration = vscode.workspace.getConfiguration('nsf');
			const previousDefines = configuration.inspect<string[]>('defines')?.workspaceValue;
			const document = await openFixture('module_analysis_context_include_switch.nsf');
			const completionPosition = new vscode.Position(0, 0);

			try {
				await waitForClientReady('client ready for include closure analysis context');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', document.uri.toString());
				await configuration.update('defines', [], vscode.ConfigurationTarget.Workspace);
				await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
							'vscode.executeCompletionItemProvider',
							document.uri,
							completionPosition
						),
					(value) => getCompletionItems(value).length > 0,
					'completion for include closure analysis context A'
				);
				await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.SemanticTokens>(
							'vscode.provideDocumentSemanticTokens',
							document.uri
						),
					(value) => Boolean(value) && value.data.length > 0,
					'semantic tokens for include closure analysis context A'
				);
				const beforeEntries = await getDocumentRuntimeDebug([document.uri.toString()]);
				const before = beforeEntries[0];
				assert.ok(before?.analysisFullFingerprint);
				assert.ok(before?.activeUnitIncludeClosureFingerprint);

				await configuration.update(
					'defines',
					['USE_ANALYSIS_CONTEXT_ALT_INCLUDE=1'],
					vscode.ConfigurationTarget.Workspace
				);
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', document.uri.toString());
				await touchDocument(document);
				await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
							'vscode.executeCompletionItemProvider',
							document.uri,
							completionPosition
						),
					(value) => getCompletionItems(value).length > 0,
					'completion for include closure analysis context B'
				);
				await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.SemanticTokens>(
							'vscode.provideDocumentSemanticTokens',
							document.uri
						),
					(value) => Boolean(value) && value.data.length > 0,
					'semantic tokens for include closure analysis context B'
				);

				const afterEntries = await waitFor(
					() => getDocumentRuntimeDebug([document.uri.toString()]),
					(entries) => {
						const entry = entries[0];
						return Boolean(
							entry?.analysisFullFingerprint &&
							entry.analysisFullFingerprint !== before.analysisFullFingerprint &&
							entry.activeUnitIncludeClosureFingerprint &&
							entry.activeUnitIncludeClosureFingerprint !== before.activeUnitIncludeClosureFingerprint &&
							entry.currentDocSemanticAnalysisFullFingerprint ===
								entry.analysisFullFingerprint &&
							entry.deferredAnalysisFullFingerprint === entry.analysisFullFingerprint
						);
					},
					'include closure analysis context refresh'
				);

				const after = afterEntries[0];
				assert.notStrictEqual(after.analysisFullFingerprint, before.analysisFullFingerprint);
				assert.notStrictEqual(after.activeUnitIncludeClosureFingerprint, before.activeUnitIncludeClosureFingerprint);
				assert.strictEqual(
					after.analysisFullFingerprint,
					after.currentDocSemanticAnalysisFullFingerprint
				);
				assert.strictEqual(after.analysisFullFingerprint, after.deferredAnalysisFullFingerprint);
			} finally {
				await configuration.update('defines', previousDefines, vscode.ConfigurationTarget.Workspace);
				await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
				await openFixture('module_suite.nsf');
			}
		});
	});
}

export function registerDeferredDocWorkspaceSummaryAnalysisContextTests(): void {
	repoDescribe('NSF client integration: Deferred Doc Runtime / Analysis Context / Workspace Summary', () => {
		it('updates shared interactive and deferred analysis context together when workspace summary version changes', async function () {
			this.timeout(120000);
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				const providerPath = path.join(getWorkspaceRoot(), 'test_files', 'watch_provider.nsf');
				const original = fs.readFileSync(providerPath, 'utf8');
				const originalDefinedSymbol = original.includes('u_value2') ? 'u_value2' : 'u_value';
				const updatedDefinedSymbol = originalDefinedSymbol === 'u_value' ? 'u_value2' : 'u_value';
				const updated = original.replace(originalDefinedSymbol, updatedDefinedSymbol);

				try {
					await waitForIndexingIdle('indexing idle for workspace summary shared context');
					const document = await openFixture('watch_consumer.nsf');
					const completionPosition = new vscode.Position(0, 0);

					await waitFor(
						() =>
							vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
								'vscode.executeCompletionItemProvider',
								document.uri,
								completionPosition
							),
						(value) => getCompletionItems(value).length > 0,
						'interactive completion before workspace summary change'
					);
					await waitFor(
						() =>
							vscode.commands.executeCommand<SymbolLike[]>(
								'vscode.executeDocumentSymbolProvider',
								document.uri
							),
						(value) => Array.isArray(value) && value.length > 0,
						'deferred symbols before workspace summary change'
					);

					const beforeEntries = await getDocumentRuntimeDebug([document.uri.toString()]);
					const before = beforeEntries[0];
					assert.ok(before?.analysisFullFingerprint);
					assert.ok(before?.workspaceSummaryVersion !== undefined);

					await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(updated, 'utf8'));
					await waitForIndexingIdle('indexing idle after workspace summary provider change');
					await touchDocument(document);

					await waitFor(
						() =>
							vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
								'vscode.executeCompletionItemProvider',
								document.uri,
								completionPosition
							),
						(value) => getCompletionItems(value).length > 0,
						'interactive completion after workspace summary change'
					);
					await waitFor(
						() =>
							vscode.commands.executeCommand<SymbolLike[]>(
								'vscode.executeDocumentSymbolProvider',
								document.uri
							),
						(value) => Array.isArray(value) && value.length > 0,
						'deferred symbols after workspace summary change'
					);

					const afterEntries = await waitFor(
						() => getDocumentRuntimeDebug([document.uri.toString()]),
						(entries) => {
						const entry = entries[0];
						return Boolean(
							entry?.analysisFullFingerprint &&
							entry.workspaceSummaryVersion !== undefined &&
							before.workspaceSummaryVersion !== undefined &&
							entry.workspaceSummaryVersion > before.workspaceSummaryVersion &&
							entry.analysisFullFingerprint !== before.analysisFullFingerprint &&
							entry.currentDocSemanticAnalysisFullFingerprint ===
								entry.analysisFullFingerprint &&
							entry.deferredAnalysisFullFingerprint === entry.analysisFullFingerprint
						);
					},
					'workspace summary shared analysis context refresh'
				);

					const after = afterEntries[0];
					assert.ok(
						(after.workspaceSummaryVersion ?? 0) > (before.workspaceSummaryVersion ?? 0),
						'Expected workspace summary version to advance after provider change.'
					);
					assert.notStrictEqual(after.analysisFullFingerprint, before.analysisFullFingerprint);
					assert.strictEqual(
						after.analysisFullFingerprint,
						after.currentDocSemanticAnalysisFullFingerprint
					);
					assert.strictEqual(after.analysisFullFingerprint, after.deferredAnalysisFullFingerprint);
				} finally {
					await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(original, 'utf8'));
					await waitForIndexingIdle('indexing idle after workspace summary provider restore');
				}
			});
		});
	});
}

export function registerDeferredDocUnitMacroProfileAnalysisContextTests(): void {
	repoDescribe('NSF client integration: Deferred Doc Runtime / Analysis Context / Unit Macro Profile', () => {
		it('refreshes shared analysis context when the active unit resolves a different macro profile', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				const resolvedKey = 'shader\\multi_context_variant_profile_resolved.nfx2';
				const unresolvedUnit = await openFixture('include_context/units/multi_context_variant_profile_unresolved.nsf');

				try {
					await waitForIndexingIdle('indexing idle before unit macro profile analysis context');
					await vscode.commands.executeCommand('nsf._setActiveUnitForTests', unresolvedUnit.uri.toString());
					await waitForActiveUnitAndVisibilityReadyForTests(
						unresolvedUnit.uri.toString(),
						path.join('include_context', 'units', 'multi_context_variant_profile_unresolved.nsf'),
						'active unit ready for unresolved unit macro profile document'
					);
					const beforeEntries = await getDocumentRuntimeDebug([unresolvedUnit.uri.toString()]);
					const before = beforeEntries[0];
					assert.ok(before?.analysisFullFingerprint);
					assert.strictEqual(
						(before?.activeUnitProfileDefines as Record<string, number> | undefined)?.TARGET_VARIANT_PROFILE,
						undefined
					);

					const resolvedUnit = await openFixture('include_context/units/multi_context_variant_profile_resolved.nsf');
					await vscode.commands.executeCommand('nsf._setActiveUnitForTests', resolvedUnit.uri.toString());
					await waitForActiveUnitAndVisibilityReadyForTests(
						resolvedUnit.uri.toString(),
						path.join('include_context', 'units', 'multi_context_variant_profile_resolved.nsf'),
						'active unit ready for resolved unit macro profile document'
					);
					const hoverPosition = positionOf(resolvedUnit, 'value =', 1, 1);
					await waitForHoverText(
						resolvedUnit,
						hoverPosition,
						(text) => text.includes('Type: VariantProfileResolvedType'),
						'hover for resolved unit macro profile branch'
					);
					const afterEntries = await waitFor(
						() => getDocumentRuntimeDebug([resolvedUnit.uri.toString()]),
						(entries) => {
							const entry = entries[0];
							return Boolean(
								entry?.analysisFullFingerprint &&
								before?.analysisFullFingerprint &&
								entry.analysisFullFingerprint !== before.analysisFullFingerprint &&
								(entry.activeUnitProfileDefines as Record<string, number> | undefined)?.TARGET_VARIANT_PROFILE === 1
							);
						},
						'unit macro profile analysis context refresh'
					);
					const after = afterEntries[0];
					assert.notStrictEqual(after.analysisFullFingerprint, before.analysisFullFingerprint);
					assert.strictEqual(
						(after.activeUnitProfileDefines as Record<string, number> | undefined)?.TARGET_VARIANT_PROFILE,
						1
					);
					assert.strictEqual(after.activeUnitProfileShaderKey, resolvedKey);
				} finally {
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
					await waitForClientQuiescent('unit macro profile cleanup settled');
					await openFixture('module_suite.nsf');
				}
			});
		});

		it('discovers unit macro profiles from configured shaderCompilerPath', async () => {
			await withTemporaryIntellisionPath([], async () => {
				const configuration = vscode.workspace.getConfiguration('nsf');
				const inspectedCompilerPath = configuration.inspect<string>('shaderCompilerPath');
				const originalCompilerPath = inspectedCompilerPath?.workspaceValue;
				const compilerRoot = path.join(getWorkspaceRoot(), 'test_files', 'shadercompiler_path_fixture');
				await configuration.update('shaderCompilerPath', compilerRoot, vscode.ConfigurationTarget.Workspace);
				await waitForClientQuiescent('shaderCompilerPath update settled');

				const resolvedUnit = await openFixture('include_context/units/multi_context_variant_profile_resolved.nsf');
				try {
					await waitForIndexingIdle('indexing idle before shaderCompilerPath unit macro profile test');
					await vscode.commands.executeCommand('nsf._setActiveUnitForTests', resolvedUnit.uri.toString());
					await waitForActiveUnitAndVisibilityReadyForTests(
						resolvedUnit.uri.toString(),
						path.join('include_context', 'units', 'multi_context_variant_profile_resolved.nsf'),
						'active unit ready for shaderCompilerPath unit macro profile test'
					);
					const entries = await waitFor(
						() => getDocumentRuntimeDebug([resolvedUnit.uri.toString()]),
						(value) => {
							const entry = value[0];
							const defines = entry?.activeUnitProfileDefines as Record<string, number> | undefined;
							return Boolean(
								entry?.analysisFullFingerprint &&
								defines?.TARGET_VARIANT_PROFILE === 1 &&
								entry.activeUnitProfileSourcePath?.includes('shadercompiler_path_fixture') &&
								entry.activeUnitProfileSourcePath?.endsWith('gimlocalvariants.json')
							);
						},
						'shaderCompilerPath unit macro profile runtime debug'
					);
					const entry = entries[0];
					assert.strictEqual(
						(entry.activeUnitProfileDefines as Record<string, number> | undefined)?.TARGET_VARIANT_PROFILE,
						1
					);
					assert.ok(
						entry.activeUnitProfileSourcePath?.includes('shadercompiler_path_fixture'),
						`Expected shaderCompilerPath source, got ${entry.activeUnitProfileSourcePath ?? '<missing>'}.`
					);
					assert.strictEqual(entry.activeUnitProfileSourceKind, 'gimlocalvariants');
				} finally {
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
					await waitForClientQuiescent('shaderCompilerPath unit macro profile cleanup settled');
					await openFixture('module_suite.nsf');
					await configuration.update('shaderCompilerPath', originalCompilerPath, vscode.ConfigurationTarget.Workspace);
					await waitForClientQuiescent('shaderCompilerPath restore settled');
				}
			});
		});

		it('injects only unanimous CSV variant macros into the shared analysis context', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				const csvUnit = await openFixture('include_context/units/multi_context_variant_profile_csv.nsf');

				try {
					await waitForIndexingIdle('indexing idle before CSV unit macro profile analysis context');
					await vscode.commands.executeCommand('nsf._setActiveUnitForTests', csvUnit.uri.toString());
					await waitForActiveUnitAndVisibilityReadyForTests(
						csvUnit.uri.toString(),
						path.join('include_context', 'units', 'multi_context_variant_profile_csv.nsf'),
						'active unit ready for CSV unit macro profile document'
					);
					const hoverPosition = positionOf(csvUnit, 'value =', 1, 1);
					await waitForHoverText(
						csvUnit,
						hoverPosition,
						(text) => text.includes('Type: VariantProfileCsvResolvedType'),
						'hover for CSV unit macro profile resolved branch'
					);
					const entries = await waitFor(
						() => getDocumentRuntimeDebug([csvUnit.uri.toString()]),
						(value) => {
							const entry = value[0];
							return Boolean(
								entry?.analysisFullFingerprint &&
								(entry.activeUnitProfileDefines as Record<string, number> | undefined)?.CSV_STABLE_PROFILE === 3 &&
								entry.activeUnitProfileSourcePath?.endsWith('used_shader_variants.csv') &&
								entry.activeUnitProfileSourceKind === 'used_shader_variants' &&
								entry.activeUnitProfileTotalRowCount === 2 &&
								entry.activeUnitProfileSelectedRowCount === 2 &&
								Array.isArray(entry.activeUnitProfileUnresolvedMacros) &&
								entry.activeUnitProfileUnresolvedMacros.includes('CSV_CONFLICTING_MODE')
							);
						},
						'CSV unit macro profile runtime debug'
					);
					const entry = entries[0];
					assert.strictEqual(
						(entry.activeUnitProfileDefines as Record<string, number> | undefined)?.CSV_STABLE_PROFILE,
						3
					);
					assert.strictEqual(
						(entry.activeUnitProfileDefines as Record<string, number> | undefined)?.CSV_CONFLICTING_MODE,
						undefined
					);
					assert.ok(
						entry.activeUnitProfileSourcePath?.endsWith('used_shader_variants.csv'),
						`Expected used_shader_variants.csv source path, got ${entry.activeUnitProfileSourcePath ?? '<missing>'}.`
					);
					assert.strictEqual(entry.activeUnitProfileSourceKind, 'used_shader_variants');
					assert.strictEqual(entry.activeUnitProfileTotalRowCount, 2);
					assert.strictEqual(entry.activeUnitProfileSelectedRowCount, 2);
					assert.ok(
						entry.activeUnitProfileUnresolvedMacros?.includes('CSV_CONFLICTING_MODE'),
						`Expected CSV_CONFLICTING_MODE unresolved macro, got ${JSON.stringify(entry.activeUnitProfileUnresolvedMacros ?? [])}.`
					);
				} finally {
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
					await waitForClientQuiescent('CSV unit macro profile cleanup settled');
					await openFixture('module_suite.nsf');
				}
			});
		});

		it('resolves CSV variant macros by explicit workspace selection hints without guessing defaults', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				const configuration = vscode.workspace.getConfiguration('nsf');
				const inspectedMacros = configuration.inspect<Record<string, unknown>>('preprocessorMacros');
				const originalMacros = inspectedMacros?.workspaceValue;
				const updatedMacros: Record<string, unknown> = {
					...(originalMacros ?? {}),
					CSV_CONFLICTING_MODE: 1
				};
				await configuration.update('preprocessorMacros', updatedMacros, vscode.ConfigurationTarget.Workspace);
				await waitForClientQuiescent('workspace preprocessor macros update settled for CSV variant selection');

				const csvUnit = await openFixture('include_context/units/multi_context_variant_profile_csv.nsf');
				try {
					await waitForIndexingIdle('indexing idle before CSV variant selection hint test');
					await vscode.commands.executeCommand('nsf._setActiveUnitForTests', csvUnit.uri.toString());
					await waitForActiveUnitAndVisibilityReadyForTests(
						csvUnit.uri.toString(),
						path.join('include_context', 'units', 'multi_context_variant_profile_csv.nsf'),
						'active unit ready for CSV variant selection hint test'
					);
					const entries = await waitFor(
						() => getDocumentRuntimeDebug([csvUnit.uri.toString()]),
						(value) => {
							const entry = value[0];
							const defines = entry?.activeUnitProfileDefines as Record<string, number> | undefined;
							return Boolean(
								entry?.analysisFullFingerprint &&
								entry.activeUnitProfileSourceKind === 'used_shader_variants' &&
								defines?.CSV_STABLE_PROFILE === 3 &&
								defines?.CSV_CONFLICTING_MODE === 1 &&
								entry.activeUnitProfileTotalRowCount === 2 &&
								entry.activeUnitProfileSelectedRowCount === 1 &&
								entry.activeUnitProfileSelectedRowSignature?.includes('CSV_CONFLICTING_MODE=1') &&
								Array.isArray(entry.activeUnitProfileUnresolvedMacros) &&
								!entry.activeUnitProfileUnresolvedMacros.includes('CSV_CONFLICTING_MODE')
							);
						},
						'CSV variant selection hint runtime debug'
					);
					const entry = entries[0];
					assert.strictEqual(
						(entry.activeUnitProfileDefines as Record<string, number> | undefined)?.CSV_CONFLICTING_MODE,
						1
					);
					assert.ok(
						!(entry.activeUnitProfileUnresolvedMacros ?? []).includes('CSV_CONFLICTING_MODE'),
						`Expected CSV_CONFLICTING_MODE to be resolved by explicit selection hints, got ${JSON.stringify(entry.activeUnitProfileUnresolvedMacros ?? [])}.`
					);
					assert.strictEqual(entry.activeUnitProfileTotalRowCount, 2);
					assert.strictEqual(entry.activeUnitProfileSelectedRowCount, 1);
					assert.ok(
						(entry.activeUnitProfileSelectedRowSignature ?? '').includes('CSV_CONFLICTING_MODE=1'),
						`Expected selected row signature to include CSV_CONFLICTING_MODE=1, got ${entry.activeUnitProfileSelectedRowSignature ?? '<missing>'}.`
					);
				} finally {
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
					await waitForClientQuiescent('CSV variant selection hint cleanup settled');
					await openFixture('module_suite.nsf');
					await configuration.update('preprocessorMacros', originalMacros, vscode.ConfigurationTarget.Workspace);
					await waitForClientQuiescent('workspace preprocessor macros restore settled for CSV variant selection');
				}
			});
		});

		it('resolves symbolic workspace macro hints for CSV variant selection', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				const configuration = vscode.workspace.getConfiguration('nsf');
				const inspectedMacros = configuration.inspect<Record<string, unknown>>('preprocessorMacros');
				const originalMacros = inspectedMacros?.workspaceValue;
				const updatedMacros: Record<string, unknown> = {
					...(originalMacros ?? {}),
					CSV_CONFLICTING_MODE: 'CSV_MODE_SELECTED',
					CSV_MODE_SELECTED: 1
				};
				await configuration.update('preprocessorMacros', updatedMacros, vscode.ConfigurationTarget.Workspace);
				await waitForClientQuiescent('symbolic workspace preprocessor macros update settled');

				const csvUnit = await openFixture('include_context/units/multi_context_variant_profile_csv.nsf');
				try {
					await waitForIndexingIdle('indexing idle before symbolic CSV variant selection test');
					await vscode.commands.executeCommand('nsf._setActiveUnitForTests', csvUnit.uri.toString());
					await waitForActiveUnitAndVisibilityReadyForTests(
						csvUnit.uri.toString(),
						path.join('include_context', 'units', 'multi_context_variant_profile_csv.nsf'),
						'active unit ready for symbolic CSV variant selection test'
					);
					const entries = await waitFor(
						() => getDocumentRuntimeDebug([csvUnit.uri.toString()]),
						(value) => {
							const entry = value[0];
							const defines = entry?.activeUnitProfileDefines as Record<string, number> | undefined;
							return Boolean(
								entry?.analysisFullFingerprint &&
								defines?.CSV_CONFLICTING_MODE === 1 &&
								entry.activeUnitProfileTotalRowCount === 2 &&
								entry.activeUnitProfileSelectedRowCount === 1 &&
								entry.activeUnitProfileSelectedRowSignature?.includes('CSV_CONFLICTING_MODE=1') &&
								Array.isArray(entry.activeUnitProfileUnresolvedMacros) &&
								!entry.activeUnitProfileUnresolvedMacros.includes('CSV_CONFLICTING_MODE')
							);
						},
						'symbolic CSV variant selection runtime debug'
					);
					const entry = entries[0];
					assert.strictEqual(
						(entry.activeUnitProfileDefines as Record<string, number> | undefined)?.CSV_CONFLICTING_MODE,
						1
					);
					assert.ok(
						!(entry.activeUnitProfileUnresolvedMacros ?? []).includes('CSV_CONFLICTING_MODE'),
						`Expected symbolic selection hint to resolve CSV_CONFLICTING_MODE, got ${JSON.stringify(entry.activeUnitProfileUnresolvedMacros ?? [])}.`
					);
					assert.strictEqual(entry.activeUnitProfileTotalRowCount, 2);
					assert.strictEqual(entry.activeUnitProfileSelectedRowCount, 1);
					assert.ok(
						(entry.activeUnitProfileSelectedRowSignature ?? '').includes('CSV_CONFLICTING_MODE=1'),
						`Expected selected row signature to include CSV_CONFLICTING_MODE=1, got ${entry.activeUnitProfileSelectedRowSignature ?? '<missing>'}.`
					);
				} finally {
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
					await waitForClientQuiescent('symbolic CSV variant selection cleanup settled');
					await openFixture('module_suite.nsf');
					await configuration.update('preprocessorMacros', originalMacros, vscode.ConfigurationTarget.Workspace);
					await waitForClientQuiescent('symbolic workspace preprocessor macros restore settled');
				}
			});
		});

		it('resolves CSV variant macros from active-unit selection source without workspace hints', async () => {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				const configuration = vscode.workspace.getConfiguration('nsf');
				const inspectedMacros = configuration.inspect<Record<string, unknown>>('preprocessorMacros');
				const originalMacros = inspectedMacros?.workspaceValue;
				const sanitizedMacros = { ...(originalMacros ?? {}) };
				delete sanitizedMacros.CSV_CONFLICTING_MODE;
				delete sanitizedMacros.CSV_MODE_SELECTED;
				await configuration.update('preprocessorMacros', sanitizedMacros, vscode.ConfigurationTarget.Workspace);
				await waitForClientQuiescent('workspace preprocessor macros sanitized for active-unit selection source test');

				const selectionSourcePath = path.join(
					getWorkspaceRoot(),
					'test_files',
					'include_context',
					'active_unit_variant_selection.csv'
				);
				const sourceExisted = fs.existsSync(selectionSourcePath);
				const originalSource = sourceExisted ? fs.readFileSync(selectionSourcePath, 'utf8') : undefined;
				fs.writeFileSync(
					selectionSourcePath,
					'multi_context_variant_profile_csv,CSV_CONFLICTING_MODE=0\n',
					'utf8'
				);

				const csvUnit = await openFixture('include_context/units/multi_context_variant_profile_csv.nsf');
				try {
					await waitForIndexingIdle('indexing idle before active-unit selection source test');
					await vscode.commands.executeCommand('nsf._setActiveUnitForTests', csvUnit.uri.toString());
					await waitForActiveUnitAndVisibilityReadyForTests(
						csvUnit.uri.toString(),
						path.join('include_context', 'units', 'multi_context_variant_profile_csv.nsf'),
						'active unit ready for active-unit selection source test'
					);
					const entries = await waitFor(
						() => getDocumentRuntimeDebug([csvUnit.uri.toString()]),
						(value) => Boolean(value[0]?.analysisFullFingerprint),
						'active-unit selection source runtime debug'
					);
					const entry = entries[0];
					const defines = entry?.activeUnitProfileDefines as Record<string, number> | undefined;
					assert.strictEqual(defines?.CSV_CONFLICTING_MODE, 0);
					assert.strictEqual(entry.activeUnitProfileTotalRowCount, 2);
					assert.strictEqual(entry.activeUnitProfileSelectedRowCount, 1);
					assert.ok(
						(entry.activeUnitProfileSelectedRowSignature ?? '').includes('CSV_CONFLICTING_MODE=0'),
						`Expected selected row signature to include CSV_CONFLICTING_MODE=0, got ${entry.activeUnitProfileSelectedRowSignature ?? '<missing>'}.`
					);
					assert.ok(
						entry.activeUnitProfileSelectionHintSourcePath?.endsWith('active_unit_variant_selection.csv'),
						`Expected selection hint source path to end with active_unit_variant_selection.csv, got ${entry.activeUnitProfileSelectionHintSourcePath ?? '<missing>'}.`
					);
					assert.ok(
						!(entry.activeUnitProfileUnresolvedMacros ?? []).includes('CSV_CONFLICTING_MODE'),
						`Expected CSV_CONFLICTING_MODE to be resolved by active-unit selection source, got ${JSON.stringify(entry.activeUnitProfileUnresolvedMacros ?? [])}.`
					);
				} finally {
					await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
					await waitForClientQuiescent('active-unit selection source cleanup settled');
					await openFixture('module_suite.nsf');
					if (sourceExisted) {
						fs.writeFileSync(selectionSourcePath, originalSource ?? '', 'utf8');
					} else if (fs.existsSync(selectionSourcePath)) {
						fs.unlinkSync(selectionSourcePath);
					}
					await configuration.update('preprocessorMacros', originalMacros, vscode.ConfigurationTarget.Workspace);
					await waitForClientQuiescent('workspace preprocessor macros restore settled for active-unit selection source test');
				}
			});
		});
	});
}
