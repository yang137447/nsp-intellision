import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	diagnosticCodeText,
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
		assert.ok(onMessages.includes('Assignment type mismatch: float2 = float4.'));
	});

	it('expands object-like macros and arithmetic comparisons in #if expressions', async () => {
		const document = await openFixture('module_diagnostics_preprocessor_object_macro_expr.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Assignment type mismatch: float2 = float4.'));
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
		assert.ok(messages.includes('Return type mismatch: expected float3 but got float4.'));
	});

	it('publishes diagnostics for assignment and operator type mismatches', async () => {
		const assignDoc = await openFixture('module_diagnostics_type_mismatch_assign.nsf');
		const assignDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(assignDoc.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);
		const assignMessages = assignDiagnostics.map((diag) => diag.message).join('\n');
		assert.ok(assignMessages.includes('Assignment type mismatch: float3 = float4.'));

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
		assert.ok(opMessages.includes('Binary operator type mismatch: float3 + float4.'));
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
		assert.ok(mismatchMessages.includes('Assignment type mismatch: float3 = float4.'));
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
						messages.includes('Assignment type mismatch: float3 = float4.') &&
						!messages.includes('Undefined identifier: u_wind_tex_factors.')
					);
				},
				'external cbuffer diagnostics'
			);
			const messages = diagnostics.map((diag) => diag.message).join('\n');
			assert.ok(!messages.includes('Undefined identifier: u_wind_tex_factors.'));
			assert.ok(messages.includes('Assignment type mismatch: float3 = float4.'));
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
		assert.ok(messages.includes('Assignment type mismatch: float3 = float4.'));
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
		assert.ok(messages.includes('Assignment type mismatch: float3 = float4.'));
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
						messages.includes('Assignment type mismatch: half4 = float4.') &&
						messages.includes('Assignment type mismatch: float3 = float4.') &&
						!messages.includes('Undefined identifier: t_fresnel_normalMap.') &&
						!messages.includes('Undefined identifier: s_fresnel_normalmap.')
					);
				},
				'texture sample diagnostics'
			);
			const messages = diagnostics.map((diag) => diag.message).join('\n');
			assert.ok(!messages.includes('Undefined identifier: t_fresnel_normalMap.'));
			assert.ok(!messages.includes('Undefined identifier: s_fresnel_normalmap.'));
			assert.ok(messages.includes('Assignment type mismatch: half4 = float4.'));
			assert.ok(messages.includes('Assignment type mismatch: float3 = float4.'));
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
			(value) => Array.isArray(value) && value.length >= 2,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Assignment type mismatch: half = float.'));
		assert.ok(messages.includes('Assignment type mismatch: half3 = float3.'));
		for (const diag of diagnostics) {
			if (
				diag.message.includes('Assignment type mismatch: half = float.') ||
				diag.message.includes('Assignment type mismatch: half3 = float3.')
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
		const powMismatch = powDiagnostics.find((diag) => diag.message.includes('Assignment type mismatch: half = float.'));
		assert.ok(powMismatch, 'Expected half=float narrowing warning for pow.');
		assert.strictEqual(powMismatch!.severity, vscode.DiagnosticSeverity.Warning);

		const normalizeDocument = await openFixture('narrowing_normalize_half3.nsf');
		const normalizeDiagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(normalizeDocument.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics for narrowing_normalize_half3'
		);
		const normalizeMismatch = normalizeDiagnostics.find((diag) =>
			diag.message.includes('Assignment type mismatch: half3 = float3.')
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
		assert.ok(messages.includes('Builtin call type mismatch: dot.'));
		assert.ok(messages.includes('Builtin call type mismatch: lerp.'));
		assert.ok(messages.includes('Builtin call mixed integer signedness: max.'));
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

	it('displays inferred argument types instead of nearby identifiers in user function mismatch', async () => {
		const document = await openFixture('module_diagnostics_user_function_call_arg_display_regression.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const mismatch = diagnostics.find((diag) => diag.message.includes('Function call argument mismatch: Interaction.'));
		assert.ok(mismatch, 'Expected user function argument mismatch for Interaction.');
		assert.ok(mismatch!.message.includes('Expected: (VS_INPUT, float4, float3).'));
		assert.ok(mismatch!.message.includes('Got: (VS_INPUT, float4, float4).'));
		assert.ok(!mismatch!.message.includes('world_position'));
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
		assert.ok(messages.includes('Binary operator type mismatch: float3 == float4.'));
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

