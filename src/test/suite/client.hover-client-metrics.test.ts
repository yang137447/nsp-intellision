import * as assert from 'assert';
import * as vscode from 'vscode';

import { openFixture, positionOf, repoDescribe, waitFor } from './test_helpers';

type InternalStatusWithHoverClientMetrics = {
	hoverRequestCount?: number;
	hoverRequestAvgMs?: number;
	hoverRequestMaxMs?: number;
	hoverRpcRequestCount?: number;
	hoverRpcRequestAvgMs?: number;
	hoverRpcRequestMaxMs?: number;
	hoverCode2ProtocolSamples?: number;
	hoverCode2ProtocolAvgMs?: number;
	hoverCode2ProtocolMaxMs?: number;
	hoverProtocolRequestSamples?: number;
	hoverProtocolRequestAvgMs?: number;
	hoverProtocolRequestMaxMs?: number;
	hoverProtocol2CodeSamples?: number;
	hoverProtocol2CodeAvgMs?: number;
	hoverProtocol2CodeMaxMs?: number;
	hoverExtensionHostOverheadAvgMs?: number;
	hoverExtensionHostOverheadMaxMs?: number;
};

repoDescribe('NSF client integration: Hover Client Metrics', () => {
	it('records client-side hover request timing through middleware', async function () {
		this.timeout(90000);

		await openFixture('module_suite.nsf');
		await vscode.commands.executeCommand('nsf._resetInternalStatus');
		const document = await openFixture('module_hover_docs.nsf');
		const hoverPosition = positionOf(document, 'SuiteHoverVar', 3, 2);

		await waitFor(
			() =>
				vscode.commands.executeCommand<vscode.Hover[]>(
					'vscode.executeHoverProvider',
					document.uri,
					hoverPosition
				),
			(value) => Array.isArray(value) && value.length > 0,
			'hover client metrics warmup'
		);

		const status = await waitFor(
			() => vscode.commands.executeCommand<InternalStatusWithHoverClientMetrics>('nsf._getInternalStatus'),
			(value) =>
				(value?.hoverRequestCount ?? 0) > 0 &&
				(value?.hoverRequestAvgMs ?? -1) >= 0 &&
				(value?.hoverRequestMaxMs ?? -1) >= 0 &&
				(value?.hoverRpcRequestCount ?? 0) > 0 &&
				(value?.hoverRpcRequestAvgMs ?? -1) >= 0 &&
				(value?.hoverRpcRequestMaxMs ?? -1) >= 0 &&
				(value?.hoverCode2ProtocolSamples ?? 0) > 0 &&
				(value?.hoverCode2ProtocolAvgMs ?? -1) >= 0 &&
				(value?.hoverCode2ProtocolMaxMs ?? -1) >= 0 &&
				(value?.hoverProtocolRequestSamples ?? 0) > 0 &&
				(value?.hoverProtocolRequestAvgMs ?? -1) >= 0 &&
				(value?.hoverProtocolRequestMaxMs ?? -1) >= 0 &&
				(value?.hoverProtocol2CodeSamples ?? 0) > 0 &&
				(value?.hoverProtocol2CodeAvgMs ?? -1) >= 0 &&
				(value?.hoverProtocol2CodeMaxMs ?? -1) >= 0 &&
				(value?.hoverExtensionHostOverheadAvgMs ?? -1) >= 0 &&
				(value?.hoverExtensionHostOverheadMaxMs ?? -1) >= 0,
			'hover client metrics'
		);

		assert.ok((status?.hoverRequestCount ?? 0) > 0);
		assert.ok((status?.hoverRequestAvgMs ?? -1) >= 0);
		assert.ok((status?.hoverRequestMaxMs ?? -1) >= 0);
		assert.ok((status?.hoverRpcRequestCount ?? 0) > 0);
		assert.ok((status?.hoverRpcRequestAvgMs ?? -1) >= 0);
		assert.ok((status?.hoverRpcRequestMaxMs ?? -1) >= 0);
		assert.ok((status?.hoverCode2ProtocolSamples ?? 0) > 0);
		assert.ok((status?.hoverCode2ProtocolAvgMs ?? -1) >= 0);
		assert.ok((status?.hoverCode2ProtocolMaxMs ?? -1) >= 0);
		assert.ok((status?.hoverProtocolRequestSamples ?? 0) > 0);
		assert.ok((status?.hoverProtocolRequestAvgMs ?? -1) >= 0);
		assert.ok((status?.hoverProtocolRequestMaxMs ?? -1) >= 0);
		assert.ok((status?.hoverProtocol2CodeSamples ?? 0) > 0);
		assert.ok((status?.hoverProtocol2CodeAvgMs ?? -1) >= 0);
		assert.ok((status?.hoverProtocol2CodeMaxMs ?? -1) >= 0);
		assert.ok((status?.hoverExtensionHostOverheadAvgMs ?? -1) >= 0);
		assert.ok((status?.hoverExtensionHostOverheadMaxMs ?? -1) >= 0);
	});
});
