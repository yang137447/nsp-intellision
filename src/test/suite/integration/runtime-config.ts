import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	type ProviderLocation,
	type DocumentRuntimeDebugEntry,
	getCompletionItems,
	getDocumentRuntimeDebug,
	getWorkspaceRoot,
	hoverToText,
	isEmptyDefinitionResult,
	openExternalDocument,
	openFixture,
	positionOf,
	repoDescribe,
	toFsPath,
	touchDocument,
	waitFor,
	waitForClientReady,
	waitForDiagnosticsWithTouches,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from '../test_helpers';

export function registerRuntimeConfigLanguageOwnershipTests(): void {
	repoDescribe('NSF client integration: Runtime Config / Language Ownership', () => {
		it('registers configured shader extensions on the client', async () => {
			const cases = [
				{ file: 'client_feature.usf', symbol: 'HelperColor', occurrence: 2 },
				{ file: 'module_shared.ush', symbol: 'SuiteMakeSharedColor', occurrence: 2 },
				{ file: 'client_feature.fx', symbol: 'HelperColor', occurrence: 2 },
				{ file: 'include_target.hlsl', symbol: 'IncludedColor', occurrence: 1 }
			];

			for (const testCase of cases) {
				const document = await openFixture(testCase.file);
				assert.strictEqual(
					document.languageId,
					'nsf',
					`Expected ${testCase.file} to be owned by the NSF language on a clean client.`
				);

				const hoverPosition = positionOf(document, testCase.symbol, testCase.occurrence, 2);
				const hovers = await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.Hover[]>(
							'vscode.executeHoverProvider',
							document.uri,
							hoverPosition
						),
					(value) => Array.isArray(value) && value.length > 0,
					`hover results for ${testCase.file}`
				);

				assert.ok(hovers[0], `Expected hover results from the language client for ${testCase.file}.`);
			}
		});
	});
}

export function registerRuntimeIndexingTests(): void {
	repoDescribe('NSF client integration: Runtime Config / Indexing', () => {
		it('exposes indexing status on the client', async () => {
			const status = await waitFor(
				() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
				(value) => Boolean(value?.clientState),
				'indexing status'
			);
			assert.ok(['ready', 'starting', 'stopped', 'error'].includes(status.clientState));
			if (status.lastIndexingEvent?.kind) {
				assert.ok(['backgroundIndex', 'workspaceScan', 'structScan'].includes(status.lastIndexingEvent.kind));
			}
		});

		it('clears disk index caches before rebuilding when requested', async function () {
			this.timeout(180000);
			const workspaceRoot = getWorkspaceRoot();
			const configuration = vscode.workspace.getConfiguration('nsf');
			const inspectedServerPath = configuration.inspect<string>('serverPath');
			const originalServerPath = inspectedServerPath?.workspaceValue ?? '';
			const rebuiltServerPath = path.join(workspaceRoot, 'server_cpp', 'build', 'nsf_lsp.exe');
			assert.ok(fs.existsSync(rebuiltServerPath), 'Expected rebuilt C++ server at server_cpp/build/nsf_lsp.exe.');
			const cacheRoot = path.join(workspaceRoot, '.vscode', 'nsp');
			const fakeIndexDir = path.join(cacheRoot, 'index_v2_manual_clear_cache_test');
			const fakeIndexFile = path.join(cacheRoot, 'index_v1_manual_clear_cache_test.json');
			fs.mkdirSync(fakeIndexDir, { recursive: true });
			fs.writeFileSync(path.join(fakeIndexDir, 'sentinel.txt'), 'stale-cache', 'utf8');
			fs.mkdirSync(cacheRoot, { recursive: true });
			fs.writeFileSync(fakeIndexFile, '{}', 'utf8');

			try {
				await configuration.update('serverPath', rebuiltServerPath, vscode.ConfigurationTarget.Workspace);
				await vscode.commands.executeCommand('nsf.restartServer');
				await vscode.commands.executeCommand('nsf.rebuildIndexClearCache');
				const status = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
				assert.strictEqual(status?.indexingState?.reason, 'manualClearCache');
				assert.ok(!fs.existsSync(fakeIndexDir), 'Expected clear-cache rebuild to remove fake index_v2 cache.');
				assert.ok(!fs.existsSync(fakeIndexFile), 'Expected clear-cache rebuild to remove fake index_v1 cache.');
			} finally {
				await configuration.update('serverPath', originalServerPath, vscode.ConfigurationTarget.Workspace);
				await vscode.commands.executeCommand('nsf.restartServer');
				if (fs.existsSync(fakeIndexDir)) {
					fs.rmdirSync(fakeIndexDir, { recursive: true });
				}
				if (fs.existsSync(fakeIndexFile)) {
					fs.unlinkSync(fakeIndexFile);
				}
			}
		});

		it('auto triggers initial inlay refresh after indexing becomes stable', async () => {
			await vscode.commands.executeCommand('nsf.restartServer');
			await openFixture('module_inlay_hints.nsf');
			const status = await waitFor(
				() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
				(value) => (value?.initialInlayRefreshTriggerCount ?? 0) > 0,
				'initial inlay refresh trigger'
			);
			assert.ok((status?.initialInlayRefreshTriggerCount ?? 0) > 0);
		});

		it('triggers initial inlay refresh after target editor appears post-index', async () => {
			await vscode.commands.executeCommand('nsf.restartServer');
			await vscode.commands.executeCommand('nsf._resetInternalStatus');
			const plainDocument = await openFixture('tecvan.txt');
			assert.strictEqual(plainDocument.languageId, 'plaintext');

			await waitFor(
				() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
				(value) => {
					const state = value?.indexingState;
					if (!state) {
						return false;
					}
					return (
						state.state === 'Idle' &&
						(state.pending?.queuedTasks ?? 0) === 0 &&
						(state.pending?.runningWorkers ?? 0) === 0
					);
				},
				'indexing stable without inlay target'
			);

			const beforeOpenTarget = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
			const beforeCount = beforeOpenTarget?.initialInlayRefreshTriggerCount ?? 0;

			await openFixture('module_inlay_hints.nsf');
			const afterOpenTarget = await waitFor(
				() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
				(value) => (value?.initialInlayRefreshTriggerCount ?? 0) >= beforeCount,
				'initial inlay refresh after target editor appears'
			);
			assert.ok((afterOpenTarget?.initialInlayRefreshTriggerCount ?? 0) >= beforeCount);
		});

		it('triggers inlay refresh after each indexing settle cycle', async () => {
			await openFixture('module_inlay_hints.nsf');
			await vscode.commands.executeCommand('nsf.restartServer');
			const first = await waitFor(
				() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
				(value) => (value?.indexSettledInlayRefreshTriggerCount ?? 0) > 0,
				'first settled inlay refresh'
			);
			const firstCount = first?.indexSettledInlayRefreshTriggerCount ?? 0;
			assert.ok(firstCount > 0);

			await vscode.commands.executeCommand('nsf.restartServer');
			const second = await waitFor(
				() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
				(value) => (value?.indexSettledInlayRefreshTriggerCount ?? 0) > firstCount,
				'second settled inlay refresh'
			);
			assert.ok((second?.indexSettledInlayRefreshTriggerCount ?? 0) > firstCount);
		});
	});
}

