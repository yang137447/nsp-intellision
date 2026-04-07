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

async function waitForClientQuiescent(label: string): Promise<void> {
	await waitFor(
		() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
		(value) => (value?.activeRpcCount ?? 0) === 0,
		label
	);
}

async function waitForNextMetricsRevision(
	revision: number,
	label: string
): Promise<LatestMetricsResponse> {
	return waitFor(
		() => vscode.commands.executeCommand<LatestMetricsResponse>('nsf._getLatestMetrics'),
		(value) => (value?.revision ?? 0) > revision,
		label
	);
}

function positionOf(document: vscode.TextDocument, needle: string, occurrence = 1, characterOffset = 0): vscode.Position {
	let fromIndex = 0;
	let foundIndex = -1;
	for (let current = 0; current < occurrence; current++) {
		foundIndex = document.getText().indexOf(needle, fromIndex);
		assert.notStrictEqual(foundIndex, -1, `Unable to find occurrence ${occurrence} of '${needle}'.`);
		fromIndex = foundIndex + needle.length;
	}
	return document.positionAt(foundIndex + characterOffset);
}

function getCompletionItems(result: vscode.CompletionList | vscode.CompletionItem[] | undefined): vscode.CompletionItem[] {
	if (!result) {
		return [];
	}
	return Array.isArray(result) ? result : result.items;
}

type LastCompletionDebugResponse = {
	memberAccessDetected?: boolean;
	base?: string;
	member?: string;
	memberTypeResolved?: boolean;
	resolvedType?: string;
	memberItemsReturned?: boolean;
	fieldCount?: number;
	methodCount?: number;
	path?: string;
};

type MemberCompletionDebugResponse = {
	memberAccessDetected?: boolean;
	base?: string;
	member?: string;
	declarationType?: string;
	resolvedType?: string;
	resolved?: boolean;
	fieldCount?: number;
	methodCount?: number;
	fields?: string[];
	methods?: string[];
};

type RuntimeDebugResponse = {
	documents?: Array<Record<string, unknown>>;
};

type LatestMetricsResponse = {
	summary?: string;
	payload?: unknown;
	revision?: number;
	receivedAtMs?: number;
};

realDescribe('NSF real workspace member completion', () => {
	it('serves float-vector dot completion on the first request after editing uber_fx_common.nsf', async function () {
		this.timeout(240000);

		const shaderSourceRoot = getWorkspaceFolderPath('shader-source');
		const targetPath = path.join(shaderSourceRoot, 'sfx', 'uber_fx_common.nsf');
		const document = await openDocument(targetPath);
		await waitForIndexingIdle('real workspace indexing idle for vector-dot request');
		await waitForClientQuiescent('real workspace client quiescent for vector-dot request');
		const source = 'addmaskdistort2UV.x';
		const sourcePosition = positionOf(document, source, 1, 0);
		const deleteRange = new vscode.Range(
			sourcePosition.translate(0, 'addmaskdistort2UV.'.length),
			sourcePosition.translate(0, source.length)
		);
		const revertText = document.getText(deleteRange);
		const edit = new vscode.WorkspaceEdit();
		edit.delete(document.uri, deleteRange);
		await vscode.workspace.applyEdit(edit);
		try {
			const updatedDocument = await vscode.workspace.openTextDocument(document.uri);
			const position = positionOf(updatedDocument, 'addmaskdistort2UV.', 1, 'addmaskdistort2UV.'.length);
			const startedAt = process.hrtime.bigint();
			const result = await vscode.commands.executeCommand<vscode.CompletionList | vscode.CompletionItem[]>(
				'vscode.executeCompletionItemProvider',
				updatedDocument.uri,
				position
			);
			const elapsedMs = Number(process.hrtime.bigint() - startedAt) / 1_000_000;
			const labels = new Set(getCompletionItems(result).map((item) => item.label.toString()));
			const completionDebug = await vscode.commands.executeCommand<LastCompletionDebugResponse>(
				'nsf._getLastCompletionDebug'
			);
			const memberDebug = await vscode.commands.executeCommand<MemberCompletionDebugResponse>(
				'nsf._debugMemberCompletion',
				{
					uri: updatedDocument.uri.toString(),
					line: position.line,
					character: position.character
				}
			);
			const runtimeDebug = await vscode.commands.executeCommand<RuntimeDebugResponse>(
				'nsf._getDocumentRuntimeDebug',
				{
					uris: [updatedDocument.uri.toString()]
				}
			);
			const internalStatus = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
			const metricsBefore = await vscode.commands.executeCommand<LatestMetricsResponse>(
				'nsf._getLatestMetrics'
			);
			const latestMetrics =
				process.env.NSF_LOG_REAL_MEMBER_DEBUG === '1'
					? await waitForNextMetricsRevision(
							metricsBefore?.revision ?? 0,
							'real workspace member completion metrics flush'
					  )
					: metricsBefore;
			const debugSummary = JSON.stringify({
				elapsedMs,
				completionDebug,
				memberDebug,
				internalStatus,
				runtimeDebug,
				latestMetrics,
				firstLabels: Array.from(labels).slice(0, 32)
			});
			if (process.env.NSF_LOG_REAL_MEMBER_DEBUG === '1') {
				console.log(`[real-member] ${debugSummary}`);
			}
			assert.ok(labels.has('x'), debugSummary);
			assert.ok(labels.has('y'), debugSummary);
			void elapsedMs;
		} finally {
			const restore = new vscode.WorkspaceEdit();
			restore.insert(document.uri, deleteRange.start, revertText);
			await vscode.workspace.applyEdit(restore);
		}
	});
});
