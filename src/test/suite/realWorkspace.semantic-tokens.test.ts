import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

import {
	openExternalDocument,
	getDocumentRuntimeDebug,
	positionOf,
	waitFor,
} from './test_helpers';

type Awaitable<T> = T | Thenable<T> | PromiseLike<T>;

type DecodedSemanticToken = {
	line: number;
	start: number;
	length: number;
	type: string;
	modifiers: string[];
	text: string;
};

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const realDescribe = testMode === 'real' ? describe : describe.skip;

function getWorkspaceFolderPath(folderSuffix: string): string {
	const folder = vscode.workspace.workspaceFolders?.find((item) =>
		item.uri.fsPath.replace(/\\/g, '/').toLowerCase().endsWith(folderSuffix.toLowerCase())
	);
	assert.ok(folder, `Expected workspace folder ending with '${folderSuffix}'.`);
	return folder.uri.fsPath;
}

async function waitForIndexingIdle(label: string): Promise<void> {
	await waitFor(
		() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
		(value) => !value?.indexingState || value.indexingState.state === 'Idle',
		label
	);
}

function decodeSemanticTokens(
	document: vscode.TextDocument,
	legend: vscode.SemanticTokensLegend,
	tokens: vscode.SemanticTokens
): DecodedSemanticToken[] {
	const decoded: DecodedSemanticToken[] = [];
	let line = 0;
	let start = 0;
	const data = tokens.data;
	for (let i = 0; i + 4 < data.length; i += 5) {
		const deltaLine = data[i];
		const deltaStart = data[i + 1];
		line += deltaLine;
		start = deltaLine === 0 ? start + deltaStart : deltaStart;
		const length = data[i + 2];
		const tokenType = legend.tokenTypes[data[i + 3]] ?? '';
		const modifierBits = data[i + 4];
		const modifiers = legend.tokenModifiers.filter(
			(_, index) => (modifierBits & (1 << index)) !== 0
		);
		decoded.push({
			line,
			start,
			length,
			type: tokenType,
			modifiers,
			text: document.lineAt(line).text.substr(start, length)
		});
	}
	return decoded;
}

function tokenAtPosition(
	document: vscode.TextDocument,
	tokens: DecodedSemanticToken[],
	position: vscode.Position,
	label: string
): DecodedSemanticToken {
	const token = tokens.find(
		(item) =>
			item.line === position.line &&
			item.start === position.character &&
			item.length > 0 &&
			document.getText(
				new vscode.Range(position, document.positionAt(document.offsetAt(position) + item.length))
			) === item.text
	);
	assert.ok(token, `Expected semantic token for ${label} at ${position.line}:${position.character}.`);
	return token!;
}

function assertTokenRoleAtPosition(
	document: vscode.TextDocument,
	tokens: DecodedSemanticToken[],
	position: vscode.Position,
	expectedType: string,
	expectedModifiers: string[],
	label: string
): DecodedSemanticToken {
	const token = tokenAtPosition(document, tokens, position, label);
	assert.strictEqual(token.type, expectedType, `${label} type`);
	assert.deepStrictEqual(token.modifiers, expectedModifiers, `${label} modifiers`);
	return token;
}

function formatToken(token: DecodedSemanticToken): string {
	const modifierText = token.modifiers.length > 0 ? `+${token.modifiers.join('+')}` : '';
	return `${token.text}@${token.line}:${token.start}=${token.type}${modifierText}`;
}

async function getDecodedSemanticTokens(document: vscode.TextDocument): Promise<DecodedSemanticToken[]> {
	const legend = await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.SemanticTokensLegend>(
				'vscode.provideDocumentSemanticTokensLegend',
				document.uri
			),
		(value) => Array.isArray(value?.tokenTypes) && value.tokenTypes.length > 0,
		'semantic token legend'
	);
	const fullTokens = await waitFor(
		() =>
			vscode.commands.executeCommand<vscode.SemanticTokens>(
				'vscode.provideDocumentSemanticTokens',
				document.uri
			),
		(value) => Boolean(value) && value.data.length > 0,
		'semantic tokens'
	);
	return decodeSemanticTokens(document, legend, fullTokens);
}