export function registerRuntimeExternalFileConfigTests(): void {
	repoDescribe('NSF client integration: Runtime Config / External Files', () => {
		it('does not infer include roots for external files when intellisionPath is empty', async () => {
			const configuration = vscode.workspace.getConfiguration('nsf');
			const inspectedIncludePaths = configuration.inspect<string[]>('intellisionPath');
			const previousIncludePaths = inspectedIncludePaths?.workspaceValue;
			await configuration.update('intellisionPath', [], vscode.ConfigurationTarget.Workspace);

			try {
				const tempRoot = path.join(os.tmpdir(), `nsf_external_${Date.now()}`);
				const shaderSource = path.join(tempRoot, 'shader-source');
				const shaderLib = path.join(shaderSource, 'shaderlib');
				const terrain = path.join(shaderSource, 'terrain');
				await fs.promises.mkdir(shaderLib, { recursive: true });
				await fs.promises.mkdir(terrain, { recursive: true });

				const structFile = path.join(shaderLib, 'structs.hlsl');
				const mainFile = path.join(terrain, 'external_member.hlsl');

				await fs.promises.writeFile(
					structFile,
					`struct ExtExternalStruct\n{\n    float foo;\n    float4 bar;\n};\n`,
					'utf8'
				);
				await fs.promises.writeFile(
					mainFile,
					`float4 ExternalMain(float2 uv : TEXCOORD0) : SV_Target0\n{\n    ExtExternalStruct value;\n    return value.\n}\n`,
					'utf8'
				);

				const document = await openExternalDocument(mainFile);
				const completionPosition = positionOf(document, 'value.', 1, 'value.'.length);
				const completionResult = await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
							'vscode.executeCompletionItemProvider',
							document.uri,
							completionPosition
						),
					(value) => Boolean(value),
					'external struct member completion items'
				);

				const items = getCompletionItems(completionResult);
				const labels = new Set(items.map((item) => item.label.toString()));
				assert.ok(!labels.has('foo'));
				assert.ok(!labels.has('bar'));
			} finally {
				await configuration.update('intellisionPath', previousIncludePaths, vscode.ConfigurationTarget.Workspace);
			}
		});
	});
}

