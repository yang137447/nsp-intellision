import * as assert from 'assert';
import * as path from 'path';
import * as vscode from 'vscode';

type Awaitable<T> = T | Thenable<T> | PromiseLike<T>;

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const realDescribe = testMode === 'real' ? describe : describe.skip;

function getWorkspaceFolderPath(folderSuffix: string): string {
	const folder = vscode.workspace.workspaceFolders?.find((item) =>
		item.uri.fsPath.replace(/\\/g, '/').toLowerCase().endsWith(folderSuffix.toLowerCase())
	);
	assert.ok(folder, `Expected workspace folder ending with '${folderSuffix}'.`);
	return folder.uri.fsPath;
}

async function openDocument(absolutePath: string): Promise<vscode.TextDocument> {
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
	const maxAttempts = testMode === 'real' ? 120 : 40;
	for (let attempt = 0; attempt < maxAttempts; attempt++) {
		lastValue = await Promise.resolve(producer());
		if (isReady(lastValue)) {
			return lastValue;
		}
		await new Promise((resolve) => setTimeout(resolve, 500));
	}
	throw new Error(`Timed out waiting for ${label}.`);
}

async function waitForIndexingIdle(label: string): Promise<void> {
	await waitFor(
		() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
		(value) => !value?.indexingState || value.indexingState.state === 'Idle',
		label
	);
}

function lineOf(document: vscode.TextDocument, needle: string, occurrence = 1): number {
	let fromIndex = 0;
	let foundIndex = -1;
	for (let current = 0; current < occurrence; current++) {
		foundIndex = document.getText().indexOf(needle, fromIndex);
		assert.notStrictEqual(foundIndex, -1, `Unable to find occurrence ${occurrence} of '${needle}'.`);
		fromIndex = foundIndex + needle.length;
	}
	return document.positionAt(foundIndex).line;
}

function lineOfAfter(document: vscode.TextDocument, needle: string, afterLine: number): number {
	const lines = document.getText().split(/\r?\n/);
	for (let index = Math.max(0, afterLine); index < lines.length; index++) {
		if (lines[index].includes(needle)) {
			return index;
		}
	}
	throw new Error(`Unable to find '${needle}' after line ${afterLine}.`);
}

realDescribe('NSF real workspace inlay', () => {
	it('refreshes automatic inlay requests after scrolling deeper into uber_fx_common.nsf', async function () {
		this.timeout(240000);

		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		const targetPath = path.join(shaderSourceRoot, 'sfx', 'uber_fx_common.nsf');
		const document = await openDocument(targetPath);
		await waitForIndexingIdle('real workspace indexing idle for uber_fx_common.nsf');
		await vscode.commands.executeCommand('nsf._resetInternalStatus');

		const editor = await waitFor(
			() => Promise.resolve(vscode.window.activeTextEditor),
			(value): value is vscode.TextEditor =>
				Boolean(value) && value.document.uri.toString() === document.uri.toString(),
			'active editor for uber_fx_common.nsf'
		);

		const targetLine = 318;
		editor.revealRange(
			new vscode.Range(new vscode.Position(targetLine, 0), new vscode.Position(targetLine, 0)),
			vscode.TextEditorRevealType.InCenter
		);

		const status = await waitFor(
			() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
			(value) =>
				value?.lastInlayRequestDocumentUri === document.uri.toString() &&
				(value?.lastInlayRequestEndLine ?? -1) >= targetLine,
			'real workspace scrolled inlay request'
		);

		assert.ok(
			(status?.lastInlayRequestEndLine ?? -1) >= targetLine,
			`Expected inlay refresh after scrolling to line ${targetLine}, got ${JSON.stringify(status)}`
		);
	});

	it('returns inlay hints around the lower visible block in uber_fx_common.nsf', async function () {
		this.timeout(240000);

		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		const targetPath = path.join(shaderSourceRoot, 'sfx', 'uber_fx_common.nsf');
		const document = await openDocument(targetPath);
		await waitForIndexingIdle('real workspace indexing idle for lower-block inlay');

		const startLine = lineOf(document, 'mask02UV = UV_TC_OS_NOCLAMP(');
		const endLine = lineOf(document, 'half mask02 = batch.mask02_channel ? mask02_rgba.a : mask02_rgba.r;');
		const range = new vscode.Range(new vscode.Position(startLine, 0), new vscode.Position(endLine + 1, 0));

		const hints = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.InlayHint[]>(
					'vscode.executeInlayHintProvider',
					document.uri,
					range
				),
			(value) => Array.isArray(value),
			'real workspace lower-block inlay hints'
		);
		const status = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
		const labels = (hints ?? []).map((hint) => `${hint.position.line}:${String(hint.label)}`);
		const sampleLine = lineOfAfter(document, 'distort02_offset = t_distort_tex2.Sample(', startLine);
		const lerpLine = lineOfAfter(document, 'addmaskdistort2UV.x = lerp(', startLine);
		const mask02SampleLine = lineOfAfter(document, 'half4 mask02_rgba = t_mask02.Sample(', startLine);

		assert.ok(
			labels.includes(`${startLine}:UV:`) &&
				labels.includes(`${startLine}:OS:`) &&
				labels.includes(`${startLine}:TC:`),
			`Expected UV_TC_OS_NOCLAMP hints, got ${labels.slice(0, 48).join(', ')}`
		);
		assert.ok(
			labels.includes(`${sampleLine}:s:`) && labels.includes(`${sampleLine}:uv:`),
			`Expected Sample hints near line ${sampleLine}, got ${labels.slice(0, 64).join(', ')} status=${JSON.stringify(status)}`
		);
		assert.ok(
			labels.includes(`${lerpLine}:x:`) &&
				labels.includes(`${lerpLine}:y:`) &&
				labels.includes(`${lerpLine}:s:`),
			`Expected lerp hints near line ${lerpLine}, got ${labels.slice(0, 80).join(', ')} status=${JSON.stringify(status)}`
		);
		assert.ok(
			labels.includes(`${mask02SampleLine}:s:`) && labels.includes(`${mask02SampleLine}:uv:`),
			`Expected lower Sample hints near line ${mask02SampleLine}, got ${labels.slice(0, 96).join(', ')} status=${JSON.stringify(status)}`
		);
	});
});