realDescribe('NSF real workspace semantic-token probe', () => {
	it('captures role and modifier samples from building.nsf and a shared hlsl include', async function () {
		this.timeout(240000);

		try {
			const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
			const buildingPath = path.join(shaderSourceRoot, 'base', 'building.nsf');
			const lightingPath = path.join(shaderSourceRoot, 'base', 'bluetide_common.hlsl');

			const buildingDocument = await openExternalDocument(buildingPath);
			assert.strictEqual(buildingDocument.languageId, 'nsf');
			await waitForIndexingIdle('real workspace semantic-token indexing idle for building.nsf');
			await vscode.commands.executeCommand('nsf._setActiveUnitForTests', buildingDocument.uri.toString());
			await waitForIndexingIdle('real workspace semantic-token indexing idle after active unit selection');

			const lightingDocument = await openExternalDocument(lightingPath);
			console.log(`[semantic-token-probe] bluetide_common.hlsl languageId=${lightingDocument.languageId}`);
			const [lightingRuntime] = await waitFor(
				() => getDocumentRuntimeDebug([lightingDocument.uri.toString()]),
				(entries) => Array.isArray(entries) && Boolean(entries[0]),
				'real workspace semantic-token runtime entry for bluetide_common.hlsl'
			);
			console.log(
				`[semantic-token-probe] bluetide_common.hlsl activeUnitPath=${lightingRuntime?.activeUnitPath ?? '<none>'}`
			);
			console.log(
				`[semantic-token-probe] bluetide_common.hlsl activeUnitIncludeClosureFingerprint=${
					lightingRuntime?.activeUnitIncludeClosureFingerprint ?? '<none>'
				}`
			);
			console.log(
				`[semantic-token-probe] bluetide_common.hlsl interactiveVisibilityFingerprint=${
					lightingRuntime?.interactiveVisibilityFingerprint ?? '<none>'
				}`
			);
			await vscode.commands.executeCommand('workbench.action.closeActiveEditor');

			const legend = await waitFor(
				() =>
					vscode.commands.executeCommand<vscode.SemanticTokensLegend>(
						'vscode.provideDocumentSemanticTokensLegend',
						buildingDocument.uri
					),
				(value) => Array.isArray(value?.tokenTypes) && value.tokenTypes.length > 0,
				'semantic token legend'
			);
			assert.ok(legend.tokenTypes.includes('parameter'));
			assert.ok(legend.tokenModifiers.includes('declaration'));
			assert.ok(legend.tokenModifiers.includes('modification'));
			console.log(`[semantic-token-probe] legend tokenTypes=${legend.tokenTypes.join(',')}`);
			console.log(`[semantic-token-probe] legend tokenModifiers=${legend.tokenModifiers.join(',')}`);

			const buildingTokens = await getDecodedSemanticTokens(buildingDocument);
			const lightingTokens = await getDecodedSemanticTokens(lightingDocument);

			const buildingSamples = [
				assertTokenRoleAtPosition(
					buildingDocument,
					buildingTokens,
					positionOf(
						buildingDocument,
						'inout PixelMaterialInputs MaterialInputs',
						1,
						'inout PixelMaterialInputs '.length
					),
					'parameter',
					['declaration'],
					'MaterialInputs declaration'
				),
				assertTokenRoleAtPosition(
					buildingDocument,
					buildingTokens,
					positionOf(buildingDocument, 'half4 diffuse_color = (half4)0;', 1, 'half4 '.length),
					'variable',
					['declaration'],
					'diffuse_color declaration'
				),
				assertTokenRoleAtPosition(
					buildingDocument,
					buildingTokens,
					positionOf(buildingDocument, 'diffuse_color = SampleColorTextureByArray', 1, 0),
					'variable',
					['modification'],
					'diffuse_color assignment'
				),
				assertTokenRoleAtPosition(
					buildingDocument,
					buildingTokens,
					positionOf(
						buildingDocument,
						'MaterialInputs.base_color = diffuse_color.rgb;',
						1,
						'MaterialInputs.'.length
					),
					'property',
					['modification'],
					'base_color property write'
				)
			];

			const lightingSamples = [
				assertTokenRoleAtPosition(
					lightingDocument,
					lightingTokens,
					positionOf(
						lightingDocument,
						'inout PixelMaterialInputs MaterialInputs',
						1,
						'inout PixelMaterialInputs '.length
					),
					'parameter',
					['declaration'],
					'MaterialInputs declaration'
				),
				assertTokenRoleAtPosition(
					lightingDocument,
					lightingTokens,
					positionOf(lightingDocument, 'half4 virus_basecolor;', 1, 'half4 '.length),
					'variable',
					['declaration'],
					'virus_basecolor declaration'
				),
				assertTokenRoleAtPosition(
					lightingDocument,
					lightingTokens,
					positionOf(
						lightingDocument,
						'MaterialInputs.base_color = virus_basecolor.xyz;',
						1,
						'MaterialInputs.'.length
					),
					'property',
					['modification'],
					'base_color property write'
				),
				assertTokenRoleAtPosition(
					lightingDocument,
					lightingTokens,
					positionOf(
						lightingDocument,
						'MaterialParameters.world_normal +=  float3(offset.x,1.0,offset.y);',
						1,
						'MaterialParameters.'.length
					),
					'property',
					['modification'],
					'world_normal property write'
				),
				assertTokenRoleAtPosition(
					lightingDocument,
					lightingTokens,
					positionOf(lightingDocument, 'u_camera_pos', 1, 0),
					'variable',
					[],
					'u_camera_pos reference'
				)
			];

			console.log(
				`[semantic-token-probe] building.nsf samples=${buildingSamples.map(formatToken).join(' | ')}`
			);
			console.log(
				`[semantic-token-probe] bluetide_common.hlsl samples=${lightingSamples.map(formatToken).join(' | ')}`
			);
		} finally {
			await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
			await vscode.commands.executeCommand('workbench.action.closeAllEditors');
		}
	});
});