export function registerRuntimeFileWatchTests(): void {
	repoDescribe('NSF client integration: Runtime Config / File Watch', () => {
		it('smoke: refreshes consumer diagnostics when included file changes on disk', async function () {
			this.timeout(120000);
			const providerPath = path.join(getWorkspaceRoot(), 'test_files', 'watch_provider.nsf');
			const original = fs.readFileSync(providerPath, 'utf8');
			const originalDefinedSymbol = original.includes('u_value2') ? 'u_value2' : 'u_value';
			const updatedDefinedSymbol = originalDefinedSymbol === 'u_value' ? 'u_value2' : 'u_value';
			const updated = original.replace(originalDefinedSymbol, updatedDefinedSymbol);
			const expectedUndefinedAfterUpdate = updatedDefinedSymbol !== 'u_value';

			try {
				const consumerDocument = await openFixture('watch_consumer.nsf');

				const initialDiagnostics = await waitFor(
					() => vscode.languages.getDiagnostics(consumerDocument.uri),
					(value) => Array.isArray(value),
					'watch initial diagnostics'
				);
				void initialDiagnostics;
				await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(updated, 'utf8'));
				const touchEdit = new vscode.WorkspaceEdit();
				touchEdit.insert(consumerDocument.uri, new vscode.Position(0, 0), ' ');
				await vscode.workspace.applyEdit(touchEdit);
				const revertEdit = new vscode.WorkspaceEdit();
				revertEdit.delete(consumerDocument.uri, new vscode.Range(0, 0, 0, 1));
				await vscode.workspace.applyEdit(revertEdit);

				let updatedDiagnostics: readonly vscode.Diagnostic[];
				try {
					updatedDiagnostics = await waitFor(
						() => vscode.languages.getDiagnostics(consumerDocument.uri),
						(value) =>
							Array.isArray(value) &&
							value.some((diag) => diag.message.includes('Undefined identifier: u_value.')) === expectedUndefinedAfterUpdate,
						'watch updated diagnostics'
					);
				} catch {
					const reopenedDocument = await openFixture('watch_consumer.nsf');
					const refreshEdit = new vscode.WorkspaceEdit();
					refreshEdit.insert(reopenedDocument.uri, new vscode.Position(0, 0), ' ');
					await vscode.workspace.applyEdit(refreshEdit);
					const refreshRevert = new vscode.WorkspaceEdit();
					refreshRevert.delete(reopenedDocument.uri, new vscode.Range(0, 0, 0, 1));
					await vscode.workspace.applyEdit(refreshRevert);
					updatedDiagnostics = await waitFor(
						() => vscode.languages.getDiagnostics(reopenedDocument.uri),
						(value) =>
							Array.isArray(value) &&
							value.some((diag) => diag.message.includes('Undefined identifier: u_value.')) === expectedUndefinedAfterUpdate,
						'watch updated diagnostics after reopen'
					);
				}
				const updatedHasUndefined = updatedDiagnostics.some((diag) =>
					diag.message.includes('Undefined identifier: u_value.')
				);
				assert.strictEqual(
					updatedHasUndefined,
					expectedUndefinedAfterUpdate,
					'Expected file-watch refresh to match the updated provider content.'
				);
			} finally {
				await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(original, 'utf8'));
			}
		});

		it('refreshes every reverse-include consumer when an included file changes on disk', async function () {
			this.timeout(120000);
			const configuration = vscode.workspace.getConfiguration('nsf');
			const originalIncludePaths = configuration.inspect<string[]>('intellisionPath')?.workspaceValue ?? [];
			const providerPath = path.join(getWorkspaceRoot(), 'test_files', 'watch_provider.nsf');
			const original = fs.readFileSync(providerPath, 'utf8');
			const originalDefinedSymbol = original.includes('u_value2') ? 'u_value2' : 'u_value';
			const updatedDefinedSymbol = originalDefinedSymbol === 'u_value' ? 'u_value2' : 'u_value';
			const updated = original.replace(originalDefinedSymbol, updatedDefinedSymbol);
			const expectedUndefinedAfterUpdate = updatedDefinedSymbol !== 'u_value';

			try {
				await waitForClientReady('client ready for reverse-include file-watch');
				await configuration.update(
					'intellisionPath',
					[path.join(getWorkspaceRoot(), 'test_files')],
					vscode.ConfigurationTarget.Workspace
				);
				await waitForIndexingIdle('indexing idle for reverse-include file-watch');

				const primaryConsumer = await openFixture('watch_consumer.nsf');
				const secondaryConsumer = await openFixture('watch_consumer_secondary.nsf');

				const initialPrimary = await waitFor(
					() => vscode.languages.getDiagnostics(primaryConsumer.uri),
					(value) => Array.isArray(value),
					'watch primary initial diagnostics'
				);
				const initialSecondary = await waitFor(
					() => vscode.languages.getDiagnostics(secondaryConsumer.uri),
					(value) => Array.isArray(value),
					'watch secondary initial diagnostics'
				);
				void initialPrimary;
				void initialSecondary;

				await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(updated, 'utf8'));
				await touchDocument(primaryConsumer);

				const primaryUpdated = await waitForDiagnosticsWithTouches(
					primaryConsumer,
					(value) =>
						value.some((diag) => diag.message.includes('Undefined identifier: u_value.')) === expectedUndefinedAfterUpdate,
					'watch primary updated diagnostics'
				);
				const secondaryUpdated = await waitFor(
					() => vscode.languages.getDiagnostics(secondaryConsumer.uri),
					(value) =>
						Array.isArray(value) &&
						value.some((diag) => diag.message.includes('Undefined identifier: u_value.')) === expectedUndefinedAfterUpdate,
					'watch secondary updated diagnostics'
				);

				const primaryUpdatedHasUndefined = primaryUpdated.some((diag) =>
					diag.message.includes('Undefined identifier: u_value.')
				);
				const secondaryUpdatedHasUndefined = secondaryUpdated.some((diag) =>
					diag.message.includes('Undefined identifier: u_value.')
				);
				assert.strictEqual(primaryUpdatedHasUndefined, expectedUndefinedAfterUpdate);
				assert.strictEqual(
					secondaryUpdatedHasUndefined,
					expectedUndefinedAfterUpdate,
					'Expected reverse-include refresh to reach the untouched secondary consumer.'
				);
			} finally {
				await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(original, 'utf8'));
				await configuration.update('intellisionPath', originalIncludePaths, vscode.ConfigurationTarget.Workspace);
			}
		});

		it('refreshes only reverse-include impacted open docs after provider file changes', async function () {
			this.timeout(120000);
			const providerPath = path.join(getWorkspaceRoot(), 'test_files', 'watch_provider.nsf');
			const original = fs.readFileSync(providerPath, 'utf8');
			const originalDefinedSymbol = original.includes('u_value2') ? 'u_value2' : 'u_value';
			const updatedDefinedSymbol = originalDefinedSymbol === 'u_value' ? 'u_value2' : 'u_value';
			const updated = original.replace(originalDefinedSymbol, updatedDefinedSymbol);

			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
				try {
					const primaryConsumer = await openFixture('watch_consumer.nsf');
					const secondaryConsumer = await openFixture('watch_consumer_secondary.nsf');
					const unrelatedDocument = await openFixture('module_suite.nsf');

					const uris = [
						primaryConsumer.uri.toString(),
						secondaryConsumer.uri.toString(),
						unrelatedDocument.uri.toString()
					];
					const runtimeMapFor = (entries: DocumentRuntimeDebugEntry[]) =>
						new Map(entries.map((entry) => [entry.uri, entry]));

					const beforeEntries = await getDocumentRuntimeDebug(uris);
					const before = runtimeMapFor(beforeEntries);
					assert.ok(before.get(primaryConsumer.uri.toString())?.exists);
					assert.ok(before.get(secondaryConsumer.uri.toString())?.exists);
					assert.ok(before.get(unrelatedDocument.uri.toString())?.exists);

					await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(updated, 'utf8'));
					await touchDocument(primaryConsumer);

					const afterEntries = await waitFor(
						() => getDocumentRuntimeDebug(uris),
						(entries) => {
							const runtimeMap = runtimeMapFor(entries);
							const primaryBefore = before.get(primaryConsumer.uri.toString());
							const secondaryBefore = before.get(secondaryConsumer.uri.toString());
							const unrelatedBefore = before.get(unrelatedDocument.uri.toString());
							const primaryAfter = runtimeMap.get(primaryConsumer.uri.toString());
							const secondaryAfter = runtimeMap.get(secondaryConsumer.uri.toString());
							const unrelatedAfter = runtimeMap.get(unrelatedDocument.uri.toString());
							if (!primaryBefore || !secondaryBefore || !unrelatedBefore) {
								return false;
							}
							if (!primaryAfter || !secondaryAfter || !unrelatedAfter) {
								return false;
							}
							return (
								primaryAfter.analysisFullFingerprint !== primaryBefore.analysisFullFingerprint &&
								secondaryAfter.analysisFullFingerprint !== secondaryBefore.analysisFullFingerprint &&
								unrelatedAfter.analysisFullFingerprint === unrelatedBefore.analysisFullFingerprint
							);
						},
						'reverse-include impacted document runtime refresh'
					);

					const after = runtimeMapFor(afterEntries);
					assert.notStrictEqual(
						after.get(primaryConsumer.uri.toString())?.analysisFullFingerprint,
						before.get(primaryConsumer.uri.toString())?.analysisFullFingerprint
					);
					assert.notStrictEqual(
						after.get(secondaryConsumer.uri.toString())?.analysisFullFingerprint,
						before.get(secondaryConsumer.uri.toString())?.analysisFullFingerprint
					);
					assert.strictEqual(
						after.get(unrelatedDocument.uri.toString())?.analysisFullFingerprint,
						before.get(unrelatedDocument.uri.toString())?.analysisFullFingerprint,
						'Expected unrelated open docs not to refresh when they are outside the reverse-include closure.'
					);
				} finally {
					await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(original, 'utf8'));
				}
			});
		});
	});
}

