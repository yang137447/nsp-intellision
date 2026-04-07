import * as assert from 'assert';
import * as vscode from 'vscode';

import { writePerfReport } from './perf_helpers';
import { openFixture, positionOf, repoDescribe } from './test_helpers';

function getCompletionItems(
	result: vscode.CompletionList | vscode.CompletionItem[] | undefined
): vscode.CompletionItem[] {
	if (!result) {
		return [];
	}
	return Array.isArray(result) ? result : result.items;
}

type MatrixCase = {
	id: string;
	file: string;
	category: string;
	anchorText: string;
	deleteFrom?: string;
	deleteTo?: string;
	expectedLabels: string[];
	mustPass: boolean;
};

async function runCase(testCase: MatrixCase): Promise<{
	id: string;
	category: string;
	file: string;
	passed: boolean;
	labels: string[];
	debug: unknown;
}> {
	const document = await openFixture(testCase.file);
	let revertRange: vscode.Range | undefined;
	let revertText = '';

	if (testCase.deleteFrom && testCase.deleteTo) {
		const sourcePosition = positionOf(document, testCase.deleteFrom, 1, 0);
		revertRange = new vscode.Range(
			sourcePosition.translate(0, testCase.deleteFrom.length),
			sourcePosition.translate(0, testCase.deleteTo.length)
		);
		revertText = document.getText(revertRange);
		const edit = new vscode.WorkspaceEdit();
		edit.delete(document.uri, revertRange);
		await vscode.workspace.applyEdit(edit);
	}

	try {
		const updatedDocument = await vscode.workspace.openTextDocument(document.uri);
		const completionPosition = positionOf(
			updatedDocument,
			testCase.anchorText,
			1,
			testCase.anchorText.length
		);
		const completionResult =
			await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				updatedDocument.uri,
				completionPosition,
				'.'
			);
		const labels = Array.from(
			new Set(getCompletionItems(completionResult).map((item) => item.label.toString()))
		);
		const debug = await vscode.commands.executeCommand<any>('nsf._getLastCompletionDebug');
		const passed = testCase.expectedLabels.every((label) => labels.includes(label));
		return {
			id: testCase.id,
			category: testCase.category,
			file: testCase.file,
			passed,
			labels: labels.slice(0, 40),
			debug
		};
	} finally {
		if (revertRange) {
			const restore = new vscode.WorkspaceEdit();
			restore.insert(document.uri, revertRange.start, revertText);
			await vscode.workspace.applyEdit(restore);
		}
	}
}

repoDescribe('NSF client integration: Member Completion Matrix', () => {
	it('captures dot-member completion coverage across representative categories', async function () {
		this.timeout(240000);

		const cases: MatrixCase[] = [
			{
				id: 'struct-field',
				file: 'module_struct_completion.nsf',
				category: 'struct field',
				anchorText: 'instance.',
				expectedLabels: ['color', 'value'],
				mustPass: true
			},
			{
				id: 'current-doc-local',
				file: 'module_local_decl_member_completion.nsf',
				category: 'current-doc local declaration',
				anchorText: 'batch.',
				deleteFrom: 'batch.',
				deleteTo: 'batch.mask02_clamp',
				expectedLabels: ['mask02_clamp', 'depth_bias'],
				mustPass: true
			},
			{
				id: 'vector-dot',
				file: 'module_vector_member_completion.nsf',
				category: 'vector/swizzle',
				anchorText: 'color.',
				deleteFrom: 'color.',
				deleteTo: 'color.x',
				expectedLabels: ['x', 'y'],
				mustPass: true
			},
			{
				id: 'texture-object',
				file: 'module_texture_member_completion.nsf',
				category: 'texture/object methods',
				anchorText: 'gTex.',
				deleteFrom: 'gTex.',
				deleteTo: 'gTex.Sample(gSampler, uv)',
				expectedLabels: ['Sample', 'Load'],
				mustPass: false
			},
			{
				id: 'texture-metadata',
				file: 'module_texture_metadata_member_completion.nsf',
				category: 'legacy texture metadata',
				anchorText: 't_mask02.',
				deleteFrom: 't_mask02.',
				deleteTo: 't_mask02.Sample(s_mask02, uv)',
				expectedLabels: ['Sample', 'Load'],
				mustPass: false
			},
			{
				id: 'texture-indexed',
				file: 'module_texture_indexed_member_completion.nsf',
				category: 'indexed base',
				anchorText: 'gTexArr[0].',
				deleteFrom: 'gTexArr[0].',
				deleteTo: 'gTexArr[0].Sample(gSampler, uv)',
				expectedLabels: ['Sample', 'Load'],
				mustPass: false
			},
			{
				id: 'texture-macro',
				file: 'module_texture_macro_member_completion.nsf',
				category: 'macro-wrapped base',
				anchorText: 'GET_TEX(gTex).',
				deleteFrom: 'GET_TEX(gTex).',
				deleteTo: 'GET_TEX(gTex).Sample(gSampler, uv)',
				expectedLabels: ['Sample', 'Load'],
				mustPass: false
			},
			{
				id: 'texture-spaced',
				file: 'module_texture_spaced_member_completion.nsf',
				category: 'spaced dot',
				anchorText: 'gTex . ',
				expectedLabels: ['Sample', 'Load'],
				mustPass: false
			}
		];

		const results = [];
		for (const testCase of cases) {
			results.push(await runCase(testCase));
		}
		writePerfReport('member-completion-matrix', {
			scenario: 'member-completion-matrix',
			results
		});

		for (const testCase of cases.filter((item) => item.mustPass)) {
			const result = results.find((item) => item.id === testCase.id);
			assert.ok(result?.passed, JSON.stringify(result));
		}
	});
});
