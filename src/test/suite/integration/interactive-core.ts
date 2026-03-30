import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	type ProviderLocation,
	getCompletionItems,
	getWorkspaceRoot,
	hoverToText,
	openFixture,
	positionOf,
	repoDescribe,
	toFsPath,
	toRange,
	waitFor,
	waitForClientReady,
	waitForCompletionLabels,
	waitForHoverText,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from '../test_helpers';

export function registerInteractiveRuntimeCoreTests(): void {
	repoDescribe('NSF client integration: Interactive Runtime / Core', () => {
	it('provides completion items through the client', async () => {
		const document = await openFixture('module_suite.nsf');
		const completionPosition = new vscode.Position(0, 0);
		const completionItems = await waitForCompletionLabels(
			document,
			completionPosition,
			['#include', 'discard', 'float4'],
			'completion items with lsp labels'
		);

		const labels = new Set(completionItems.map((item) => item.label.toString()));
		assert.ok(labels.has('#include'), 'Expected #include completion item.');
		assert.ok(labels.has('discard'), 'Expected discard completion item.');
		assert.ok(labels.has('float4'), 'Expected float4 completion item.');
	});

	it('merges current-doc locals, globals, and functions into completion items', async () => {
		const document = await openFixture('module_completion_current_doc.nsf');
		const completionPosition = positionOf(document, 'return Comp', 1, 'return Comp'.length);
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'current-doc completion items'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('CompletionDocHelper'));
		assert.ok(labels.has('CompletionDocEntry'));
		assert.ok(labels.has('CompletionDocGlobal'));
		assert.ok(labels.has('completionLocalColor'));
	});

	it('prefers current-doc function symbols over workspace summary conflicts', async () => {
		await withTemporaryIntellisionPath(
			[path.join(getWorkspaceRoot(), 'test_files', 'current_doc_precedence_root')],
			async () => {
				const document = await openFixture('module_current_doc_precedence_local.nsf');

				const completionPosition = positionOf(document, 'return Shado', 1, 'return Shado'.length);
				const completionItems = await waitForCompletionLabels(
					document,
					completionPosition,
					['ShadowedCall'],
					'current-doc precedence completion items'
				);
				const shadowedCompletion = completionItems.find(
					(item) => item.label.toString() === 'ShadowedCall'
				);
				assert.ok(shadowedCompletion, 'Expected ShadowedCall completion item.');
				assert.strictEqual(
					shadowedCompletion.detail,
					'float3',
					'Expected current-doc function detail to win over workspace summary detail.'
				);

				const callPosition = positionOf(document, 'ShadowedCall(localValue, 2.0)', 1, 2);
				const hovers = await waitForHoverText(
					document,
					callPosition,
					(text) =>
						text.includes('local ShadowedCall doc line 1') &&
						text.includes('float3 ShadowedCall(float3 value, float gain)'),
					'current-doc precedence hover'
				);
				const hoverText = hoverToText(hovers);
				assert.ok(hoverText.includes('local ShadowedCall doc line 2'));
				assert.ok(!hoverText.includes('remote ShadowedCall doc line 1'));

				const signaturePosition = positionOf(
					document,
					'ShadowedCall(localValue, 2.0)',
					1,
					'ShadowedCall(localValue, '.length
				);
				const help = await waitFor(
					() =>
						vscode.commands.executeCommand<vscode.SignatureHelp>(
							'vscode.executeSignatureHelpProvider',
							document.uri,
							signaturePosition
						),
					(value) =>
						Boolean(value) &&
						value.signatures.length > 0 &&
						value.signatures.some((signature) =>
							signature.label.includes('float3 ShadowedCall(float3 value, float gain)')
						),
					'current-doc precedence signature help'
				);
				assert.ok(
					help.signatures.some((signature) =>
						signature.label.includes('float3 ShadowedCall(float3 value, float gain)')
					),
					'Expected signature help to prefer the current-doc overload.'
				);
				assert.ok(
					!help.signatures.some((signature) =>
						signature.label.includes('float4 ShadowedCall(float2 uv, int mode)')
					),
					'Expected workspace-summary overloads not to override current-doc signature help.'
				);

				const definitions = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							document.uri,
							callPosition
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) => toFsPath(location) === document.uri.fsPath),
					'current-doc precedence definition'
				);
				assert.ok(
					definitions.some((location) => toFsPath(location) === document.uri.fsPath),
					'Expected definition to remain on the current document.'
				);
				assert.ok(
					definitions.every((location) =>
						path.basename(toFsPath(location)) !== 'shadowed_symbols.ush'
					),
					'Expected workspace summary conflict not to replace current-doc definition.'
				);
			}
		);
		await openFixture('module_suite.nsf');
	});

	it('provides completion items for HLSL attributes inside brackets', async () => {
		const document = await openFixture('module_completion_attribute.nsf');
		const completionPosition = new vscode.Position(0, 1);
		const completionItems = await waitForCompletionLabels(
			document,
			completionPosition,
			['unroll', 'loop'],
			'completion items for attributes'
		);

		const labels = new Set(completionItems.map((item) => item.label.toString()));
		assert.ok(labels.has('unroll'));
		assert.ok(labels.has('loop'));
	});

	it('shows hover documentation for declarations and HLSL intrinsics', async () => {
		const document = await openFixture('module_hover_docs.nsf');

		const varHoverPosition = positionOf(document, 'SuiteHoverVar', 2, 2);
		const varHovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					varHoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for SuiteHoverVar'
		);
		const varHoverText = hoverToText(varHovers);
		assert.ok(varHoverText.includes('SuiteHoverVar doc line 1'), varHoverText);
		assert.ok(varHoverText.includes('SuiteHoverVar doc line 2'), varHoverText);

		const funcHoverPosition = positionOf(document, 'SuiteHoverFunc', 2, 2);
		const funcHovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					funcHoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for SuiteHoverFunc'
		);
		const funcHoverText = hoverToText(funcHovers);
		assert.ok(funcHoverText.includes('SuiteHoverFunc doc line 1'));
		assert.ok(funcHoverText.includes('SuiteHoverFunc doc line 2'));
		assert.ok(funcHoverText.includes('SuiteHoverFunc('));
		assert.ok(funcHoverText.includes('(HLSL function)'));
		assert.ok(funcHoverText.includes('Returns:'));
		assert.ok(funcHoverText.includes('Parameters:'));

		const intrinsicHoverPosition = positionOf(document, 'saturate', 1, 2);
		const intrinsicHovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					intrinsicHoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for saturate'
		);
		const intrinsicHoverText = hoverToText(intrinsicHovers);
		assert.ok(intrinsicHoverText.includes('Clamps the specified value within the range of 0 to 1.'));
	});

	it('surfaces include-context ambiguity in hover when no active unit is selected', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedRoots = configuration.inspect<string[]>('intellisionPath');
		const originalRoots = inspectedRoots?.workspaceValue ?? [];
		await configuration.update(
			'intellisionPath',
			[path.join(getWorkspaceRoot(), 'test_files', 'include_context')],
			vscode.ConfigurationTarget.Workspace
		);
		await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
		try {
			const document = await openFixture('include_context/shared/multi_context_common.hlsl');
			await waitFor(
				() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
				(value) => !value?.indexingState || value.indexingState.state === 'Idle',
				'indexing status for include context hover'
			);

			const hoverPosition = positionOf(document, 'MultiContextColor', 1, 2);
			const hovers = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.Hover[]>(
						'vscode.executeHoverProvider',
						document.uri,
						hoverPosition
					),
				(value) => Array.isArray(value) && value.length > 0,
				'hover results for include context ambiguity'
			);
			const hoverText = hoverToText(hovers);
			assert.ok(hoverText.includes('float4 MultiContextColor(float2 uv)'), hoverText);
			assert.ok(!hoverText.includes('Include context ambiguous'), hoverText);
			assert.ok(!hoverText.includes('Candidate definitions ('), hoverText);
		} finally {
			await configuration.update('intellisionPath', originalRoots, vscode.ConfigurationTarget.Workspace);
			await openFixture('module_suite.nsf');
		}
	});

	it('shows include-context candidate definition summaries in hover', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedRoots = configuration.inspect<string[]>('intellisionPath');
		const originalRoots = inspectedRoots?.workspaceValue ?? [];
		await configuration.update(
			'intellisionPath',
			[path.join(getWorkspaceRoot(), 'test_files', 'include_context')],
			vscode.ConfigurationTarget.Workspace
		);
		await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
		try {
			const document = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
			await waitFor(
				() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
				(value) => !value?.indexingState || value.indexingState.state === 'Idle',
				'indexing status for include-context hover summaries'
			);

			const hoverPosition = positionOf(document, 'UnitSpecificTone', 1, 2);
			const hovers = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.Hover[]>(
						'vscode.executeHoverProvider',
						document.uri,
						hoverPosition
					),
				(value) => Array.isArray(value) && value.length > 0,
				'hover results for include-context summaries'
			);
			const hoverText = hoverToText(hovers);
			assert.ok(hoverText.includes('Candidate definitions ('));
			assert.ok(hoverText.includes('multi_context_symbol_a.nsf'));
			assert.ok(hoverText.includes('multi_context_symbol_b.nsf'));
			assert.ok(
				hoverText.includes('float4 UnitSpecificTone(float2 uv)') &&
					hoverText.includes('[multi_context_symbol_a.nsf:1](file:///'),
				hoverText
			);
			assert.ok(
				hoverText.includes('float4 UnitSpecificTone(float2 uv)') &&
					hoverText.includes('[multi_context_symbol_b.nsf:1](file:///'),
				hoverText
			);
		} finally {
			await configuration.update('intellisionPath', originalRoots, vscode.ConfigurationTarget.Workspace);
			await openFixture('module_suite.nsf');
		}
	});

	it('keeps function hover free of include-context ambiguity when an active unit is selected', async () => {
		try {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				const unitADocument = await openFixture('include_context/units/multi_context_symbol_a.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', unitADocument.uri.toString());
				await waitForIndexingIdle('indexing idle for active include-context function hover A');

				let sharedDocument = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
				const hoverPosition = positionOf(sharedDocument, 'UnitSpecificTone', 1, 2);
				const hoverForUnitA = await waitForHoverText(
					sharedDocument,
					hoverPosition,
					(text) =>
						text.includes('float4 UnitSpecificTone(float2 uv)') &&
						text.includes('multi_context_symbol_a.nsf') &&
						!text.includes('Include context ambiguous') &&
						!text.includes('Candidate definitions ('),
					'active-unit function hover A'
				);
				const hoverTextForUnitA = hoverToText(hoverForUnitA);
				assert.ok(hoverTextForUnitA.includes('(HLSL function)'), hoverTextForUnitA);
				assert.ok(!hoverTextForUnitA.includes('Include context ambiguous'), hoverTextForUnitA);
				assert.ok(!hoverTextForUnitA.includes('Candidate definitions ('), hoverTextForUnitA);

				const unitBDocument = await openFixture('include_context/units/multi_context_symbol_b.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', unitBDocument.uri.toString());
				await waitForIndexingIdle('indexing idle for active include-context function hover B');

				sharedDocument = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
				const hoverForUnitB = await waitForHoverText(
					sharedDocument,
					hoverPosition,
					(text) =>
						text.includes('float4 UnitSpecificTone(float2 uv)') &&
						text.includes('multi_context_symbol_b.nsf') &&
						!text.includes('Include context ambiguous') &&
						!text.includes('Candidate definitions ('),
					'active-unit function hover B'
				);
				const hoverTextForUnitB = hoverToText(hoverForUnitB);
				assert.ok(hoverTextForUnitB.includes('(HLSL function)'), hoverTextForUnitB);
				assert.ok(!hoverTextForUnitB.includes('Include context ambiguous'), hoverTextForUnitB);
				assert.ok(!hoverTextForUnitB.includes('Candidate definitions ('), hoverTextForUnitB);
			});
		} finally {
			await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
			await openFixture('module_suite.nsf');
		}
	});

	it('keeps current-doc parameter hover free of include-context candidate notes', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
			await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
			const document = await openFixture('include_context/shared/multi_context_parameter_common.hlsl');
			await waitForIndexingIdle('indexing idle for include-context parameter hover');

			const hoverPosition = positionOf(document, 'fog_factors', 4, 2);
			const hovers = await waitForHoverText(
				document,
				hoverPosition,
				(text) => text.includes('(Parameter)') && text.includes('Type: half4'),
				'include-context parameter hover'
			);
			const hoverText = hoverToText(hovers);
			assert.ok(hoverText.includes('(Parameter)'));
			assert.ok(hoverText.includes('Type: half4'));
			assert.ok(hoverText.includes('Defined at:'));
			assert.ok(!hoverText.includes('Include context ambiguous'), hoverText);
			assert.ok(!hoverText.includes('Candidate definitions ('), hoverText);
		});
	});

	it('renders object-like and function-like macro hover without misclassifying them as HLSL functions', async () => {
		const document = await openFixture('module_hover_macros.nsf');

		const objectDefinitionHovers = await waitForHoverText(
			document,
			positionOf(document, 'FEATURE_LEVEL', 1, 2),
			(text) => text.includes('#define FEATURE_LEVEL'),
			'object-like macro definition hover'
		);
		const objectDefinitionText = hoverToText(objectDefinitionHovers);
		assert.ok(objectDefinitionText.includes('(Macro)'), objectDefinitionText);
		assert.ok(!objectDefinitionText.includes('(HLSL function)'), objectDefinitionText);
		assert.ok(!objectDefinitionText.includes('Returns:'), objectDefinitionText);

		const objectUsageHovers = await waitForHoverText(
			document,
			positionOf(document, 'FEATURE_LEVEL', 2, 2),
			(text) => text.includes('#define FEATURE_LEVEL'),
			'object-like macro usage hover'
		);
		const objectUsageText = hoverToText(objectUsageHovers);
		assert.ok(objectUsageText.includes('(Macro)'), objectUsageText);
		assert.ok(!objectUsageText.includes('(HLSL function)'), objectUsageText);

		const functionLikeDefinitionHovers = await waitForHoverText(
			document,
			positionOf(document, 'WRAP_COLOR', 1, 2),
			(text) => text.includes('#define WRAP_COLOR(value)'),
			'function-like macro definition hover'
		);
		const functionLikeDefinitionText = hoverToText(functionLikeDefinitionHovers);
		assert.ok(functionLikeDefinitionText.includes('(Function-like macro)'), functionLikeDefinitionText);
		assert.ok(!functionLikeDefinitionText.includes('(HLSL function)'), functionLikeDefinitionText);
		assert.ok(!functionLikeDefinitionText.includes('Returns:'), functionLikeDefinitionText);

		const functionLikeUsageHovers = await waitForHoverText(
			document,
			positionOf(document, 'WRAP_COLOR', 2, 2),
			(text) => text.includes('#define WRAP_COLOR(value)'),
			'function-like macro usage hover'
		);
		const functionLikeUsageText = hoverToText(functionLikeUsageHovers);
		assert.ok(functionLikeUsageText.includes('(Function-like macro)'), functionLikeUsageText);
		assert.ok(!functionLikeUsageText.includes('(HLSL function)'), functionLikeUsageText);
	});

	it('drops last-good interactive context when the active unit changes', async () => {
		try {
			await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files', 'include_context')], async () => {
				await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');

				await openFixture('include_context/units/multi_context_symbol_a.nsf');
				let sharedDocument = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
				const symbolPosition = positionOf(sharedDocument, 'UnitSpecificTone', 1, 2);

				const definitionsForUnitA = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							sharedDocument.uri,
							symbolPosition
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) => path.basename(toFsPath(location)) === 'multi_context_symbol_a.nsf'),
					'definition results for active unit A'
				);
				assert.ok(
					definitionsForUnitA.some(
						(location) => path.basename(toFsPath(location)) === 'multi_context_symbol_a.nsf'
					)
				);

				await openFixture('include_context/units/multi_context_symbol_b.nsf');
				sharedDocument = await openFixture('include_context/shared/multi_context_symbol_common.hlsl');
				const definitionsForUnitB = await waitFor(
					() =>
						vscode.commands.executeCommand<ProviderLocation[]>(
							'vscode.executeDefinitionProvider',
							sharedDocument.uri,
							symbolPosition
						),
					(value) =>
						Array.isArray(value) &&
						value.some((location) => path.basename(toFsPath(location)) === 'multi_context_symbol_b.nsf'),
					'definition results for active unit B'
				);

				const resolvedFiles = definitionsForUnitB.map((location) => path.basename(toFsPath(location)));
				assert.ok(resolvedFiles.includes('multi_context_symbol_b.nsf'), resolvedFiles.join(','));
				assert.ok(
					!resolvedFiles.includes('multi_context_symbol_a.nsf'),
					`Expected active-unit switch to invalidate stale current-doc context. Actual=${resolvedFiles.join(',')}`
				);
			});
		} finally {
			await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
			await openFixture('module_suite.nsf');
		}
	});

	it('drops last-good interactive context when defines configuration changes', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const previousDefines = configuration.inspect<string[]>('defines')?.workspaceValue;
		const document = await openFixture('module_hover_define_config_branch.nsf');
		const completionPosition = positionOf(document, 'return value.', 1, 'return value.'.length);
		await waitForClientReady('client ready before defines switch');

		try {
			await configuration.update('defines', [], vscode.ConfigurationTarget.Workspace);
			const inactiveItems = await waitForCompletionLabels(
				document,
				completionPosition,
				['inactive_only', 'shared'],
				'completion after defines disabled'
			);
			const inactiveLabels = new Set(inactiveItems.map((item) => item.label.toString()));
			assert.ok(inactiveLabels.has('inactive_only'));
			assert.ok(inactiveLabels.has('shared'));
			assert.ok(!inactiveLabels.has('active_only'));

			await configuration.update('defines', ['USE_CONFIG_BRANCH=1'], vscode.ConfigurationTarget.Workspace);
			const activeItems = await waitForCompletionLabels(
				document,
				completionPosition,
				['active_only', 'shared'],
				'completion after defines enabled'
			);
			const activeLabels = new Set(activeItems.map((item) => item.label.toString()));
			assert.ok(activeLabels.has('active_only'));
			assert.ok(activeLabels.has('shared'));
			assert.ok(!activeLabels.has('inactive_only'));
		} finally {
			await configuration.update('defines', previousDefines, vscode.ConfigurationTarget.Workspace);
			await openFixture('module_suite.nsf');
		}
	});

	it('drops last-good interactive context when intellisionPath changes', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const originalRoots = configuration.inspect<string[]>('intellisionPath')?.workspaceValue ?? [];
		const rootA = path.join(getWorkspaceRoot(), 'test_files', 'runtime_include_root');
		const rootB = path.join(getWorkspaceRoot(), 'test_files', 'runtime_include_root_alt');
		const document = await openFixture('config_runtime_suite.nsf');
		const symbolPosition = positionOf(document, 'RuntimeOnlySharedColor', 1, 3);
		await waitForClientReady('client ready before intellisionPath switch');

		try {
			await configuration.update('intellisionPath', [rootA], vscode.ConfigurationTarget.Workspace);
			await waitForIndexingIdle('indexing idle for runtime include root A');
			const definitionsForRootA = await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						document.uri,
						symbolPosition
					),
				(value) =>
					Array.isArray(value) &&
					value.some((location) => path.basename(toFsPath(location)) === 'runtime_only_shared.ush'),
				'definition results for runtime include root A'
			);
			assert.ok(
				definitionsForRootA.some(
					(location) => path.basename(toFsPath(location)) === 'runtime_only_shared.ush'
				)
			);

			await configuration.update('intellisionPath', [rootB], vscode.ConfigurationTarget.Workspace);
			await waitForIndexingIdle('indexing idle for runtime include root B');
			const definitionsForRootB = await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						document.uri,
						symbolPosition
					),
				(value) =>
					Array.isArray(value) &&
					value.some((location) => path.basename(toFsPath(location)) === 'runtime_only_shared_alt.ush'),
				'definition results for runtime include root B'
			);

			const resolvedFiles = definitionsForRootB.map((location) => path.basename(toFsPath(location)));
			assert.ok(resolvedFiles.includes('runtime_only_shared_alt.ush'), resolvedFiles.join(','));
			assert.ok(
				!resolvedFiles.includes('runtime_only_shared.ush'),
				`Expected intellisionPath switch to invalidate stale current-doc context. Actual=${resolvedFiles.join(',')}`
			);
		} finally {
			await configuration.update('intellisionPath', originalRoots, vscode.ConfigurationTarget.Workspace);
			await waitForIndexingIdle('indexing idle after runtime include root restore');
			await openFixture('module_suite.nsf');
		}
	});

	it('shows hover documentation for discard keyword resources', async () => {
		const document = await openFixture('module_keyword_discard.nsf');

		const hoverPosition = positionOf(document, 'discard', 1, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for discard keyword'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('(HLSL keyword)'));
		assert.ok(hoverText.includes('Discards the current pixel (pixel shader).'));
	});

	it('includes trailing inline comments in hover documentation', async () => {
		const document = await openFixture('module_hover_inline_comment.nsf');

		const hoverPosition = positionOf(document, 'u_inline_comment_var', 2, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for u_inline_comment_var'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('Inline comment for hover'));
	});

	it('shows hover types for UI metadata declarations', async () => {
		const document = await openFixture('module_hover_ui_metadata_decl.nsf');

		const hoverPosition = positionOf(document, 'u_b_distance_length', 2, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for u_b_distance_length'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('Type: float'));
	});

	it('keeps local variable hover type in assignment statements', async () => {
		const document = await openFixture('module_hover_local_type_assignment.nsf');

		const hoverPosition = positionOf(document, 'main_color2', 2, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for local main_color2 assignment'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('(Local variable)'));
		assert.ok(hoverText.includes('Type: half3'));
		assert.ok(!hoverText.includes('Type: main_color2'));
	});

	it('keeps local variable hover type on the active preprocessor branch', async () => {
		const document = await openFixture('module_hover_local_preprocessor_branch.nsf');

		const hoverPosition = positionOf(document, 'value.active_only', 2, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for active preprocessor local variable'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('(Local variable)'));
		assert.ok(hoverText.includes('Type: HoverPreprocActive'));
		assert.ok(!hoverText.includes('Type: HoverPreprocInactive'));
	});

	it('shows full hover info for current-doc parameter usages', async () => {
		const document = await openFixture('module_hover_parameter_usage.nsf');

		const hoverPosition = positionOf(document, 'entry_fog', 2, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for current-doc parameter usage'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('(Parameter)'));
		assert.ok(hoverText.includes('Type: half4'));
		assert.ok(hoverText.includes('Defined at:'));
		assert.ok(!hoverText.includes('Type: entry_fog'));
	});

	it('shows full hover info for struct member access', async () => {
		const document = await openFixture('module_hover_struct_member_field.nsf');

		const hoverPosition = positionOf(document, 'mask2_clamp_x', 3, 4);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for struct member mask2_clamp_x'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('(Field) Owner: HoverBatch'));
		assert.ok(hoverText.includes('Type: float'));
	});

	it('includes struct member leading and inline comments in hover', async () => {
		const document = await openFixture('module_hover_struct_member_docs.nsf');

		const hoverPosition = positionOf(document, 'mask2_clamp_x', 2, 3);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for documented struct member mask2_clamp_x'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('(Field) Owner: HoverMemberDocs'));
		assert.ok(hoverText.includes('member doc from previous line'));
		assert.ok(hoverText.includes('member inline comment'));
	});

	it('shows struct hover members from active inline include fragments', async () => {
		await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
		const document = await openFixture('module_hover_struct_inline_include_fields.nsf');

		const hoverPosition = positionOf(document, 'HoverInlineInclude', 1, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for inline include struct members'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('struct HoverInlineInclude'));
		assert.ok(hoverText.includes('inline_scalar'));
		assert.ok(hoverText.includes('inline_color'));
		assert.ok(hoverText.includes('float inline_scalar'));
		assert.ok(hoverText.includes('float4 inline_color'));
		assert.ok(!hoverText.includes('inactive_inline_field'));
	});

	it('shows struct hover members from active inline include fragments in included struct files', async () => {
		await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
		const document = await openFixture('module_hover_struct_inline_include_external.ush');

		const hoverPosition = positionOf(document, 'HoverInlineIncludeExternal', 1, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for external inline include struct members'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('struct HoverInlineIncludeExternal'));
		assert.ok(hoverText.includes('external_inline_scalar'));
		assert.ok(hoverText.includes('external_inline_color'));
		assert.ok(hoverText.includes('float external_inline_scalar'));
		assert.ok(hoverText.includes('float3 external_inline_color'));
		assert.ok(!hoverText.includes('external_inactive_field'));
	});

	it('shows hover info on member-access base symbol', async () => {
		const document = await openFixture('module_hover_struct_member_field.nsf');

		const hoverPosition = positionOf(document, 'batch.mask2_clamp_x', 1, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for member-access base batch'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('(Member access base)'));
		assert.ok(hoverText.includes('Type: HoverBatch'));
		assert.ok(hoverText.includes('(Local variable)'));
	});

	it('resolves function definitions through includes for assignment calls', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for include-resolved function definition');
			const document = await openFixture('module_definition_function_call_in_assignment.nsf');

			const hoverPosition = positionOf(document, 'SampleColorTexture', 1, 2);
			const hovers = await waitForHoverText(
				document,
				hoverPosition,
				(text) => text.includes('SampleColorTexture doc line 1'),
				'hover results for SampleColorTexture'
			);
			const hoverText = hoverToText(hovers);
			assert.ok(hoverText.includes('SampleColorTexture doc line 1'));
			assert.ok(hoverText.includes('SampleColorTexture('));
			assert.ok(hoverText.includes('(HLSL function)'), hoverText);
			assert.ok(hoverText.includes('Returns:'));
			assert.ok(
				hoverText.includes('[include_texture_like.hlsl:3](file:///'),
				hoverText
			);
			assert.ok(
				hoverText.includes('include_texture_like.hlsl#L3'),
				hoverText
			);
			assert.ok(
				(
					hoverText.match(
						/SampleColorTexture\(Texture2D tex, sampler sam, float2 uv\)` — \[include_texture_like\.hlsl:3\]/g
					) ?? []
				).length <= 1,
				hoverText
			);
			assert.ok(!hoverText.includes('Overloads (2):'), hoverText);

			const definitions = await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						document.uri,
						hoverPosition
					),
				(value) => Array.isArray(value) && value.length > 0,
				'definition results for SampleColorTexture'
			);
			const defPath = toFsPath(definitions[0]);
			assert.ok(defPath.endsWith(path.join('test_files', 'include_texture_like.hlsl')));
		});
	});

	it('keeps hover and definition aligned for include-resolved function calls', async () => {
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for aligned include-resolved function call');
			const document = await openFixture('module_definition_function_call_in_assignment.nsf');
			const position = positionOf(document, 'SampleColorTexture', 1, 2);
			const definitions = await waitFor(
				() =>
					vscode.commands.executeCommand<ProviderLocation[]>(
						'vscode.executeDefinitionProvider',
						document.uri,
						position
					),
				(value) => Array.isArray(value) && value.length > 0,
				'aligned definition for include-resolved call'
			);
			const definitionFile = path.basename(toFsPath(definitions[0]));
			const hovers = await waitForHoverText(
				document,
				position,
				(text) => text.includes(definitionFile),
				'aligned hover for include-resolved call'
			);

			const hoverText = hoverToText(hovers);
			assert.ok(hoverText.includes(definitionFile));
		});
	});

	it('prefers the active preprocessor branch for hover and definition', async () => {
		const document = await openFixture('module_definition_preprocessor_else_branch.nsf');
		const position = positionOf(document, 'SelectBranchColor', 3, 2);

		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					position
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for active preprocessor branch'
		);
		const definitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					position
				),
			(value) => Array.isArray(value) && value.length > 0,
			'definition results for active preprocessor branch'
		);

		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('active branch doc'));
		assert.ok(!hoverText.includes('inactive branch doc'));
		assert.strictEqual(toFsPath(definitions[0]), document.uri.fsPath);
		assert.strictEqual(toRange(definitions[0]).start.line, 10);
	});

	it('shows full hover info for local multiline function call sites', async () => {
		const document = await openFixture('module_hover_local_function_complete.nsf');

		const hoverPosition = positionOf(document, 'GetExponentialHeightFog_Cloud', 1, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for GetExponentialHeightFog_Cloud'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('(HLSL function)'), hoverText);
		assert.ok(hoverText.includes('Returns: half4'));
		assert.ok(hoverText.includes('Parameters:'));
		assert.ok(hoverText.includes('float3 WorldCameraOrigin'));
	});

	});
}

