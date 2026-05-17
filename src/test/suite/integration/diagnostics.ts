import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	diagnosticCodeText,
	getDocumentRuntimeDebug,
	getWorkspaceRoot,
	hoverToText,
	openFixture,
	positionOf,
	repoDescribe,
	touchDocument,
	waitFor,
	waitForClientQuiescent,
	waitForDiagnostics,
	waitForDiagnosticsWithTouches,
	waitForIndexingIdle,
	withTemporaryIntellisionPath
} from '../test_helpers';

function diagnosticPublishKey(diagnostic: vscode.Diagnostic): string {
	return [
		diagnostic.range.start.line,
		diagnostic.range.start.character,
		diagnostic.range.end.line,
		diagnostic.range.end.character,
		diagnostic.message,
		diagnosticCodeText(diagnostic),
		diagnostic.source ?? ''
	].join('|');
}

function diagnosticMessages(diagnostics: readonly vscode.Diagnostic[]): string {
	return diagnostics.map((diag) => diag.message).join('\n');
}

function lineOf(document: vscode.TextDocument, needle: string): number {
	return positionOf(document, needle).line;
}

function hasDiagnosticOnLine(
	diagnostics: readonly vscode.Diagnostic[],
	line: number,
	message: string
): boolean {
	return diagnostics.some((diag) => diag.range.start.line === line && diag.message === message);
}

function diagnosticOnLine(
	diagnostics: readonly vscode.Diagnostic[],
	line: number,
	message: string
): vscode.Diagnostic | undefined {
	return diagnostics.find((diag) => diag.range.start.line === line && diag.message === message);
}

async function fetchPreprocessorMacroPresetForTests(): Promise<Record<string, string>> {
	await waitForClientQuiescent('client ready before preprocessor macro preset request');
	const response = await vscode.commands.executeCommand<any>('nsf._sendServerRequest', {
		method: 'nsf/getPreprocessorMacroPreset',
		params: {}
	});
	const macros: Record<string, string> = {};
	const entries = Array.isArray(response?.entries) ? response.entries : [];
	for (const entry of entries) {
		const name = typeof entry?.name === 'string' ? entry.name : '';
		const replacement = typeof entry?.replacement === 'string' ? entry.replacement : '';
		if (name.length > 0) {
			macros[name] = replacement;
		}
	}
	return macros;
}