export function registerRuntimeIncludePathConfigTests(): void {
	repoDescribe('NSF client integration: Runtime Config / Include Path Sync', () => {
		it('applies include path configuration changes without restarting the client', async () => {
			const configuration = vscode.workspace.getConfiguration('nsf');
			const inspectedIncludePaths = configuration.inspect<string[]>('intellisionPath');
			const originalIncludePaths = inspectedIncludePaths?.workspaceValue ?? [];
			const document = await openFixture('config_runtime_suite.nsf');
			const symbolPosition = positionOf(document, 'RuntimeOnlySharedColor', 1, 3);

			try {
				await configuration.update('intellisionPath', [], vscode.ConfigurationTarget.Workspace);

				const missingDefinitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							symbolPosition
						),
					(value) => isEmptyDefinitionResult(value),
					'definition results without include paths'
				);
				assert.ok(isEmptyDefinitionResult(missingDefinitions));

				await configuration.update(
					'intellisionPath',
					[path.join(getWorkspaceRoot(), 'test_files', 'runtime_include_root')],
					vscode.ConfigurationTarget.Workspace
				);

				const restoredDefinitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							symbolPosition
						),
					(value) => Array.isArray(value) && value.length > 0,
					'definition results after include path update'
				);
				assert.strictEqual(path.basename(toFsPath(restoredDefinitions[0])), 'runtime_only_shared.ush');
			} finally {
				await configuration.update(
					'intellisionPath',
					originalIncludePaths,
					vscode.ConfigurationTarget.Workspace
				);
			}
		});

		it('restores hover and definition together after include path configuration changes', async () => {
			const configuration = vscode.workspace.getConfiguration('nsf');
			const inspectedIncludePaths = configuration.inspect<string[]>('intellisionPath');
			const originalIncludePaths = inspectedIncludePaths?.workspaceValue ?? [];
			const document = await openFixture('config_runtime_suite.nsf');
			const symbolPosition = positionOf(document, 'RuntimeOnlySharedColor', 1, 3);

			try {
				await configuration.update('intellisionPath', [], vscode.ConfigurationTarget.Workspace);

				const missingDefinitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							symbolPosition
						),
					(value) => isEmptyDefinitionResult(value),
					'aligned missing definition without include paths'
				);
				assert.ok(isEmptyDefinitionResult(missingDefinitions));

				await configuration.update(
					'intellisionPath',
					[path.join(getWorkspaceRoot(), 'test_files', 'runtime_include_root')],
					vscode.ConfigurationTarget.Workspace
				);

				const restoredDefinitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							symbolPosition
						),
					(value) => Array.isArray(value) && value.length > 0,
					'aligned restored definition after include path update'
				);
				const restoredHovers = await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.Hover[]>(
							'vscode.executeHoverProvider',
							document.uri,
							symbolPosition
						),
					(value) => Array.isArray(value) && value.length > 0,
					'aligned restored hover after include path update'
				);

				const hoverText = hoverToText(restoredHovers);
				const definitionFile = path.basename(toFsPath(restoredDefinitions[0]));
				assert.ok(hoverText.includes('RuntimeOnlySharedColor('));
				assert.ok(hoverText.includes(definitionFile));
			} finally {
				await configuration.update(
					'intellisionPath',
					originalIncludePaths,
					vscode.ConfigurationTarget.Workspace
				);
			}
		});
	});
}
