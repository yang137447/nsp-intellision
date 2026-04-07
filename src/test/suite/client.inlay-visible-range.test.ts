import * as assert from 'assert';
import * as vscode from 'vscode';

import { openFixture, repoDescribe, waitFor } from './test_helpers';

repoDescribe('NSF client integration: Inlay Visible Range', () => {
	it('provides builtin method and builtin function inlay hints inside nsf files', async function () {
		this.timeout(120000);
		await vscode.commands.executeCommand('nsf.restartServer');
		const document = await openFixture('main.nsf');
		const range = new vscode.Range(new vscode.Position(168, 0), new vscode.Position(214, 0));
		const hints = await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.InlayHint[]>(
					'vscode.executeInlayHintProvider',
					document.uri,
					range
				),
			(value) => Array.isArray(value) && value.length > 0,
			'nsf builtin/method inlay hints'
		);
		const labels = hints.map((hint) => `${hint.position.line}:${String(hint.label)}`);
		assert.ok(
			labels.includes('171:s:') && labels.includes('171:uv:'),
			`Expected Sample method hints in main.nsf, got ${labels.slice(0, 32).join(', ')}`
		);
		assert.ok(
			labels.includes('190:x:') && labels.includes('190:y:') && labels.includes('190:s:'),
			`Expected lerp builtin hints in main.nsf, got ${labels.slice(0, 48).join(', ')}`
		);
	});

	it('keeps the first automatic inlay request near the visible window budget on large nsf files', async function () {
		this.timeout(120000);
		await vscode.commands.executeCommand('nsf.restartServer');
		await vscode.commands.executeCommand('nsf._resetInternalStatus');
		const document = await openFixture('module_perf_large_current_doc.nsf');
		const editor = await waitFor(
			() => Promise.resolve(vscode.window.activeTextEditor),
			(value): value is vscode.TextEditor =>
				Boolean(value) && value.document.uri.toString() === document.uri.toString(),
			'active editor for large nsf auto-open range'
		);
		const status = await waitFor(
			() => vscode.commands.executeCommand<any>('nsf._getInternalStatus'),
			(value) =>
				value?.lastInlayRequestDocumentUri === document.uri.toString() &&
				(value?.inlayProviderRequestCount ?? 0) > 0,
			'first automatic inlay request after open'
		);
		const visibleEndLine = Math.max(...editor.visibleRanges.map((range) => range.end.line));
		assert.ok(
			(status?.lastInlayRequestEndLine ?? Number.POSITIVE_INFINITY) <= visibleEndLine + 24,
			`Expected the first auto inlay request to stay close to the visible window. visibleEnd=${visibleEndLine} actualEnd=${status?.lastInlayRequestEndLine}`
		);
	});

});