export function registerDiagnosticsTests(): void {
	repoDescribe('NSF client integration: Diagnostics', () => {
	it('publishes diagnostics for missing includes and unterminated comments', async () => {
		const document = await openFixture('module_diagnostics.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 2,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Cannot resolve include'), 'Expected missing include diagnostic.');
		assert.ok(messages.includes('Unterminated block comment'), 'Expected unterminated block comment diagnostic.');
	});

	it('publishes diagnostics for invalid include syntax', async () => {
		const document = await openFixture('module_diagnostics_invalid_include_syntax.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const invalid = diagnostics.filter((diag) => diag.message.includes('Invalid #include syntax.'));
		assert.ok(invalid.length >= 3);
		for (const diag of invalid) {
			assert.strictEqual(diag.severity, vscode.DiagnosticSeverity.Warning);
		}
	});

	it('does not report diagnostics for nested preprocessor directives', async () => {
		const document = await openFixture('module_diagnostics_preprocessor_nesting.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Missing parentheses after if.'));
		assert.ok(!messages.includes('Unmatched preprocessor directive'));
		assert.ok(!messages.includes('Unterminated preprocessor conditional'));
		assert.ok(!messages.includes('Undefined macro in preprocessor expression'));
	});

	it('publishes diagnostics for unterminated preprocessor conditionals', async () => {
		const document = await openFixture('module_diagnostics_preprocessor_unterminated.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Unterminated preprocessor conditional: #if.'));
	});

	it('filters diagnostics based on #if evaluation for numeric defines', async () => {
		const offDoc = await openFixture('module_diagnostics_preprocessor_eval_off.nsf');
		const offDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(offDoc.uri),
			(value) => Array.isArray(value),
			'diagnostics for preprocessor eval off'
		);
		const offMessages = offDiagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!offMessages.includes('Assignment type mismatch: float2 = float4.'));

		const onDoc = await openFixture('module_diagnostics_preprocessor_eval_on.nsf');
		const onDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(onDoc.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics for preprocessor eval on'
		);
		const onMessages = onDiagnostics.map((diag) => diag.message).join('\n');
		assert.ok(onMessages.includes('Implicit truncation conversion: float4 -> float2. Use an explicit cast or swizzle if this is intentional.'));
	});

	it('expands object-like macros and arithmetic comparisons in #if expressions', async () => {
		const document = await openFixture('module_diagnostics_preprocessor_object_macro_expr.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Implicit truncation conversion: float4 -> float2. Use an explicit cast or swizzle if this is intentional.'));
		assert.ok(!messages.includes('Undefined macro in preprocessor expression'));
	});

	it('uses macros defined in active includes for #if diagnostics evaluation', async () => {
		const document = await openFixture('module_diagnostics_preprocessor_include_macro_eval.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined macro in preprocessor expression'));
		assert.ok(!messages.includes('Assignment type mismatch: float2 = float4.'));
	});

	it('uses active-unit prefix macros when a target include has a dot segment', async () => {
		const root = await openFixture('module_diagnostics_active_unit_dot_include_root.nsf');
		try {
			await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());
			await waitForClientQuiescent('active unit dot-include root settled');

			const document = await openFixture('module_diagnostics_active_unit_dot_include_target.hlsl');
			await waitForDiagnosticsWithTouches(
				document,
				(value) => {
					const messages = value.map((diag) => diag.message).join('\n');
					return (
						!messages.includes('Undefined macro in preprocessor expression: DOT_INCLUDE_MACRO.') &&
						!messages.includes('Assignment type mismatch: float2 = float4.')
					);
				},
				'active-unit dot-include preprocessor diagnostics'
			);
			await waitForClientQuiescent('active unit dot-include diagnostics settled');

			const settledMessages = vscode.languages
				.getDiagnostics(document.uri)
				.map((diag) => diag.message)
				.join('\n');
			assert.ok(!settledMessages.includes('Undefined macro in preprocessor expression: DOT_INCLUDE_MACRO.'));
			assert.ok(!settledMessages.includes('Assignment type mismatch: float2 = float4.'));
		} finally {
			await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
			await waitForClientQuiescent('active unit dot-include cleanup settled');
		}
	});

	it('uses workspace preprocessor macro presets for #if diagnostics evaluation', async () => {
		const preset = await fetchPreprocessorMacroPresetForTests();
		const compilerContextMacros = [
			'API_MOBILE_HIGH_QUALITY',
			'API_PC_HIGH_QUALITY',
			'API_SUPPORT_SSBO',
			'API_SUPPORT_SV_INSTANCE_ID',
			'API_SUPPORT_TEXFETCH',
			'API_SUPPORT_SV_VERTEX_ID',
			'API_SUPPORT_FRAGCOORD',
			'API_SUPPORT_TEXTURE_GATHER',
			'SYSTEM_SUPPORT_DEPTH_BUFFER_AS_TEXTURE',
			'GLES_USE_UBO',
			'SYSTEM_SUPPORT_SRGB'
		];
		assert.ok(Object.keys(preset).length >= 100, 'Expected a complete builtin preprocessor macro preset.');
		assert.strictEqual(preset.QUALITY_SUPPORT_MIDDLE, '(SHADER_QUALITY!=QUALITY_LOW)');
		assert.strictEqual(preset.PLAYERS_SELF, '0');
		assert.strictEqual(preset.NORMAL_MAP_SUPPORT, '(NORMAL_MAP_ENABLE&&QUALITY_SUPPORT_HIGH)');
		assert.strictEqual(preset.CLUSTERED_NONE, '0');
		for (const macro of compilerContextMacros) {
			assert.strictEqual(preset[macro], '0', `Expected compiler context macro ${macro} in preset.`);
		}
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedMacros = configuration.inspect<Record<string, unknown>>('preprocessorMacros');
		const originalMacros = inspectedMacros?.workspaceValue;
		try {
			await configuration.update('preprocessorMacros', preset, vscode.ConfigurationTarget.Workspace);
			await waitForClientQuiescent('workspace preprocessor macros setting settled');

			const document = await openFixture('module_diagnostics_preprocessor_builtin_macros.nsf');
			await waitForDiagnosticsWithTouches(
				document,
				(value) => {
					const messages = value.map((diag) => diag.message).join('\n');
					return (
						!messages.includes('Undefined macro in preprocessor expression: QUALITY_SUPPORT_MIDDLE.') &&
						!messages.includes('Undefined macro in preprocessor expression: PLAYERS_SELF.') &&
						compilerContextMacros.every(
							(macro) => !messages.includes(`Undefined macro in preprocessor expression: ${macro}.`)
						) &&
						!messages.includes('Assignment type mismatch: float2 = float4.')
					);
				},
				'workspace preprocessor macro diagnostics'
			);
			await waitForClientQuiescent('workspace preprocessor macro diagnostics settled');

			const settledMessages = vscode.languages
				.getDiagnostics(document.uri)
				.map((diag) => diag.message)
				.join('\n');
			assert.ok(!settledMessages.includes('Undefined macro in preprocessor expression: QUALITY_SUPPORT_MIDDLE.'));
			assert.ok(!settledMessages.includes('Undefined macro in preprocessor expression: PLAYERS_SELF.'));
			for (const macro of compilerContextMacros) {
				assert.ok(!settledMessages.includes(`Undefined macro in preprocessor expression: ${macro}.`));
			}
			assert.ok(!settledMessages.includes('Assignment type mismatch: float2 = float4.'));
		} finally {
			await configuration.update('preprocessorMacros', originalMacros, vscode.ConfigurationTarget.Workspace);
			await waitForClientQuiescent('workspace preprocessor macros cleanup settled');
		}
	});

	it('treats configured preprocessor macros as the complete effective preset table', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedMacros = configuration.inspect<Record<string, unknown>>('preprocessorMacros');
		const originalMacros = inspectedMacros?.workspaceValue;
		try {
			await configuration.update(
				'preprocessorMacros',
				{
					PRESET_BASE: '2',
					PRESET_EXPR: '(PRESET_BASE==2)'
				},
				vscode.ConfigurationTarget.Workspace
			);
			await waitForClientQuiescent('complete preprocessor macro table setting settled');

			const document = await openFixture('module_diagnostics_preprocessor_configured_macros.nsf');
			await waitForDiagnosticsWithTouches(
				document,
				(value) => {
					const messages = value.map((diag) => diag.message).join('\n');
					return (
						!messages.includes('Undefined macro in preprocessor expression: PRESET_EXPR.') &&
						!messages.includes('Assignment type mismatch: float2 = float4.')
					);
				},
				'complete preprocessor macro table diagnostics'
			);
			await waitForClientQuiescent('complete preprocessor macro table diagnostics settled');

			const settledMessages = vscode.languages
				.getDiagnostics(document.uri)
				.map((diag) => diag.message)
				.join('\n');
			assert.ok(!settledMessages.includes('Undefined macro in preprocessor expression: PRESET_EXPR.'));
			assert.ok(!settledMessages.includes('Assignment type mismatch: float2 = float4.'));
		} finally {
			await configuration.update('preprocessorMacros', originalMacros, vscode.ConfigurationTarget.Workspace);
			await waitForClientQuiescent('complete preprocessor macro table cleanup settled');
		}
	});

	it('does not report include diagnostics inside comments', async () => {
		const document = await openFixture('module_diagnostics_include_in_comments.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Invalid #include syntax.'));
		assert.ok(!messages.includes('Cannot resolve include'));
	});

	it('publishes diagnostics for unmatched brackets', async () => {
		const document = await openFixture('module_diagnostics_unmatched_brackets.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Unterminated bracket'));
		for (const diag of diagnostics) {
			if (diag.message.includes('Unterminated bracket') || diag.message.includes('Unmatched closing bracket')) {
				assert.strictEqual(diag.severity, vscode.DiagnosticSeverity.Error);
			}
		}
	});

	it('publishes diagnostics for missing return and invalid return usage', async () => {
		const document = await openFixture('module_diagnostics_return_errors.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 3,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Missing return statement.'));
		assert.ok(messages.includes('Return value in void function.'));
		assert.ok(messages.includes('Missing return value.'));
		assert.ok(messages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'));
	});

	it('publishes diagnostics for assignment and operator type mismatches', async () => {
		const assignDoc = await openFixture('module_diagnostics_type_mismatch_assign.nsf');
		const assignDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(assignDoc.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);
		const assignMessages = assignDiagnostics.map((diag) => diag.message).join('\n');
		assert.ok(assignMessages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'));

		const userReturnDoc = await openFixture('module_diagnostics_user_function_return_type.nsf');
		const userReturnDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(userReturnDoc.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);
		const userReturnMessages = userReturnDiagnostics.map((diag) => diag.message).join('\n');
		assert.ok(userReturnMessages.includes('Assignment type mismatch: float4 = float3.'));

		const opDoc = await openFixture('module_diagnostics_type_mismatch_op.nsf');
		const opDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(opDoc.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);
		const opMessages = opDiagnostics.map((diag) => diag.message).join('\n');
		assert.ok(opMessages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'));
	});

	it('models official HLSL implicit conversions with risk warnings', async () => {
		const document = await openFixture('module_diagnostics_type_relation_official_conversions.nsf');

		const diagnostics = await waitForDiagnostics(
			document,
			(value) => {
				const messages = diagnosticMessages(value);
				return (
					messages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.') &&
					messages.includes('Implicit narrowing conversion: float -> half. Use an explicit cast if this is intentional.') &&
					messages.includes('Implicit floating-integral conversion: int -> half. Use an explicit cast if this is intentional.') &&
					messages.includes('Implicit floating-integral conversion: float -> int. Use an explicit cast if this is intentional.') &&
					messages.includes('Implicit signedness conversion: int -> uint. Use an explicit cast if this is intentional.') &&
					messages.includes('Implicit boolean conversion: int -> bool. Use an explicit cast if this is intentional.') &&
					messages.includes('Implicit boolean conversion: bool -> int. Use an explicit cast if this is intentional.') &&
					messages.includes('Assignment type mismatch: float3 = float2.')
				);
			},
			'official conversion diagnostics'
		);

		const messages = diagnosticMessages(diagnostics);
		assert.ok(!messages.includes('Assignment type mismatch: float3 = float4.'));
		assert.ok(!messages.includes('Assignment type mismatch: half = float.'));
		assert.ok(!messages.includes('Assignment type mismatch: half3 = float3.'));
		assert.ok(!messages.includes('Function call argument mismatch: P2AcceptFloat3.'));
		assert.ok(!messages.includes('Function call argument mismatch: P2AcceptHalf3.'));
		assert.ok(!hasDiagnosticOnLine(
			diagnostics,
			lineOf(document, 'float3 truncCast = (float3)wide;'),
			'Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'
		));
		assert.ok(!hasDiagnosticOnLine(
			diagnostics,
			lineOf(document, 'float3 truncCtor = float3(wide);'),
			'Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'
		));
		assert.ok(!hasDiagnosticOnLine(
			diagnostics,
			lineOf(document, 'float3 truncSwizzle = wide.xyz;'),
			'Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'
		));
		assert.ok(!hasDiagnosticOnLine(
			diagnostics,
			lineOf(document, 'half literalHalf = 1.0;'),
			'Implicit narrowing conversion: float -> half. Use an explicit cast if this is intentional.'
		));
		assert.ok(!hasDiagnosticOnLine(
			diagnostics,
			lineOf(document, 'int safeSignedLiteral = 1u;'),
			'Implicit signedness conversion: uint -> int. Use an explicit cast if this is intentional.'
		));
	});

	it('keeps semantic diagnostics across comment-only edits before replacement full diagnostics is ready', async () => {
		let document = await openFixture('module_diagnostics_type_mismatch_assign.nsf');
		const mismatchText = 'Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.';
		const restoreText = '    // continuity comment\n';
		let sawTransientDrop = false;
		const diagnosticsSubscription = vscode.languages.onDidChangeDiagnostics((event) => {
			if (!event.uris.some((uri) => uri.toString() === document.uri.toString())) {
				return;
			}
			const currentDiagnostics = vscode.languages.getDiagnostics(document.uri);
			if (!currentDiagnostics.some((diag) => diag.message.includes(mismatchText))) {
				sawTransientDrop = true;
			}
		});
		try {
			const initialDiagnostics = await waitForDiagnostics(
				document,
				(value) => value.some((diag) => diag.message.includes(mismatchText)),
				'initial semantic diagnostics before comment-only edit'
			);
			assert.ok(initialDiagnostics.some((diag) => diag.message.includes(mismatchText)));

			const edit = new vscode.WorkspaceEdit();
			edit.insert(document.uri, new vscode.Position(2, 0), restoreText);
			assert.ok(await vscode.workspace.applyEdit(edit), 'Expected comment-only edit to apply.');
			document = await vscode.workspace.openTextDocument(document.uri);

			const afterEditDiagnostics = await waitForDiagnostics(
				document,
				(value) => value.some((diag) => diag.message.includes(mismatchText)),
				'semantic diagnostics preserved after comment-only edit'
			);
			assert.ok(afterEditDiagnostics.some((diag) => diag.message.includes(mismatchText)));
			await waitForClientQuiescent('client quiescent after comment-only diagnostics continuity edit');
			assert.ok(!sawTransientDrop, 'Expected semantic diagnostics to remain visible throughout comment-only edit processing.');
		} finally {
			diagnosticsSubscription.dispose();
			document = await vscode.workspace.openTextDocument(document.uri);
			if (document.getText().includes(restoreText)) {
				const restoreStart = new vscode.Position(2, 0);
				const restoreEnd = new vscode.Position(3, 0);
				const restoreEdit = new vscode.WorkspaceEdit();
				restoreEdit.delete(document.uri, new vscode.Range(restoreStart, restoreEnd));
				assert.ok(await vscode.workspace.applyEdit(restoreEdit), 'Expected continuity comment cleanup to apply.');
			}
		}
	});

	it('keeps same-line semantic diagnostics across whitespace-only edits when full rebuild is skipped', async () => {
		let document = await openFixture('module_diagnostics_type_mismatch_assign.nsf');
		const mismatchText = 'Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.';
		const whitespaceInsert = '  ';
		let sawTransientDrop = false;
		const diagnosticsSubscription = vscode.languages.onDidChangeDiagnostics((event) => {
			if (!event.uris.some((uri) => uri.toString() === document.uri.toString())) {
				return;
			}
			const currentDiagnostics = vscode.languages.getDiagnostics(document.uri);
			if (!currentDiagnostics.some((diag) => diag.message.includes(mismatchText))) {
				sawTransientDrop = true;
			}
		});
		try {
			const initialDiagnostics = await waitForDiagnostics(
				document,
				(value) => value.some((diag) => diag.message.includes(mismatchText)),
				'initial semantic diagnostics before same-line whitespace edit'
			);
			assert.ok(initialDiagnostics.some((diag) => diag.message.includes(mismatchText)));

			const insertPosition = positionOf(
				document,
				'a = float4(uv, 0.0, 1.0);',
				1,
				'a = float4(uv, 0.0, 1.0);'.length
			);
			const edit = new vscode.WorkspaceEdit();
			edit.insert(document.uri, insertPosition, whitespaceInsert);
			assert.ok(await vscode.workspace.applyEdit(edit), 'Expected same-line whitespace edit to apply.');
			document = await vscode.workspace.openTextDocument(document.uri);

			await waitForClientQuiescent('client quiescent after same-line whitespace diagnostics edit');
			const settledDiagnostics = vscode.languages.getDiagnostics(document.uri);
			assert.ok(
				settledDiagnostics.some((diag) => diag.message.includes(mismatchText)),
				'Expected same-line semantic diagnostics to remain visible after whitespace-only edit.'
			);
			assert.ok(!sawTransientDrop, 'Expected same-line semantic diagnostics to remain visible throughout whitespace-only edit processing.');
		} finally {
			diagnosticsSubscription.dispose();
			document = await vscode.workspace.openTextDocument(document.uri);
			const mismatchLine = document.lineAt(2).text;
			if (mismatchLine.endsWith(whitespaceInsert)) {
				const deleteStart = new vscode.Position(2, mismatchLine.length - whitespaceInsert.length);
				const deleteEnd = new vscode.Position(2, mismatchLine.length);
				const restoreEdit = new vscode.WorkspaceEdit();
				restoreEdit.delete(document.uri, new vscode.Range(deleteStart, deleteEnd));
				assert.ok(await vscode.workspace.applyEdit(restoreEdit), 'Expected same-line whitespace cleanup to apply.');
			}
		}
	});

	it('does not misinfer cast initialization types', async () => {
		const document = await openFixture('module_diagnostics_cast_initialization.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Assignment type mismatch: PS_INPUT = ps_main.'));
	});

	it('does not treat parenthesized identifiers as cast types', async () => {
		const document = await openFixture('module_diagnostics_parenthesized_identifier_expr.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Assignment type mismatch: float3 = InscatteringColor.'));
	});

	it('infers struct member swizzles for assignment type checking', async () => {
		const okDoc = await openFixture('module_diagnostics_member_access_swizzle_ok.nsf');
		const okDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(okDoc.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);
		const okMessages = okDiagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!okMessages.includes('Assignment type mismatch'));

		const mismatchDoc = await openFixture('module_diagnostics_member_access_swizzle_mismatch.nsf');
		const mismatchDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(mismatchDoc.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);
		const mismatchMessages = mismatchDiagnostics.map((diag) => diag.message).join('\n');
		assert.ok(mismatchMessages.includes('Assignment type mismatch: float3 = float2.'));
	});

	it('infers cbuffer member types for assignment type checking', async () => {
		const okDoc = await openFixture('module_diagnostics_global_symbol_type_ok.nsf');
		const okDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(okDoc.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);
		const okMessages = okDiagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!okMessages.includes('Assignment type mismatch'));

		const mismatchDoc = await openFixture('module_diagnostics_global_symbol_type_mismatch.nsf');
		const mismatchDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(mismatchDoc.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);
		const mismatchMessages = mismatchDiagnostics.map((diag) => diag.message).join('\n');
		assert.ok(mismatchMessages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'));
	});

	it('resolves cbuffer members from other files through workspace scan', async () => {
		await vscode.commands.executeCommand('workbench.action.closeAllEditors');
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for external cbuffer diagnostics');
			await openFixture('module_diagnostics_external_cbuffer_provider.nsf');
			const consumerDoc = await openFixture('module_diagnostics_external_cbuffer_consumer.nsf');
			await touchDocument(consumerDoc);
			const diagnostics = await waitForDiagnosticsWithTouches(
				consumerDoc,
				(value) => {
					const messages = value.map((diag) => diag.message).join('\n');
					return (
						messages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.') &&
						!messages.includes('Undefined identifier: u_wind_tex_factors.')
					);
				},
				'external cbuffer diagnostics'
			);
			const messages = diagnostics.map((diag) => diag.message).join('\n');
			assert.ok(!messages.includes('Undefined identifier: u_wind_tex_factors.'));
			assert.ok(messages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'));
		});
	});

	it('keeps last-good global-context diagnostics while macro context for the new edit is not yet ready', async function () {
		this.timeout(120000);
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			const rootPath = path.join(getWorkspaceRoot(), 'test_files', 'layered_runtime_macro_root.nsf');
			const rootUri = vscode.Uri.file(rootPath);
			const transientWrongPublishes: string[] = [];
			const observerTasks: Promise<void>[] = [];
			const diagnosticsSubscription = vscode.languages.onDidChangeDiagnostics((event) => {
				if (!event.uris.some((uri) => uri.toString() === rootUri.toString())) {
					return;
				}
				const currentDiagnostics = vscode.languages.getDiagnostics(rootUri);
				const messages = currentDiagnostics.map((diag) => diag.message).join('\n');
				const sawWrongPublish =
					messages.includes('Undefined macro in preprocessor expression: LAYERED_RUNTIME_SHARED_BRANCH.') ||
					messages.includes('Undefined identifier: layeredRuntimeSharedColor.');
				if (!sawWrongPublish) {
					return;
				}
				observerTasks.push(
					getDocumentRuntimeDebug([rootUri.toString()]).then(([runtime]) => {
						if (runtime?.globalContextReady === true) {
							return;
						}
						transientWrongPublishes.push(
							`${runtime?.globalContextReady ?? 'unknown'}:${messages}`
						);
					})
				);
			});
			try {
				const root = await openFixture('layered_runtime_macro_root.nsf');
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', root.uri.toString());

				const diagnostics = await waitForDiagnosticsWithTouches(
					root,
					(value) => {
						const messages = value.map((diag) => diag.message).join('\n');
						return (
							!messages.includes('Undefined macro in preprocessor expression: LAYERED_RUNTIME_SHARED_BRANCH.') &&
							!messages.includes('Undefined identifier: layeredRuntimeSharedColor.')
						);
					},
					'macro-sensitive diagnostics after global context readiness'
				);

				const messages = diagnostics.map((diag) => diag.message).join('\n');
				assert.ok(!messages.includes('Undefined macro in preprocessor expression: LAYERED_RUNTIME_SHARED_BRANCH.'));
				assert.ok(!messages.includes('Undefined identifier: layeredRuntimeSharedColor.'));
				await waitForClientQuiescent('macro-sensitive diagnostics continuity settled');
				await Promise.all(observerTasks);
				assert.deepStrictEqual(
					transientWrongPublishes,
					[],
					`Expected no transient macro-sensitive wrong publish before global context readiness, got ${transientWrongPublishes.join('\n')}`
				);
			} finally {
				diagnosticsSubscription.dispose();
				await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
			}
		});
	});

	it('supports float4x4 and matrix mul type inference', async () => {
		const document = await openFixture('module_diagnostics_matrix_mul.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: float4x4.'));
		assert.ok(!messages.includes('Binary operator type mismatch'));
		assert.ok(messages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'));
	});

	it('supports matrix index type inference in arithmetic expressions', async () => {
		const document = await openFixture('module_diagnostics_matrix_index.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Binary operator type mismatch'));
		assert.ok(messages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'));
	});

	it('publishes diagnostics for undefined identifiers', async () => {
		const document = await openFixture('module_diagnostics_undefined_identifier.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 2,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Undefined identifier: UnknownVar.'));
		assert.ok(messages.includes('Undefined identifier: UnknownVar2.'));
	});

	it('does not mark second variable undefined in single-line multi declarations', async () => {
		const document = await openFixture('module_diagnostics_single_line_multi_decl.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: ScreenPosition.'));
		assert.ok(!messages.includes('Undefined identifier: NDCPosition.'));
	});

	it('does not treat member assignments or return statements as declarations', async () => {
		const document = await openFixture('module_diagnostics_non_decl_statements.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Duplicate local declaration: world_normal.'));
		assert.ok(!messages.includes('Duplicate local declaration: world_binormal.'));
		assert.ok(!messages.includes('Duplicate local declaration: p.'));
	});

	it('does not mark multiline function parameters as undefined', async () => {
		const document = await openFixture('module_diagnostics_multiline_params.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: WorldPositionRelativeToCamera.'));
		assert.ok(!messages.includes('Undefined identifier: ExcludeDistance.'));
	});

	it('treats hex literals as numeric values', async () => {
		const document = await openFixture('module_diagnostics_hex_literal.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: 0xFF.'));
		assert.ok(!messages.includes('Undefined identifier: 0xFFu.'));
		assert.ok(!messages.includes('Undefined identifier: 0x1p.'));
		assert.ok(messages.includes('Invalid numeric literal suffix: p.'));
	});

	it('accepts supported numeric literal forms without suffix diagnostics', async () => {
		const document = await openFixture('module_diagnostics_numeric_literal_exponent.nsf');
		const noSuffixAfterOperatorLine = lineOf(document, 'half noSuffixAfterOperator = 1.0f-HALF_MIN;');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) =>
				Array.isArray(value) &&
				hasDiagnosticOnLine(
					value,
					noSuffixAfterOperatorLine,
					'Undefined identifier: HALF_MIN.'
				),
			'numeric literal positive diagnostics'
		);

		const messages = diagnosticMessages(diagnostics);
		assert.ok(
			!messages.includes('Invalid numeric literal suffix:'),
			`Expected all supported numeric literal forms to avoid suffix diagnostics. Actual:\n${messages}`
		);
		assert.ok(!messages.includes('Undefined identifier: 1e.'));
		assert.ok(!messages.includes('Undefined identifier: 1E.'));
		assert.ok(!messages.includes('Undefined identifier: 0xFF.'));
		assert.ok(!messages.includes('Undefined identifier: 0xFFu.'));
		assert.ok(!messages.includes('Undefined identifier: ..'));
		for (const lineText of [
			'half narrowingLeadingDot = .5;',
			'half narrowingTrailingDot = 1.;',
			'half narrowingExponent = 1e-5;',
			'half narrowingDoubleSuffix = 1.0L;'
		]) {
			assert.ok(
				!hasDiagnosticOnLine(diagnostics, lineOf(document, lineText), 'Implicit narrowing conversion: float -> half. Use an explicit cast if this is intentional.') &&
					!hasDiagnosticOnLine(diagnostics, lineOf(document, lineText), 'Implicit narrowing conversion: double -> half. Use an explicit cast if this is intentional.'),
				`Expected ${lineText} to be accepted as literal adaptation without narrowing warning. Actual diagnostics:\n${messages}`
			);
		}
		assert.ok(
			hasDiagnosticOnLine(
				diagnostics,
				noSuffixAfterOperatorLine,
				'Undefined identifier: HALF_MIN.'
			),
			`Expected '-' after a suffixed literal to remain an operator, not a suffix. Actual diagnostics:\n${messages}`
		);
		assert.ok(
			!hasDiagnosticOnLine(
				diagnostics,
				noSuffixAfterOperatorLine,
				'Invalid numeric literal suffix: -.'
			),
			`Expected '-' after a suffixed literal to remain an operator, not a suffix. Actual diagnostics:\n${messages}`
		);

		const warnings: Array<{ lineText: string; message: string }> = [
			{ lineText: 'int legacyLongLongLower = 42ll;', message: 'Deprecated numeric literal suffix: ll. Use l.' },
			{ lineText: 'uint legacyUnsignedLongLongLower = 42ull;', message: 'Deprecated numeric literal suffix: ull. Use ul.' },
			{ lineText: 'uint legacyLongLongUnsignedUpper = 42LLU;', message: 'Deprecated numeric literal suffix: LLU. Use ul.' },
			{ lineText: 'int legacyHexLongLongLower = 0xFFll;', message: 'Deprecated numeric literal suffix: ll. Use l.' },
			{ lineText: 'uint legacyHexUnsignedLongLongLower = 0xFFull;', message: 'Deprecated numeric literal suffix: ull. Use ul.' },
			{ lineText: 'uint legacyHexLongLongUnsignedUpper = 0xFFLLU;', message: 'Deprecated numeric literal suffix: LLU. Use ul.' }
		];
		for (const item of warnings) {
			const diagnostic = diagnosticOnLine(diagnostics, lineOf(document, item.lineText), item.message);
			assert.ok(diagnostic, `Expected ${item.message} on line "${item.lineText}". Actual diagnostics:\n${messages}`);
			assert.strictEqual(diagnostic.severity, vscode.DiagnosticSeverity.Warning);
		}
	});

	it('keeps invalid numeric suffix diagnostics for malformed literals', async () => {
		const document = await openFixture('module_diagnostics_numeric_literal_invalid_suffix.nsf');

		const diagnostics = await waitForDiagnostics(
			document,
			(value) => value.filter((diag) => diag.message.startsWith('Invalid numeric literal suffix:')).length >= 10,
			'numeric literal invalid suffix diagnostics'
		);

		const expected: Array<{ lineText: string; message: string }> = [
			{ lineText: 'float invalidDecimal = 1q;', message: 'Invalid numeric literal suffix: q.' },
			{ lineText: 'float invalidFraction = 1.0q;', message: 'Invalid numeric literal suffix: q.' },
			{ lineText: 'float invalidExponent = 1e-5q;', message: 'Invalid numeric literal suffix: q.' },
			{ lineText: 'float invalidFractionExponent = 1.0e+5q;', message: 'Invalid numeric literal suffix: q.' },
			{ lineText: 'float invalidFloatSuffixTail = 1.0fQ;', message: 'Invalid numeric literal suffix: Q.' },
			{ lineText: 'float invalidHalfSuffixTail = 1.0hQ;', message: 'Invalid numeric literal suffix: Q.' },
			{ lineText: 'float invalidFloatUnsignedSuffix = 1.0u;', message: 'Invalid numeric literal suffix: u.' },
			{ lineText: 'float invalidIntegerRepeatedUnsignedSuffix = 42ulu;', message: 'Invalid numeric literal suffix: u.' },
			{ lineText: 'float invalidOctalDigitEight = 08;', message: 'Invalid numeric literal suffix: 8.' },
			{ lineText: 'float invalidOctalDigitNine = 09u;', message: 'Invalid numeric literal suffix: 9.' },
			{ lineText: 'float invalidHex = 0x1p;', message: 'Invalid numeric literal suffix: p.' },
			{ lineText: 'float invalidHexFloatStyle = 0x1p2;', message: 'Invalid numeric literal suffix: p.' },
			{ lineText: 'float invalidHexRepeatedUnsignedSuffix = 0x1ulu;', message: 'Invalid numeric literal suffix: u.' }
		];

		const messages = diagnosticMessages(diagnostics);
		for (const item of expected) {
			const diagnostic = diagnosticOnLine(diagnostics, lineOf(document, item.lineText), item.message);
			assert.ok(diagnostic, `Expected ${item.message} on line "${item.lineText}". Actual diagnostics:\n${messages}`);
			assert.strictEqual(diagnostic.severity, vscode.DiagnosticSeverity.Error);
		}
	});

	it('does not flag builtin function calls as undefined identifiers', async () => {
		const document = await openFixture('module_diagnostics_hlsl_builtin_length.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: length.'));
	});

	it('does not flag discard as an undefined identifier', async () => {
		const document = await openFixture('module_keyword_discard.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: discard.'));
	});

	it('does not mark include-resolvable types and globals as undefined', async () => {
		const document = await openFixture('module_diagnostics_include_graph_consumer.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: PixelMaterialInputs.'));
		assert.ok(!messages.includes('Undefined identifier: u_frame_time_radian.'));
		assert.ok(!messages.includes('Undefined identifier: u_include_metadata_scale.'));
		assert.ok(!messages.includes('Undefined identifier: t_distort1.'));
		assert.ok(!messages.includes('Undefined identifier: s_distort1.'));
	});

	it('infers texture Sample() call return types', async () => {
		await vscode.commands.executeCommand('workbench.action.closeAllEditors');
		await withTemporaryIntellisionPath([path.join(getWorkspaceRoot(), 'test_files')], async () => {
			await waitForIndexingIdle('indexing idle for texture sample diagnostics');
			const document = await openFixture('module_diagnostics_texture_sample_consumer.nsf');
			await touchDocument(document);
			const diagnostics = await waitForDiagnosticsWithTouches(
				document,
				(value) => {
					const messages = value.map((diag) => diag.message).join('\n');
					return (
						messages.includes('Implicit narrowing conversion: float4 -> half4. Use an explicit cast if this is intentional.') &&
						messages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.') &&
						!messages.includes('Undefined identifier: t_fresnel_normalMap.') &&
						!messages.includes('Undefined identifier: s_fresnel_normalmap.')
					);
				},
				'texture sample diagnostics'
			);
			const messages = diagnostics.map((diag) => diag.message).join('\n');
			assert.ok(!messages.includes('Undefined identifier: t_fresnel_normalMap.'));
			assert.ok(!messages.includes('Undefined identifier: s_fresnel_normalmap.'));
			assert.ok(messages.includes('Implicit narrowing conversion: float4 -> half4. Use an explicit cast if this is intentional.'));
			assert.ok(messages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'));
		});
	});

	it('uses Texture2DArray coord dimensions for built-in Sample and Load diagnostics', async () => {
		const document = await openFixture('module_texture2d_array_methods.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'texture2darray diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Built-in method call type mismatch: Sample.'));
		assert.ok(!messages.includes('Built-in method call type mismatch: Load.'));
	});

	it('does not report diagnostics for complex member-call base expressions', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: gTex.'));
		assert.ok(!messages.includes('Undefined identifier: gTexArr.'));
		assert.ok(!messages.includes('Undefined identifier: GET_TEX.'));
		assert.ok(!messages.includes('Undefined identifier: WRAP_TEX.'));
		assert.ok(!messages.includes('Built-in method call type mismatch: Sample.'));
	});

	it('publishes diagnostics for duplicate and shadowed declarations', async () => {
		const document = await openFixture('module_diagnostics_shadowing_duplicate.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 3,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Duplicate global declaration: GlobalDup.'));
		assert.ok(messages.includes('Local shadows parameter: a.'));
		assert.ok(messages.includes('Duplicate local declaration: b.'));
	});

	it('uses lexical scopes for loop locals, sibling blocks, and block flow diagnostics', async () => {
		const document = await openFixture('module_diagnostics_local_scope_control_flow.nsf');

		const diagnostics = await waitForDiagnostics(
			document,
			(value) => {
				const messages = diagnosticMessages(value);
				return (
					messages.includes('Undefined identifier: i.') &&
					messages.includes('Unreachable code.') &&
					!messages.includes('Duplicate local declaration: scoped.') &&
					!messages.includes('Potential missing return on some paths.') &&
					!messages.includes('Missing return statement.')
				);
			},
			'lexical scope diagnostics'
		);

		const messages = diagnosticMessages(diagnostics);
		const undefinedLoopDiagnostics = diagnostics.filter((diag) => diag.message === 'Undefined identifier: i.');
		assert.strictEqual(undefinedLoopDiagnostics.length, 1, messages);
		assert.ok(!hasDiagnosticOnLine(diagnostics, lineOf(document, 'total = total + i;'), 'Undefined identifier: i.'));
		assert.ok(hasDiagnosticOnLine(diagnostics, lineOf(document, 'total = total + i;\n    return total;'), 'Undefined identifier: i.'));
		assert.ok(!messages.includes('Duplicate local declaration: scoped.'));
		assert.ok(!messages.includes('Potential missing return on some paths.'));
		assert.ok(!messages.includes('Missing return statement.'));
		assert.ok(messages.includes('Unreachable code.'));
	});

	it('treats undefined #if macros as errors and keeps only the #else branch active', async () => {
		const document = await openFixture('module_diagnostics_preprocessor_branch_scope.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Undefined macro in preprocessor expression: SOME_UNDEFINED_MACRO.'));
		assert.ok(!messages.includes('Duplicate local declaration: b.'));
		assert.ok(!messages.includes('Duplicate local declaration: x.'));
		const undefinedMacroDiag = diagnostics.find((diag) =>
			diag.message.includes('Undefined macro in preprocessor expression: SOME_UNDEFINED_MACRO.')
		);
		assert.ok(undefinedMacroDiag, 'Expected undefined macro diagnostic.');
		assert.strictEqual(undefinedMacroDiag!.severity, vscode.DiagnosticSeverity.Error);
	});

	it('publishes diagnostics for narrowing assignment into half', async () => {
		const document = await openFixture('module_diagnostics_half_narrowing.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Implicit narrowing conversion: float3 -> half3. Use an explicit cast if this is intentional.'));
		const publishKeys = new Set(diagnostics.map(diagnosticPublishKey));
		assert.strictEqual(publishKeys.size, diagnostics.length, 'Expected diagnostics publish payload to be deduped.');
		for (const diag of diagnostics) {
			if (
				diag.message.includes('Implicit narrowing conversion: float -> half.') ||
				diag.message.includes('Implicit narrowing conversion: float3 -> half3.')
			) {
				assert.strictEqual(diag.severity, vscode.DiagnosticSeverity.Warning);
			}
		}
	});

	it('keeps hover return type consistent with diagnostics assignment mismatch source type', async () => {
		const document = await openFixture('module_diagnostics_user_function_return_type.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics for module_diagnostics_user_function_return_type'
		);
		const mismatch = diagnostics.find((diag) => diag.message.includes('Assignment type mismatch: float4 = float3.'));
		assert.ok(mismatch, 'Expected assignment mismatch float4=float3.');

		const hoverPosition = positionOf(document, 'UserReturnTypeValue', 2, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover for assignment source function call'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('Returns: float3'), hoverText);
	});

	it('publishes narrowing warnings for pow and normalize into half-family targets', async () => {
		const powDocument = await openFixture('narrowing_pow_half.nsf');
		const powDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(powDocument.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics for narrowing_pow_half'
		);
		const powMismatch = powDiagnostics.find((diag) => diag.message.includes('Implicit narrowing conversion: float -> half.'));
		assert.ok(powMismatch, 'Expected half=float narrowing warning for pow.');
		assert.strictEqual(powMismatch!.severity, vscode.DiagnosticSeverity.Warning);

		const normalizeDocument = await openFixture('narrowing_normalize_half3.nsf');
		const normalizeDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(normalizeDocument.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics for narrowing_normalize_half3'
		);
		const normalizeMismatch = normalizeDiagnostics.find((diag) =>
			diag.message.includes('Implicit narrowing conversion: float3 -> half3.')
		);
		assert.ok(normalizeMismatch, 'Expected half3=float3 narrowing warning for normalize.');
		assert.strictEqual(normalizeMismatch!.severity, vscode.DiagnosticSeverity.Warning);
	});

	it('publishes indeterminate diagnostics instead of false narrowing on unknown half call', async () => {
		const document = await openFixture('narrowing_unknown_call_half.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics for narrowing_unknown_call_half'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Assignment type mismatch: half = float.'));
		assert.ok(messages.includes('Indeterminate assignment type: rhs type unavailable.'));
		const indeterminateCodes = diagnostics
			.map((diag) => diagnosticCodeText(diag))
			.filter((code) => code.startsWith('NSF_INDET_'));
		assert.ok(indeterminateCodes.length > 0, 'Expected indeterminate diagnostics with NSF_INDET_* code.');
	});

	it('publishes diagnostics for builtin overload argument mismatches', async () => {
		const document = await openFixture('module_diagnostics_hlsl_builtin_overload_args.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 2,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Implicit truncation conversion: float3 -> float2. Use an explicit cast or swizzle if this is intentional.'));
		assert.ok(messages.includes('Implicit signedness conversion: uint -> int. Use an explicit cast if this is intentional.'));
	});

	it('publishes diagnostics for user function call argument mismatches', async () => {
		const document = await openFixture('module_diagnostics_user_function_call_args.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Function call argument mismatch: SuiteUserFunc.'));
	});

	it('resolves macro-generated function signatures without fallback pseudo params', async () => {
		const document = await openFixture('module_diagnostics_macro_generated_function.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Function call argument mismatch: GetSkyLightColorMultiplier.'));
		assert.ok(messages.includes('Expected: (PixelData).'));
		assert.ok(messages.includes('Got: (float3).'));
		assert.ok(!messages.includes('Expected: (p).'));
	});

	it('displays inferred argument types instead of nearby identifiers in user function conversion diagnostics', async () => {
		const document = await openFixture('module_diagnostics_user_function_call_arg_display_regression.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(
			messages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'),
			`Expected user function argument conversion warning for Interaction. Actual diagnostics:\n${messages}`
		);
		assert.ok(!messages.includes('Function call argument mismatch: Interaction.'));
		assert.ok(!messages.includes('world_position'));
	});

	it('treats texture and sampler aliases as compatible in function calls', async () => {
		const document = await openFixture('module_diagnostics_texture_sampler_compat.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Function call argument mismatch: SampleColorTextureBias.'));
		assert.ok(!messages.includes('Function call argument count mismatch: SampleColorTextureBias.'));
	});

	it('does not report narrowing for builtin scalar results with half inputs', async () => {
		const document = await openFixture('module_diagnostics_hlsl_builtin_dot_half_ok.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Assignment type mismatch: half = float.'));
		assert.ok(!messages.includes('Builtin call type mismatch: dot.'));
		assert.ok(!messages.includes('Builtin call type mismatch: length.'));
		assert.ok(!messages.includes('Builtin call type mismatch: distance.'));
	});

	it('does not mark defined macro names as undefined identifiers', async () => {
		const document = await openFixture('module_diagnostics_macro_define_identifier_ok.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: TRANSPARENT_STAGE.'));
	});

	it('publishes diagnostics for missing semicolons', async () => {
		const document = await openFixture('module_diagnostics_missing_semicolon.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 2,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Missing semicolon.'));
	});

	it('does not misreport missing semicolons on technique and pass headers', async () => {
		const document = await openFixture('module_pass_stencil_states.nsf');

		await waitForDiagnostics(
			document,
			(value) => !value.some((diag) => diag.message.includes('Missing semicolon.')),
			'technique/pass header diagnostics'
		);

		await waitForClientQuiescent('technique/pass diagnostics settled');
		const settledDiagnostics = vscode.languages.getDiagnostics(document.uri);
		assert.ok(
			!settledDiagnostics.some((diag) => diag.message.includes('Missing semicolon.')),
			settledDiagnostics.map((diag) => `${diag.range.start.line}:${diag.message}`).join('\n')
		);
	});

	it('does not misreport missing semicolons on NSF metadata and state block headers', async () => {
		const document = await openFixture('module_diagnostics_nsf_effect_headers_ok.nsf');

		await waitForDiagnostics(
			document,
			(value) => !value.some((diag) => diag.message.includes('Missing semicolon.')),
			'NSF metadata/state header diagnostics'
		);

		await waitForClientQuiescent('NSF metadata/state diagnostics settled');
		const settledDiagnostics = vscode.languages.getDiagnostics(document.uri);
		assert.ok(
			!settledDiagnostics.some((diag) => diag.message.includes('Missing semicolon.')),
			settledDiagnostics.map((diag) => `${diag.range.start.line}:${diag.message}`).join('\n')
		);
	});

	it('does not misreport missing semicolons inside multiline constructor expressions', async () => {
		const document = await openFixture('module_diagnostics_multiline_semicolon_context.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) =>
				Array.isArray(value) &&
				value.some((diag) => diag.message.includes('Missing semicolon.')),
			'multiline constructor diagnostics'
		);

		const missingSemicolons = diagnostics.filter((diag) =>
			diag.message.includes('Missing semicolon.')
		);
		assert.strictEqual(
			missingSemicolons.length,
			1,
			`Expected exactly one missing semicolon diagnostic. Actual diagnostics:\n${diagnostics
				.map((diag) => `${diag.range.start.line}:${diag.message}`)
				.join('\n')}`
		);
		assert.strictEqual(
			missingSemicolons[0].range.start.line,
			12,
			'Expected the missing semicolon diagnostic to stay on the closing line of the broken multiline expression.'
		);
	});

	it('publishes immediate syntax diagnostics in basic mode', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedMode = configuration.inspect<string>('diagnostics.mode');
		const originalMode = inspectedMode?.workspaceValue ?? 'balanced';
		try {
			await configuration.update('diagnostics.mode', 'basic', vscode.ConfigurationTarget.Workspace);
			const document = await openFixture('module_diagnostics_missing_semicolon.nsf');
			const diagnostics = await waitFor(
				() => vscode.languages.getDiagnostics(document.uri),
				(value) =>
					Array.isArray(value) &&
					value.some((diag) => diag.message.includes('Missing semicolon.')),
				'basic mode immediate diagnostics'
			);
			assert.ok(diagnostics.some((diag) => diag.message.includes('Missing semicolon.')));
		} finally {
			await configuration.update('diagnostics.mode', originalMode, vscode.ConfigurationTarget.Workspace);
		}
	});

	it('keeps basic-mode missing semicolon diagnostics on the closing line of multiline expressions', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedMode = configuration.inspect<string>('diagnostics.mode');
		const originalMode = inspectedMode?.workspaceValue ?? 'balanced';
		try {
			await configuration.update('diagnostics.mode', 'basic', vscode.ConfigurationTarget.Workspace);
			const document = await openFixture('module_diagnostics_multiline_semicolon_context.nsf');
			const diagnostics = await waitFor(
				() => vscode.languages.getDiagnostics(document.uri),
				(value) =>
					Array.isArray(value) &&
					value.some((diag) => diag.message.includes('Missing semicolon.')),
				'basic mode multiline constructor diagnostics'
			);
			const missingSemicolons = diagnostics.filter((diag) =>
				diag.message.includes('Missing semicolon.')
			);
			assert.strictEqual(
				missingSemicolons.length,
				1,
				`Expected exactly one basic-mode missing semicolon diagnostic. Actual diagnostics:\n${diagnostics
					.map((diag) => `${diag.range.start.line}:${diag.message}`)
					.join('\n')}`
			);
			assert.strictEqual(missingSemicolons[0].range.start.line, 12);
		} finally {
			await configuration.update('diagnostics.mode', originalMode, vscode.ConfigurationTarget.Workspace);
		}
	});

	it('keeps basic-mode diagnostics quiet for NSF metadata and state block headers', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedMode = configuration.inspect<string>('diagnostics.mode');
		const originalMode = inspectedMode?.workspaceValue ?? 'balanced';
		try {
			await configuration.update('diagnostics.mode', 'basic', vscode.ConfigurationTarget.Workspace);
			const document = await openFixture('module_diagnostics_nsf_effect_headers_ok.nsf');
			await waitForDiagnostics(
				document,
				(value) => !value.some((diag) => diag.message.includes('Missing semicolon.')),
				'basic mode NSF metadata/state header diagnostics'
			);
			await waitForClientQuiescent('basic mode NSF metadata/state diagnostics settled');
			const settledDiagnostics = vscode.languages.getDiagnostics(document.uri);
			assert.ok(
				!settledDiagnostics.some((diag) => diag.message.includes('Missing semicolon.')),
				settledDiagnostics.map((diag) => `${diag.range.start.line}:${diag.message}`).join('\n')
			);
		} finally {
			await configuration.update('diagnostics.mode', originalMode, vscode.ConfigurationTarget.Workspace);
		}
	});

	it('publishes missing semicolon diagnostics when deleting a postfix update semicolon during editing', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedMode = configuration.inspect<string>('diagnostics.mode');
		const originalMode = inspectedMode?.workspaceValue ?? 'balanced';
		let document: vscode.TextDocument | undefined;
		try {
			await configuration.update('diagnostics.mode', 'full', vscode.ConfigurationTarget.Workspace);
			document = await openFixture('module_diagnostics_postfix_update.nsf');
			await waitForDiagnostics(
				document,
				(value) => !value.some((diag) => diag.message.includes('Missing semicolon.')),
				'initial postfix update diagnostics'
			);

			const deleteStart = positionOf(document, 'counter++;', 1, 'counter++'.length);
			const deleteEdit = new vscode.WorkspaceEdit();
			deleteEdit.delete(document.uri, new vscode.Range(deleteStart, deleteStart.translate(0, 1)));
			const applied = await vscode.workspace.applyEdit(deleteEdit);
			assert.ok(applied, 'Expected postfix update semicolon deletion to apply.');
			document = await vscode.workspace.openTextDocument(document.uri);

			const diagnostics = await waitForDiagnostics(
				document,
				(value) => value.some((diag) => diag.message.includes('Missing semicolon.')),
				'edited postfix update missing semicolon diagnostics'
			);
			const missingSemicolon = diagnostics.find((diag) => diag.message.includes('Missing semicolon.'));
			assert.ok(missingSemicolon, 'Expected a missing semicolon diagnostic after deleting the postfix update semicolon.');
			assert.strictEqual(missingSemicolon?.range.start.line, 3);

			await waitForClientQuiescent('postfix update diagnostics settled');
			const settledDiagnostics = vscode.languages.getDiagnostics(document.uri);
			assert.ok(
				settledDiagnostics.some((diag) => diag.message.includes('Missing semicolon.')),
				'Expected missing semicolon diagnostics to persist after deferred/full diagnostics settle.'
			);
		} finally {
			if (document && document.getText().includes('counter++\n')) {
				const insertPosition = positionOf(document, 'counter++', 1, 'counter++'.length);
				const restoreEdit = new vscode.WorkspaceEdit();
				restoreEdit.insert(document.uri, insertPosition, ';');
				const restored = await vscode.workspace.applyEdit(restoreEdit);
				assert.ok(restored, 'Expected postfix update semicolon restoration edit to apply.');
			}
			await configuration.update('diagnostics.mode', originalMode, vscode.ConfigurationTarget.Workspace);
		}
	});

	it('publishes diagnostics for control statements missing parentheses', async () => {
		const document = await openFixture('module_diagnostics_control_missing_paren.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Missing parentheses after if.'));
	});

	it('publishes diagnostics for unreachable code', async () => {
		const document = await openFixture('module_diagnostics_unreachable_code.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Unreachable code.'));
	});

	it('publishes diagnostics for potentially missing return paths', async () => {
		const document = await openFixture('module_diagnostics_partial_return.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Potential missing return on some paths.'));
	});

	it('publishes diagnostics for comparison operator type mismatches', async () => {
		const document = await openFixture('module_diagnostics_type_mismatch_compare.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Implicit truncation conversion: float4 -> float3. Use an explicit cast or swizzle if this is intentional.'));
	});

	it('infers bitwise expressions when checking return types', async () => {
		const document = await openFixture('module_diagnostics_bitwise_expression_inference.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Return type mismatch'));
	});

	it('infers shift operators inside bitwise expressions', async () => {
		const document = await openFixture('module_diagnostics_shift_bitwise_inference.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Assignment type mismatch: uint = bool.'));
		assert.ok(!messages.includes('Return type mismatch'));
	});

	it('supports HLSL attributes like [unroll] without false diagnostics', async () => {
		const document = await openFixture('module_diagnostics_hlsl_attributes.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: unroll.'));
		assert.ok(!messages.includes('Undefined identifier: loop.'));
		assert.ok(!messages.includes('Undefined identifier: branch.'));
		assert.ok(!messages.includes('Unterminated bracket: ['));
	});

	});
}

