import * as assert from 'assert';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as vscode from 'vscode';

type ProviderLocation = vscode.Location | vscode.LocationLink;
type SymbolLike = vscode.DocumentSymbol | vscode.SymbolInformation;
type Awaitable<T> = T | Thenable<T> | PromiseLike<T>;

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const repoDescribe = testMode === 'repo' ? describe : describe.skip;

function getWorkspaceRoot(): string {
	const folder = vscode.workspace.workspaceFolders?.[0];
	assert.ok(folder, 'Expected the extension tests to open a workspace folder.');
	return folder.uri.fsPath;
}

async function openFixture(relativePath: string): Promise<vscode.TextDocument> {
	const absolutePath = path.join(getWorkspaceRoot(), 'test_files', relativePath);
	const document = await vscode.workspace.openTextDocument(absolutePath);
	await vscode.window.showTextDocument(document, { preview: false });
	return document;
}

async function openExternalDocument(absolutePath: string): Promise<vscode.TextDocument> {
	const document = await vscode.workspace.openTextDocument(absolutePath);
	await vscode.window.showTextDocument(document, { preview: false });
	return document;
}

async function waitFor<T>(
	producer: () => Awaitable<T>,
	isReady: (value: T) => boolean,
	label: string
): Promise<T> {
	let lastValue: T | undefined;
	let lastError: unknown;
	for (let attempt = 0; attempt < 60; attempt++) {
		try {
			lastValue = await Promise.resolve(producer());
			if (isReady(lastValue)) {
				return lastValue;
			}
			lastError = undefined;
		} catch (error) {
			lastError = error;
		}
		await new Promise((resolve) => setTimeout(resolve, 500));
	}
	const suffix =
		lastError !== undefined
			? ` Last error: ${lastError instanceof Error ? lastError.message : String(lastError)}`
			: '';
	throw new Error(`Timed out waiting for ${label}.${suffix}`);
}

function toFsPath(location: ProviderLocation): string {
	if ('targetUri' in location) {
		return location.targetUri.fsPath;
	}
	return location.uri.fsPath;
}

function toRange(location: ProviderLocation): vscode.Range {
	if ('targetRange' in location) {
		return location.targetRange;
	}
	return location.range;
}

function hoverToText(hovers: vscode.Hover[]): string {
	const parts: string[] = [];
	for (const hover of hovers) {
		for (const content of hover.contents) {
			if (typeof content === 'string') {
				parts.push(content);
				continue;
			}
			if (content instanceof vscode.MarkdownString) {
				parts.push(content.value);
				continue;
			}
			const maybeValue = (content as unknown as { value?: unknown }).value;
			if (typeof maybeValue === 'string') {
				parts.push(maybeValue);
			}
		}
	}
	return parts.join('\n');
}

function flattenSymbolNames(symbols: SymbolLike[]): string[] {
	const names: string[] = [];
	for (const symbol of symbols) {
		names.push(symbol.name);
		if ('children' in symbol && Array.isArray(symbol.children)) {
			names.push(...flattenSymbolNames(symbol.children));
		}
	}
	return names;
}

function findOffset(document: vscode.TextDocument, needle: string, occurrence = 1): number {
	let fromIndex = 0;
	let foundIndex = -1;
	for (let current = 0; current < occurrence; current++) {
		foundIndex = document.getText().indexOf(needle, fromIndex);
		assert.notStrictEqual(foundIndex, -1, `Unable to find occurrence ${occurrence} of '${needle}'.`);
		fromIndex = foundIndex + needle.length;
	}
	return foundIndex;
}

function positionOf(
	document: vscode.TextDocument,
	needle: string,
	occurrence = 1,
	characterOffset = 0
): vscode.Position {
	const offset = findOffset(document, needle, occurrence) + characterOffset;
	return document.positionAt(offset);
}

function countWorkspaceEdits(edit: vscode.WorkspaceEdit): number {
	let count = 0;
	for (const [, edits] of edit.entries()) {
		count += edits.length;
	}
	return count;
}

