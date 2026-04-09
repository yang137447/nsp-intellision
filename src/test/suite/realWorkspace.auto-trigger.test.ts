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
	for (let attempt = 0; attempt < 120; attempt++) {
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

function positionOf(
	document: vscode.TextDocument,
	needle: string,
	occurrence = 1,
	characterOffset = 0
): vscode.Position {
	let fromIndex = 0;
	let foundIndex = -1;
	for (let current = 0; current < occurrence; current++) {
		foundIndex = document.getText().indexOf(needle, fromIndex);
		assert.notStrictEqual(foundIndex, -1, `Unable to find occurrence ${occurrence} of '${needle}'.`);
		fromIndex = foundIndex + needle.length;
	}
	return document.positionAt(foundIndex + characterOffset);
}

function rangeFromNeedles(
	document: vscode.TextDocument,
	needle: string,
	startCharacterOffset: number,
	endCharacterOffset: number,
	occurrence = 1
): vscode.Range {
	const start = positionOf(document, needle, occurrence, startCharacterOffset);
	const end = positionOf(document, needle, occurrence, endCharacterOffset);
	return new vscode.Range(start, end);
}

type InternalStatusWithInteractiveCounts = {
	completionRequestCount?: number;
	signatureHelpRequestCount?: number;
};

realDescribe('NSF real workspace auto trigger', () => {
	it('issues completion requests while typing function prefixes in uber_fx_common.nsf', async function () {
		this.timeout(240000);

		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		const targetPath = path.join(shaderSourceRoot, 'sfx', 'uber_fx_common.nsf');
		const document = await openDocument(targetPath);
		await waitForIndexingIdle('real workspace auto-trigger completion indexing idle');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'real workspace auto-trigger completion internal commands ready'
		);

		const editor = await vscode.window.showTextDocument(document, { preview: false });
		const replaceRange = rangeFromNeedles(
			document,
			'diffuseUV = Pixelate',
			'diffuseUV = '.length,
			'diffuseUV = Pixelate'.length
		);
		const deletedText = document.getText(replaceRange);
		await editor.edit((editBuilder) => {
			editBuilder.delete(replaceRange);
		});
		editor.selection = new vscode.Selection(replaceRange.start, replaceRange.start);

		try {
			await vscode.commands.executeCommand('nsf._resetInternalStatus');
			for (const ch of ['P', 'i', 'x', 'e']) {
				await vscode.commands.executeCommand('type', { text: ch });
			}

			const status = await waitFor(
				() =>
					vscode.commands.executeCommand<InternalStatusWithInteractiveCounts>(
						'nsf._getInternalStatus'
					),
				(value) => (value?.completionRequestCount ?? 0) > 0,
				'real workspace auto-trigger completion request count'
			);
			assert.ok((status?.completionRequestCount ?? 0) > 0, JSON.stringify(status));
		} finally {
			await editor.edit((editBuilder) => {
				editBuilder.replace(
					new vscode.Range(replaceRange.start, replaceRange.start.translate(0, 4)),
					deletedText
				);
			});
		}
	});

	it('issues signature-help requests when typing opening paren in uber_fx_common.nsf', async function () {
		this.timeout(240000);

		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		const targetPath = path.join(shaderSourceRoot, 'sfx', 'uber_fx_common.nsf');
		const document = await openDocument(targetPath);
		await waitForIndexingIdle('real workspace auto-trigger signature-help indexing idle');
		await waitFor(
			() => vscode.commands.getCommands(true),
			(value) => Array.isArray(value) && value.includes('nsf._resetInternalStatus'),
			'real workspace auto-trigger signature-help internal commands ready'
		);

		const editor = await vscode.window.showTextDocument(document, { preview: false });
		const replaceRange = rangeFromNeedles(
			document,
			'diffuseUV = Pixelate(diffuseUV, batch.maintex_pixel_size);',
			'diffuseUV = Pixelate'.length,
			'diffuseUV = Pixelate(diffuseUV, batch.maintex_pixel_size)'.length
		);
		const deletedText = document.getText(replaceRange);
		await editor.edit((editBuilder) => {
			editBuilder.delete(replaceRange);
		});
		editor.selection = new vscode.Selection(replaceRange.start, replaceRange.start);

		try {
			await vscode.commands.executeCommand('nsf._resetInternalStatus');
			await vscode.commands.executeCommand('type', { text: '(' });

			const status = await waitFor(
				() =>
					vscode.commands.executeCommand<InternalStatusWithInteractiveCounts>(
						'nsf._getInternalStatus'
					),
				(value) => (value?.signatureHelpRequestCount ?? 0) > 0,
				'real workspace auto-trigger signature-help request count'
			);
			assert.ok((status?.signatureHelpRequestCount ?? 0) > 0, JSON.stringify(status));
		} finally {
			await editor.edit((editBuilder) => {
				editBuilder.replace(
					new vscode.Range(replaceRange.start, replaceRange.start.translate(0, 1)),
					deletedText
				);
			});
		}
	});
});
