import * as assert from 'assert';
import * as vscode from 'vscode';

import {
	CompletionRequestCoordinator,
	type CompletionCoordinatorDecision
} from '../../../client/out/client_completion_request_coordinator';

type Deferred<T> = {
	promise: Promise<T>;
	resolve: (value: T) => void;
	reject: (error: unknown) => void;
};

function createDeferred<T>(): Deferred<T> {
	let resolve!: (value: T) => void;
	let reject!: (error: unknown) => void;
	const promise = new Promise<T>((promiseResolve, promiseReject) => {
		resolve = promiseResolve;
		reject = promiseReject;
	});
	return { promise, resolve, reject };
}

function wait(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

function documentWithText(text: string, uri = 'file:///completion-coordinator-test.nsf'): vscode.TextDocument {
	const lines = text.split(/\r?\n/);
	return {
		uri: vscode.Uri.parse(uri),
		version: 1,
		isDirty: true,
		lineAt: (lineOrPosition: number | vscode.Position) => ({
			text: lines[typeof lineOrPosition === 'number' ? lineOrPosition : lineOrPosition.line] ?? ''
		})
	} as vscode.TextDocument;
}

function token(cancelled = false): vscode.CancellationToken {
	return { isCancellationRequested: cancelled } as vscode.CancellationToken;
}

function request(
	text: string,
	triggerKind: vscode.CompletionTriggerKind,
	triggerCharacter?: string,
	positionCharacter = text.length,
	uri?: string
) {
	return {
		document: documentWithText(text, uri),
		position: new vscode.Position(0, positionCharacter),
		context: { triggerKind, triggerCharacter } as vscode.CompletionContext,
		token: token()
	};
}

function item(label: string): vscode.CompletionItem {
	return new vscode.CompletionItem(label);
}

describe('CompletionRequestCoordinator', () => {
	it('coalesces safe forward identifier-prefix requests before LSP starts', async () => {
		const coordinator = new CompletionRequestCoordinator();
		const firstDecisions: CompletionCoordinatorDecision[] = [];
		const secondDecisions: CompletionCoordinatorDecision[] = [];
		let executeCount = 0;

		const first = coordinator.coordinate(
			request('a', vscode.CompletionTriggerKind.TriggerCharacter, 'a'),
			() => {
				executeCount++;
				return [item('old')];
			},
			(decision) => firstDecisions.push(decision)
		);
		const second = coordinator.coordinate(
			request('ab', vscode.CompletionTriggerKind.Invoke),
			() => {
				executeCount++;
				return [item('latest')];
			},
			(decision) => secondDecisions.push(decision)
		);

		assert.strictEqual(await first, undefined);
		const result = await second;
		assert.strictEqual(Array.isArray(result) ? result[0].label : undefined, 'latest');
		assert.strictEqual(executeCount, 1);
		assert.strictEqual(firstDecisions[firstDecisions.length - 1]?.action, 'coalescedBeforeLsp');
		assert.strictEqual(secondDecisions[secondDecisions.length - 1]?.action, 'executed');
		const snapshot = coordinator.getSnapshot();
		assert.strictEqual(snapshot.completionCoordinatorReceivedCount, 2);
		assert.strictEqual(snapshot.completionCoordinatorExecutedCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorCoalescedBeforeLspCount, 1);
	});

	it('neutralizes older visible in-flight requests and detaches their LSP cleanup', async () => {
		const coordinator = new CompletionRequestCoordinator();
		const firstDecisions: CompletionCoordinatorDecision[] = [];
		const secondDecisions: CompletionCoordinatorDecision[] = [];
		const oldLsp = createDeferred<vscode.CompletionItem[]>();
		let oldStarted = false;
		let oldCancelled = false;

		const first = coordinator.coordinate(
			request('a', vscode.CompletionTriggerKind.TriggerCharacter, 'a'),
			(executionToken) => {
				oldStarted = true;
				executionToken.onCancellationRequested(() => {
					oldCancelled = true;
				});
				return oldLsp.promise;
			},
			(decision) => firstDecisions.push(decision)
		);
		await wait(55);
		assert.strictEqual(oldStarted, true);

		const second = coordinator.coordinate(
			request('ab', vscode.CompletionTriggerKind.TriggerCharacter, 'b'),
			() => [item('latest')],
			(decision) => secondDecisions.push(decision)
		);
		const staleResult = await Promise.race([
			first,
			wait(20).then(() => 'timeout' as const)
		]);
		assert.strictEqual(staleResult, undefined);
		assert.strictEqual(oldCancelled, true);
		assert.strictEqual(firstDecisions[firstDecisions.length - 1]?.action, 'staleResolvedWhileInFlight');

		const latestResult = await second;
		assert.strictEqual(Array.isArray(latestResult) ? latestResult[0].label : undefined, 'latest');
		oldLsp.resolve([item('old')]);
		await wait(0);

		const snapshot = coordinator.getSnapshot();
		assert.strictEqual(secondDecisions[secondDecisions.length - 1]?.action, 'executed');
		assert.strictEqual(snapshot.completionCoordinatorExecutedCount, 2);
		assert.strictEqual(snapshot.completionCoordinatorSupersededInFlightCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorNeutralResolvedWhileInFlightCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorStaleDroppedAfterLspCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorLatestRetainedWhileInFlightCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorDetachedLspCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorDetachedLspErrorCount, 0);
	});

	it('swallows detached stale LSP errors after the visible provider promise settled neutral', async () => {
		const coordinator = new CompletionRequestCoordinator();
		const oldLsp = createDeferred<vscode.CompletionItem[]>();
		const first = coordinator.coordinate(
			request('a', vscode.CompletionTriggerKind.TriggerCharacter, 'a'),
			() => oldLsp.promise,
			() => undefined
		);
		await wait(55);

		const second = coordinator.coordinate(
			request('ab', vscode.CompletionTriggerKind.TriggerCharacter, 'b'),
			() => [item('latest')],
			() => undefined
		);
		assert.strictEqual(await first, undefined);
		await second;
		oldLsp.reject(new Error('stale request failed after neutral resolve'));
		await wait(0);

		const snapshot = coordinator.getSnapshot();
		assert.strictEqual(snapshot.completionCoordinatorDetachedLspErrorCount, 1);
	});

	it('does not stale-neutralize an in-flight request when the new prefix shrinks', async () => {
		const coordinator = new CompletionRequestCoordinator();
		const firstDecisions: CompletionCoordinatorDecision[] = [];
		const oldLsp = createDeferred<vscode.CompletionItem[]>();
		let firstSettled = false;

		const first = coordinator.coordinate(
			request('abc', vscode.CompletionTriggerKind.TriggerCharacter, 'c'),
			() => oldLsp.promise,
			(decision) => firstDecisions.push(decision)
		);
		first.then(() => {
			firstSettled = true;
		});
		await wait(55);

		const second = coordinator.coordinate(
			request('ab', vscode.CompletionTriggerKind.TriggerCharacter, 'b'),
			() => [item('shorter')],
			() => undefined
		);
		await second;
		await wait(0);
		assert.strictEqual(firstSettled, false);
		assert.strictEqual(firstDecisions[firstDecisions.length - 1]?.action, 'executed');

		oldLsp.resolve([item('older-longer-prefix')]);
		const firstResult = await first;
		assert.strictEqual(Array.isArray(firstResult) ? firstResult[0].label : undefined, 'older-longer-prefix');
		const snapshot = coordinator.getSnapshot();
		assert.strictEqual(snapshot.completionCoordinatorSupersededInFlightCount, 0);
		assert.strictEqual(snapshot.completionCoordinatorNeutralResolvedWhileInFlightCount, 0);
		assert.strictEqual(snapshot.completionCoordinatorExecutedCount, 2);
	});

	it('cancels older visible auto-trigger requests when a new completion key appears', async () => {
		const coordinator = new CompletionRequestCoordinator();
		const firstDecisions: CompletionCoordinatorDecision[] = [];
		const oldLsp = createDeferred<vscode.CompletionItem[]>();
		let oldCancelled = false;

		const first = coordinator.coordinate(
			request('abc', vscode.CompletionTriggerKind.TriggerCharacter, 'c', 'abc'.length, 'file:///first-key.nsf'),
			(executionToken) => {
				executionToken.onCancellationRequested(() => {
					oldCancelled = true;
				});
				return oldLsp.promise;
			},
			(decision) => firstDecisions.push(decision)
		);
		await wait(55);

		const second = await coordinator.coordinate(
			request('other', vscode.CompletionTriggerKind.Invoke, undefined, 'other'.length, 'file:///second-key.nsf'),
			() => [item('explicit-other-key')],
			() => undefined
		);
		assert.strictEqual(Array.isArray(second) ? second[0].label : undefined, 'explicit-other-key');
		assert.strictEqual(await first, undefined);
		assert.strictEqual(oldCancelled, true);
		assert.strictEqual(firstDecisions[firstDecisions.length - 1]?.action, 'staleResolvedWhileInFlight');

		oldLsp.resolve([item('old')]);
		await wait(0);
		const snapshot = coordinator.getSnapshot();
		assert.strictEqual(snapshot.completionCoordinatorSupersededInFlightCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorNeutralResolvedWhileInFlightCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorDetachedLspCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorBypassedExplicitCount, 1);
	});

	it('bypasses explicit invoke and member completion requests', async () => {
		const coordinator = new CompletionRequestCoordinator();
		const explicitDecisions: CompletionCoordinatorDecision[] = [];
		const memberDecisions: CompletionCoordinatorDecision[] = [];

		const explicit = await coordinator.coordinate(
			request('abc', vscode.CompletionTriggerKind.Invoke),
			() => [item('explicit')],
			(decision) => explicitDecisions.push(decision)
		);
		const member = await coordinator.coordinate(
			request('foo.b', vscode.CompletionTriggerKind.TriggerCharacter, 'b'),
			() => [item('member')],
			(decision) => memberDecisions.push(decision)
		);

		assert.strictEqual(Array.isArray(explicit) ? explicit[0].label : undefined, 'explicit');
		assert.strictEqual(Array.isArray(member) ? member[0].label : undefined, 'member');
		assert.strictEqual(explicitDecisions[0]?.action, 'bypassedExplicit');
		assert.strictEqual(memberDecisions[0]?.action, 'bypassedMember');
		const snapshot = coordinator.getSnapshot();
		assert.strictEqual(snapshot.completionCoordinatorBypassedExplicitCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorBypassedMemberCount, 1);
		assert.strictEqual(snapshot.completionCoordinatorExecutedCount, 2);
	});
});