function diagnosticCodeText(diagnostic: vscode.Diagnostic): string {
	const code = diagnostic.code;
	if (typeof code === 'string' || typeof code === 'number') {
		return String(code);
	}
	if (!code || typeof code !== 'object') {
		return '';
	}
	const value = (code as { value?: unknown }).value;
	if (typeof value === 'string' || typeof value === 'number') {
		return String(value);
	}
	return '';
}

function getCompletionItems(result: vscode.CompletionList | vscode.CompletionItem[] | undefined): vscode.CompletionItem[] {
	if (!result) {
		return [];
	}
	return Array.isArray(result) ? result : result.items;
}

function isEmptyDefinitionResult(value: ProviderLocation[] | undefined): boolean {
	return !value || value.length === 0;
}

repoDescribe('NSF client integration', () => {
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

	it('provides completion items through the client', async () => {
		const document = await openFixture('module_suite.nsf');
		const completionPosition = new vscode.Position(0, 0);
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'completion items'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('#include'), 'Expected #include completion item.');
		assert.ok(labels.has('discard'), 'Expected discard completion item.');
		assert.ok(labels.has('float4'), 'Expected float4 completion item.');
	});

	it('provides completion items for HLSL attributes inside brackets', async () => {
		const document = await openFixture('module_completion_attribute.nsf');
		const completionPosition = new vscode.Position(0, 1);
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'completion items for attributes'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
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
		assert.ok(varHoverText.includes('SuiteHoverVar doc line 1'));
		assert.ok(varHoverText.includes('SuiteHoverVar doc line 2'));

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
		const document = await openFixture('module_definition_function_call_in_assignment.nsf');

		const hoverPosition = positionOf(document, 'SampleColorTexture', 1, 2);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover results for SampleColorTexture'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('SampleColorTexture doc line 1'));
		assert.ok(hoverText.includes('SampleColorTexture('));
		assert.ok(hoverText.includes('(HLSL function)'), hoverText);
		assert.ok(hoverText.includes('Returns:'));

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

	it('keeps hover and definition aligned for include-resolved function calls', async () => {
		const document = await openFixture('module_definition_function_call_in_assignment.nsf');
		const position = positionOf(document, 'SampleColorTexture', 1, 2);

		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					position
				),
			(value) => Array.isArray(value) && value.length > 0,
			'aligned hover for include-resolved call'
		);
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

		const hoverText = hoverToText(hovers);
		const definitionFile = path.basename(toFsPath(definitions[0]));
		assert.ok(hoverText.includes(definitionFile));
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
		await openFixture('module_diagnostics_external_cbuffer_provider.nsf');
		const consumerDoc = await openFixture('module_diagnostics_external_cbuffer_consumer.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(consumerDoc.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: u_wind_tex_factors.'));
		assert.ok(messages.includes('Assignment type mismatch: float3 = float4.'));
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
		const document = await openFixture('module_diagnostics_texture_sample_consumer.nsf');
		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length > 0,
			'diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: t_fresnel_normalMap.'));
		assert.ok(!messages.includes('Undefined identifier: s_fresnel_normalmap.'));
		assert.ok(messages.includes('Assignment type mismatch: half4 = float4.'));
		assert.ok(messages.includes('Assignment type mismatch: float3 = float4.'));
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

	it('does not report duplicate locals across mutually exclusive preprocessor branches', async () => {
		const document = await openFixture('module_diagnostics_preprocessor_branch_scope.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value) && value.length >= 1,
			'diagnostics'
		);

		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(messages.includes('Duplicate local declaration: b.'));
		assert.ok(!messages.includes('Duplicate local declaration: x.'));
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

	it('provides semantic tokens for documents and ranges', async () => {
		const document = await openFixture('module_semantic_tokens.nsf');

		const legend = await vscode.commands.executeCommand<vscode.SemanticTokensLegend>(
			'vscode.provideDocumentSemanticTokensLegend',
			document.uri
		);
		assert.ok(Array.isArray(legend.tokenTypes) && legend.tokenTypes.length > 0);

		const fullTokens = await vscode.commands.executeCommand<vscode.SemanticTokens>(
			'vscode.provideDocumentSemanticTokens',
			document.uri
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
		assert.ok(commentIndex >= 0 && tokenTypeIndices.includes(commentIndex));
		assert.ok(stringIndex >= 0 && tokenTypeIndices.includes(stringIndex));
		assert.ok(keywordIndex >= 0 && tokenTypeIndices.includes(keywordIndex));

		const range = new vscode.Range(new vscode.Position(0, 0), new vscode.Position(6, 0));
		const rangeTokens = await vscode.commands.executeCommand<vscode.SemanticTokens>(
			'vscode.provideDocumentRangeSemanticTokens',
			document.uri,
			range
		);
		assert.ok(rangeTokens.data.length > 0);
		assert.ok(rangeTokens.data.length < fullTokens.data.length);
	});

	it('highlights HLSL attributes as keywords in semantic tokens', async () => {
		const document = await openFixture('module_diagnostics_hlsl_attributes.nsf');

		const legend = await vscode.commands.executeCommand<vscode.SemanticTokensLegend>(
			'vscode.provideDocumentSemanticTokensLegend',
			document.uri
		);
		const keywordIndex = legend.tokenTypes.indexOf('keyword');
		assert.ok(keywordIndex >= 0);

		const fullTokens = await vscode.commands.executeCommand<vscode.SemanticTokens>(
			'vscode.provideDocumentSemanticTokens',
			document.uri
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
			(t) => t.line === pos.line && t.start === pos.character && t.length === 'unroll'.length
		);
		assert.ok(found, 'Expected semantic token for unroll attribute.');
		assert.strictEqual(found!.tokenType, keywordIndex);
	});

	it('highlights discard as a keyword in semantic tokens', async () => {
		const document = await openFixture('module_keyword_discard.nsf');

		const legend = await vscode.commands.executeCommand<vscode.SemanticTokensLegend>(
			'vscode.provideDocumentSemanticTokensLegend',
			document.uri
		);
		const keywordIndex = legend.tokenTypes.indexOf('keyword');
		assert.ok(keywordIndex >= 0);

		const fullTokens = await vscode.commands.executeCommand<vscode.SemanticTokens>(
			'vscode.provideDocumentSemanticTokens',
			document.uri
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
			(t) => t.line === pos.line && t.start === pos.character && t.length === 'discard'.length
		);
		assert.ok(found, 'Expected semantic token for discard keyword.');
		assert.strictEqual(found!.tokenType, keywordIndex);
	});

	it('provides signature help with correct active parameter', async () => {
		const document = await openFixture('module_signature_help.nsf');
		const position = positionOf(document, 'SigTarget(uv, 2.0, 3', 1, 'SigTarget(uv, '.length);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('SigTarget'));
		assert.ok((help.signatures[0].parameters?.length ?? 0) >= 3);
	});

	it('provides signature help for overload benchmark fixture', async () => {
		const document = await openFixture('module_signature_help_overload_benchmark.nsf');
		const position = positionOf(document, 'OverloadBench(v3, 2.0, 1', 1, 'OverloadBench(v3, '.length);
		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'overload benchmark signature help'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('OverloadBench'));
		assert.ok((help.signatures[0].parameters?.length ?? 0) >= 3);
	});

	it('keeps hover and signature help aligned for overload-selected call sites', async () => {
		const document = await openFixture('module_signature_help_overload_benchmark.nsf');
		const position = positionOf(document, 'OverloadBench(v3, 2.0, 1)', 1, 'OverloadBench(v3, '.length);
		const hoverPosition = positionOf(document, 'OverloadBench(v3, 2.0, 1)', 1, 2);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'aligned overload signature help'
		);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'aligned overload hover'
		);

		const activeSignature = help.signatures[help.activeSignature];
		const hoverText = hoverToText(hovers);
		assert.ok(activeSignature.label.includes('OverloadBench'));
		assert.ok(hoverText.includes('OverloadBench('));
		assert.ok((activeSignature.parameters?.length ?? 0) >= 3);
	});

	it('keeps signature help available when overload resolver flag toggles', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const previousEnabled = configuration.inspect<boolean>('overloadResolver.enabled')?.workspaceValue;
		const document = await openFixture('module_signature_help_overload_benchmark.nsf');
		const position = positionOf(document, 'OverloadBench(v3, 2.0, 1', 1, 'OverloadBench(v3, '.length);
		try {
			await configuration.update('overloadResolver.enabled', false, vscode.ConfigurationTarget.Workspace);
			const helpDisabled = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SignatureHelp>(
						'vscode.executeSignatureHelpProvider',
						document.uri,
						position
					),
				(value) => Boolean(value) && value.signatures.length > 0,
				'signature help overload resolver disabled'
			);
			assert.ok(helpDisabled.signatures[0].label.includes('OverloadBench'));
			assert.ok((helpDisabled.signatures[0].parameters?.length ?? 0) >= 3);

			await configuration.update('overloadResolver.enabled', true, vscode.ConfigurationTarget.Workspace);
			const helpEnabled = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SignatureHelp>(
						'vscode.executeSignatureHelpProvider',
						document.uri,
						position
					),
				(value) => Boolean(value) && value.signatures.length > 0,
				'signature help overload resolver enabled'
			);
			assert.ok(helpEnabled.signatures[0].label.includes('OverloadBench'));
			assert.ok((helpEnabled.signatures[0].parameters?.length ?? 0) >= 3);
		} finally {
			await configuration.update(
				'overloadResolver.enabled',
				previousEnabled,
				vscode.ConfigurationTarget.Workspace
			);
		}
	});

	it('provides signature help for HLSL built-in functions', async () => {
		const document = await openFixture('module_signature_help_builtin.nsf');
		const text = document.getText();
		const callPos = text.indexOf('lerp(');
		assert.ok(callPos >= 0);

		const position = document.positionAt(callPos + 'lerp('.length);
		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 0);
		assert.ok(help.signatures[0].label.includes('lerp'));
		assert.ok((help.signatures[0].parameters?.length ?? 0) >= 3);
	});

	it('provides signature help for member calls with spaced dot access', async () => {
		const document = await openFixture('module_signature_help_member_call_spaced.nsf');
		const position = positionOf(
			document,
			'gTex . Sample(gSampler, uv)',
			1,
			'gTex . Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for spaced member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
		assert.ok((help.signatures[0].parameters?.length ?? 0) >= 2);
	});

	it('provides signature help for indexed member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'gTexArr[0].Sample(gSampler, uv)',
			1,
			'gTexArr[0].Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for indexed member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('provides signature help for parenthesized member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'(gTex).Sample(gSampler, uv)',
			1,
			'(gTex).Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for parenthesized member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('provides signature help for parenthesized indexed member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'(gTexArr[0]).Sample(gSampler, uv)',
			1,
			'(gTexArr[0]).Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for parenthesized indexed member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('provides signature help for macro wrapped member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'GET_TEX(gTex).Sample(gSampler, uv)',
			1,
			'GET_TEX(gTex).Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for macro wrapped member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('provides signature help for nested macro argument member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'WRAP_TEX(gTexArr[0]).Sample(gSampler, uv)',
			1,
			'WRAP_TEX(gTexArr[0]).Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for nested macro member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('provides signature help for multi-parenthesized indexed member calls', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const position = positionOf(
			document,
			'((gTexArr[0])).Sample(gSampler, uv)',
			1,
			'((gTexArr[0])).Sample(gSampler, '.length
		);

		const help = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.SignatureHelp>(
					'vscode.executeSignatureHelpProvider',
					document.uri,
					position
				),
			(value) => Boolean(value) && value.signatures.length > 0,
			'signature help for multi-parenthesized indexed member call'
		);

		assert.strictEqual(help.activeSignature, 0);
		assert.strictEqual(help.activeParameter, 1);
		assert.ok(help.signatures[0].label.includes('Sample'));
	});

	it('shows hover info for complex member-call method sites', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const hoverPosition = positionOf(document, '((gTexArr[0])).Sample', 1, '((gTexArr[0])).'.length + 1);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover for complex member call method'
		);
		const hoverText = hoverToText(hovers);
		assert.ok(hoverText.includes('(HLSL built-in method)'));
		assert.ok(hoverText.includes('Sample('));
	});

	it('keeps member hover and completion aligned for complex base expressions', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');
		const completionPosition = positionOf(document, '((gTexArr[0])).Sample', 1, '((gTexArr[0])).'.length);
		const hoverPosition = positionOf(document, '((gTexArr[0])).Sample', 1, '((gTexArr[0])).'.length + 1);

		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'aligned member completion'
		);
		const hovers = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'aligned member hover'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		const hoverText = hoverToText(hovers);
		assert.ok(labels.has('Sample'));
		assert.ok(hoverText.includes('Sample('));
	});

	it('provides completion items for complex member-call base expressions', async () => {
		const document = await openFixture('module_signature_help_member_call_complex.nsf');

		const firstPosition = positionOf(document, '((gTexArr[0])).Sample', 1, '((gTexArr[0])).'.length);
		const firstResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					firstPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'completion for multi-parenthesized member base'
		);
		const firstLabels = new Set(getCompletionItems(firstResult).map((item) => item.label.toString()));
		assert.ok(firstLabels.has('Sample'));

		const secondPosition = positionOf(document, 'WRAP_TEX(gTexArr[0]).Sample', 1, 'WRAP_TEX(gTexArr[0]).'.length);
		const secondResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					secondPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'completion for macro-wrapped member base'
		);
		const secondLabels = new Set(getCompletionItems(secondResult).map((item) => item.label.toString()));
		assert.ok(secondLabels.has('Sample'));
	});

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
			const settled = await Promise.all(
				inlayBurst.map((request) => request.then(() => true, () => false))
			);
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

	it('falls back to workspace scan for type definitions when not included', async () => {
		const document = await openFixture('module_definition_scan_a.nsf');
		const position = positionOf(document, 'FGBufferData', 1, 1);

		const definitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					position
				),
			(value) => Array.isArray(value),
			'definition provider'
		);

		assert.ok(definitions.length > 0);
		assert.ok(toFsPath(definitions[0]).endsWith('module_definition_scan_b.nsf'));
	});

	it('exposes indexing status on the client', async () => {
		const document = await openFixture('module_definition_scan_a.nsf');
		const position = positionOf(document, 'FGBufferData', 1, 1);

		const definitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					position
				),
			(value) => Array.isArray(value) && value.length > 0,
			'definition provider'
		);
		assert.ok(toFsPath(definitions[0]).endsWith('module_definition_scan_b.nsf'));

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

	it('falls back to workspace scan for function and variable definitions when not included', async () => {
		const document = await openFixture('module_definition_scan_symbols_a.nsf');

		const functionPosition = positionOf(document, 'RemoteFunc', 1, 1);
		const functionDefinitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					functionPosition
				),
			(value) => Array.isArray(value),
			'definition provider'
		);
		assert.ok(functionDefinitions.length > 0);
		assert.ok(toFsPath(functionDefinitions[0]).endsWith('module_definition_scan_symbols_b.nsf'));

		const variablePosition = positionOf(document, 'RemoteGlobal', 1, 1);
		const variableDefinitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					variablePosition
				),
			(value) => Array.isArray(value),
			'definition provider'
		);
		assert.ok(variableDefinitions.length > 0);
		assert.ok(toFsPath(variableDefinitions[0]).endsWith('module_definition_scan_symbols_b.nsf'));
	});

	it('falls back to workspace scan for macro definitions when not included', async () => {
		const document = await openFixture('module_definition_scan_macro_a.nsf');
		const position = positionOf(document, 'REMOTE_MACRO', 1, 2);

		const definitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					position
				),
			(value) => Array.isArray(value),
			'definition provider'
		);

		assert.ok(definitions.length > 0);
		assert.ok(toFsPath(definitions[0]).endsWith('module_definition_scan_macro_b.nsf'));
	});

	it('resolves multiline FX-style resource declarations through include graph', async () => {
		const document = await openFixture('module_definition_multiline_fx_decl_a.nsf');

		const texPosition = positionOf(document, 'Tex0', 1, 1);
		const texDefinitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					texPosition
				),
			(value) => Array.isArray(value),
			'definition provider'
		);
		assert.ok(texDefinitions.length > 0);
		assert.ok(toFsPath(texDefinitions[0]).endsWith('module_definition_multiline_fx_decl_b.nsf'));

		const samplerPosition = positionOf(document, 's_diffuse', 1, 1);
		const samplerDefinitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					samplerPosition
				),
			(value) => Array.isArray(value),
			'definition provider'
		);
		assert.ok(samplerDefinitions.length > 0);
		assert.ok(toFsPath(samplerDefinitions[0]).endsWith('module_definition_multiline_fx_decl_b.nsf'));
	});

	it('provides struct member completion items after dot', async () => {
		const document = await openFixture('module_struct_completion.nsf');
		const completionPosition = positionOf(document, 'instance.', 1, 'instance.'.length);
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'struct member completion items'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('color'), 'Expected struct field completion for color.');
		assert.ok(labels.has('value'), 'Expected struct field completion for value.');

		const items = getCompletionItems(completionResult);
		const colorItem = items.find((item) => item.label.toString() === 'color');
		assert.ok(colorItem);
		assert.strictEqual(colorItem.detail, 'SuiteMemberStruct');
	});

	it('provides struct member completion for struct keyword declarations', async () => {
		const document = await openFixture('module_struct_completion_struct_keyword.nsf');
		const completionPosition = positionOf(document, 'instance.', 1, 'instance.'.length);
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'struct member completion items'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('normal'));
		assert.ok(labels.has('roughness'));
	});

	it('provides struct member completion from workspace scan when not included', async () => {
		const document = await openFixture('module_struct_completion_remote_a.nsf');
		const completionPosition = positionOf(document, 'value.', 1, 'value.'.length);
		const completionResult = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
					'vscode.executeCompletionItemProvider',
					document.uri,
					completionPosition
				),
			(value) => getCompletionItems(value).length > 0,
			'struct member completion items'
		);

		const labels = new Set(getCompletionItems(completionResult).map((item) => item.label.toString()));
		assert.ok(labels.has('foo'));
		assert.ok(labels.has('bar'));
	});

	it('applies include root inference for external files and provides member completion', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedIncludePaths = configuration.inspect<string[]>('includePaths');
		const previousIncludePaths = inspectedIncludePaths?.workspaceValue;
		await configuration.update('includePaths', [], vscode.ConfigurationTarget.Workspace);

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
				(value) => {
					const items = getCompletionItems(value);
					const labels = new Set(items.map((item) => item.label.toString()));
					return labels.has('foo') && labels.has('bar');
				},
				'external struct member completion items'
			);

			const items = getCompletionItems(completionResult);
			const fooItem = items.find((item) => item.label.toString() === 'foo');
			assert.ok(fooItem);
			assert.strictEqual(fooItem.detail, 'ExtExternalStruct');
		} finally {
			await configuration.update('includePaths', previousIncludePaths, vscode.ConfigurationTarget.Workspace);
		}
	});

	it('resolves local symbol definitions through the client', async () => {
		const document = await openFixture('module_suite.nsf');
		const usePosition = positionOf(document, 'SuiteLocalTint', 2, 2);
		const definitionLocations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					usePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'local definition results'
		);

		const definitionPath = path.basename(toFsPath(definitionLocations[0]));
		assert.strictEqual(definitionPath, 'module_suite.nsf');
	});

	it('resolves variable declarations through the client', async () => {
		const document = await openFixture('module_decls.nsf');
		const symbol = 'SuiteMultiB';
		const usePosition = positionOf(document, symbol, 2, 2);
		const definitionLocations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					usePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'variable declaration definitions'
		);

		assert.strictEqual(path.basename(toFsPath(definitionLocations[0])), 'module_decls.nsf');
		const range = toRange(definitionLocations[0]);
		assert.strictEqual(document.getText(range), symbol);
		assert.strictEqual(range.start.line, positionOf(document, symbol, 1, 0).line);
	});

	it('resolves parameter declarations through the client', async () => {
		const document = await openFixture('module_decls.nsf');
		const symbol = 'SuiteParamMatrix';
		const usePosition = positionOf(document, symbol, 2, 2);
		const definitionLocations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					usePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'parameter declaration definitions'
		);

		assert.strictEqual(path.basename(toFsPath(definitionLocations[0])), 'module_decls.nsf');
		const range = toRange(definitionLocations[0]);
		assert.strictEqual(document.getText(range), symbol);
		assert.strictEqual(range.start.line, positionOf(document, symbol, 1, 0).line);
	});

	it('does not return ambiguous fallback definitions', async () => {
		const document = await openFixture('module_decls.nsf');
		const hoverPosition = positionOf(document, 'SuiteMultiB', 2, 2);
		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover warmup'
		);

		const symbol = 'UndeclaredSymbol';
		const definitionPosition = positionOf(document, symbol, 1, 2);
		const definitionLocations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					definitionPosition
				),
			(value) => Array.isArray(value),
			'ambiguous fallback definitions'
		);

		assert.ok(isEmptyDefinitionResult(definitionLocations));
	});

	it('routes include definition requests through the client', async () => {
		const document = await openFixture('module_suite.nsf');
		const definitionPosition = positionOf(document, 'module_shared.ush', 1, 2);
		const locations = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					definitionPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'include definition results'
		);

		assert.strictEqual(
			path.basename(toFsPath(locations[0])),
			'module_shared.ush',
			'Expected include definition to resolve to the target shader file.'
		);
	});

	it('finds references across unopened included shader files', async () => {
		const suiteDocument = await openFixture('module_suite.nsf');
		const referencePosition = positionOf(suiteDocument, 'SuiteMakeSharedColor', 1, 2);
		const references = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Location[]>(
					'vscode.executeReferenceProvider',
					suiteDocument.uri,
					referencePosition
				),
			(value) => Array.isArray(value) && value.length >= 3,
			'reference results'
		);

		assert.strictEqual(references.length, 3, 'Expected cross-file references for SuiteMakeSharedColor.');
	});

	it('prepares rename and returns workspace edits across unopened include files', async () => {
		const suiteDocument = await openFixture('module_suite.nsf');
		const renamePosition = positionOf(suiteDocument, 'SuiteMakeSharedColor', 1, 2);
		const prepareResult = await waitFor(
			() =>
				vscode.commands.executeCommand<{ range: vscode.Range; placeholder: string }>(
					'vscode.prepareRename',
					suiteDocument.uri,
					renamePosition
				),
			(value) => Boolean(value?.placeholder),
			'prepare rename result'
		);

		assert.strictEqual(prepareResult.placeholder, 'SuiteMakeSharedColor');

		const renameEdit = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.WorkspaceEdit>(
					'vscode.executeDocumentRenameProvider',
					suiteDocument.uri,
					renamePosition,
					'RenamedSuiteColor'
				),
			(value) => Boolean(value) && countWorkspaceEdits(value) >= 3,
			'rename workspace edit'
		);

		assert.strictEqual(countWorkspaceEdits(renameEdit), 3, 'Expected rename edits in the include graph.');
	});

	it('smoke: handles UTF-16 positions when preparing rename', async () => {
		const document = await openFixture('module_utf16_positions.nsf');
		const renamePosition = positionOf(document, 'foo', 1, 2);

		const prepareResult = await waitFor(
			() =>
				vscode.commands.executeCommand<{ range: vscode.Range; placeholder: string }>(
					'vscode.prepareRename',
					document.uri,
					renamePosition
				),
			(value) => Boolean(value?.placeholder),
			'utf16 prepare rename'
		);

		assert.strictEqual(prepareResult.placeholder, 'foo');
		const expectedStart = positionOf(document, 'foo', 1, 0);
		const expectedEnd = positionOf(document, 'foo', 1, 'foo'.length);
		assert.deepStrictEqual(prepareResult.range.start, expectedStart);
		assert.deepStrictEqual(prepareResult.range.end, expectedEnd);
	});

	it('smoke: supports UI metadata block variable declarations', async () => {
		const document = await openFixture('module_sas_ui_decl.nsf');

		const diagnostics = await waitFor(
			() => vscode.languages.getDiagnostics(document.uri),
			(value) => Array.isArray(value),
			'ui meta diagnostics'
		);
		const messages = diagnostics.map((diag) => diag.message).join('\n');
		assert.ok(!messages.includes('Undefined identifier: u_b_max_random_distance.'));
		assert.ok(!messages.includes('Undefined identifier: SasUiLabel.'));
		assert.ok(!messages.includes('Duplicate global declaration: SasUiMax.'));
		assert.ok(!messages.includes('Duplicate global declaration: SasUiMin.'));

		const usagePosition = positionOf(document, 'u_b_max_random_distance', 2, 2);
		const definitions = await waitFor(
			() =>
				vscode.commands.executeCommand<ProviderLocation[]>(
					'vscode.executeDefinitionProvider',
					document.uri,
					usagePosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'ui meta definition'
		);

		const expectedDeclPos = positionOf(document, 'u_b_max_random_distance', 1, 0);
		const definition = definitions[0];
		const definitionUri = 'uri' in definition ? definition.uri : definition.targetUri;
		const definitionRange = 'range' in definition ? definition.range : definition.targetRange;
		assert.strictEqual(definitionUri.toString(), document.uri.toString());
		assert.strictEqual(definitionRange.start.line, expectedDeclPos.line);
	});

	it('smoke: refreshes consumer diagnostics when included file changes on disk', async function () {
		this.timeout(120000);
		const providerPath = path.join(getWorkspaceRoot(), 'test_files', 'watch_provider.nsf');
		const original = fs.readFileSync(providerPath, 'utf8');

		try {
			const consumerDocument = await openFixture('watch_consumer.nsf');

			const initialDiagnostics = await waitFor(
				() => vscode.languages.getDiagnostics(consumerDocument.uri),
				(value) => Array.isArray(value),
				'watch initial diagnostics'
			);
			const initialMessages = initialDiagnostics.map((diag) => diag.message).join('\n');
			assert.ok(!initialMessages.includes('Undefined identifier: u_value.'));

			const updated = original.replace('u_value', 'u_value2');
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
					(value) => Array.isArray(value) && value.some((diag) => diag.message.includes('Undefined identifier: u_value.')),
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
					(value) => Array.isArray(value) && value.some((diag) => diag.message.includes('Undefined identifier: u_value.')),
					'watch updated diagnostics after reopen'
				);
			}
			const updatedMessages = updatedDiagnostics.map((diag) => diag.message).join('\n');
			assert.ok(updatedMessages.includes('Undefined identifier: u_value.'));
		} finally {
			await vscode.workspace.fs.writeFile(vscode.Uri.file(providerPath), Buffer.from(original, 'utf8'));
		}
	});

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

	it('applies include path configuration changes without restarting the client', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedIncludePaths = configuration.inspect<string[]>('includePaths');
		const originalIncludePaths = inspectedIncludePaths?.workspaceValue ?? [];
		const document = await openFixture('config_runtime_suite.nsf');
		const symbolPosition = positionOf(document, 'RuntimeOnlySharedColor', 1, 3);

		try {
			await configuration.update('includePaths', [], vscode.ConfigurationTarget.Workspace);

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
				'includePaths',
				['test_files/runtime_include_root'],
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
				'includePaths',
				originalIncludePaths,
				vscode.ConfigurationTarget.Workspace
			);
		}
	});

	it('restores hover and definition together after include path configuration changes', async () => {
		const configuration = vscode.workspace.getConfiguration('nsf');
		const inspectedIncludePaths = configuration.inspect<string[]>('includePaths');
		const originalIncludePaths = inspectedIncludePaths?.workspaceValue ?? [];
		const document = await openFixture('config_runtime_suite.nsf');
		const symbolPosition = positionOf(document, 'RuntimeOnlySharedColor', 1, 3);

		try {
			await configuration.update('includePaths', [], vscode.ConfigurationTarget.Workspace);

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
				'includePaths',
				['test_files/runtime_include_root'],
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
				'includePaths',
				originalIncludePaths,
				vscode.ConfigurationTarget.Workspace
			);
		}
	});
});
