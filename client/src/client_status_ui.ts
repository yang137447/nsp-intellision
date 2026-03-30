import { LSP_METHOD_KEYS } from './lsp_method_keys';
import { formatIndexingProgress, hasServerIndexingWork, type ServerIndexingState } from './client_indexing_status';

export type ClientStatusLabel = 'stopped' | 'starting' | 'ready' | 'error';

export type IndexingEventSnapshot = {
	kind: string;
	state: string;
	phase?: string;
	symbol?: string;
	token?: number;
};

export type MainStatusBarInputs = {
	clientStateLabel: ClientStatusLabel;
	activeIndexingCount: number;
	lastIndexingState: ServerIndexingState | undefined;
	indexingMessage: string;
	indexingUnit: string;
	activeRpcCount: number;
	lastRpcMethod: string;
	latestMetricsSummary: string;
	lastIndexingEvent: IndexingEventSnapshot | undefined;
};

export type StatusBarPresentation = {
	text: string;
	tooltip: string;
};

export function getRpcShortLabel(method: string): string {
	switch (method) {
		case LSP_METHOD_KEYS.initialize:
			return 'Init';
		case LSP_METHOD_KEYS.shutdown:
			return 'Shutdown';
		case LSP_METHOD_KEYS.hover:
			return 'Hover';
		case LSP_METHOD_KEYS.definition:
			return 'Def';
		case LSP_METHOD_KEYS.references:
			return 'Refs';
		case LSP_METHOD_KEYS.documentHighlight:
			return 'HL';
		case LSP_METHOD_KEYS.completion:
			return 'Comp';
		case LSP_METHOD_KEYS.signatureHelp:
			return 'Sig';
		case LSP_METHOD_KEYS.documentSymbol:
			return 'Symbols';
		case LSP_METHOD_KEYS.workspaceSymbol:
			return 'WS Sym';
		case LSP_METHOD_KEYS.publishDiagnostics:
			return 'Diag';
		case LSP_METHOD_KEYS.inlayHint:
			return 'Inlay';
		case LSP_METHOD_KEYS.didChangeConfiguration:
			return 'Config';
		case LSP_METHOD_KEYS.setActiveUnit:
			return 'Unit';
		case LSP_METHOD_KEYS.rebuildIndex:
			return 'Index';
		default: {
			if (!method) {
				return '';
			}
			const slash = method.lastIndexOf('/');
			const tail = slash >= 0 ? method.slice(slash + 1) : method;
			if (tail.length <= 10) {
				return tail;
			}
			return tail.slice(0, 10);
		}
	}
}

export function buildMainStatusBarPresentation(inputs: MainStatusBarInputs): StatusBarPresentation {
	const indexingActive =
		inputs.activeIndexingCount > 0 || hasServerIndexingWork(inputs.lastIndexingState);
	const stateLabel = inputs.lastIndexingState?.state ?? '';
	const phaseLabel = inputs.lastIndexingState?.progress?.phase ?? '';
	const stateProgress = formatIndexingProgress(inputs.lastIndexingState);
	const stateMessage = [phaseLabel || stateLabel, stateProgress]
		.filter((item) => item && item.length > 0)
		.join(' ');

	let text = 'NSF';
	if (inputs.clientStateLabel === 'starting') {
		text = '$(sync~spin) NSF: Starting';
	} else if (inputs.clientStateLabel === 'error') {
		text = '$(error) NSF: Error';
	} else if (indexingActive) {
		const message = stateMessage || inputs.indexingMessage;
		text = message ? `$(sync~spin) NSF: Indexing ${message}` : '$(sync~spin) NSF: Indexing';
	} else if (inputs.activeRpcCount > 0) {
		const short = getRpcShortLabel(inputs.lastRpcMethod);
		const suffix = short ? ` ${short}` : '';
		text = `$(sync~spin) NSF: Working${suffix}`;
	} else if (inputs.clientStateLabel === 'ready') {
		text = '$(check) NSF';
	}

	let tooltip = 'NSF 语言服务';
	if (indexingActive) {
		const message = stateMessage || inputs.indexingMessage;
		const unit = inputs.indexingUnit ? `\n单位：${inputs.indexingUnit}` : '';
		const reason = inputs.lastIndexingState?.reason ? `\n原因：${inputs.lastIndexingState.reason}` : '';
		const queued = inputs.lastIndexingState?.pending?.queuedTasks ?? 0;
		const running = inputs.lastIndexingState?.pending?.runningWorkers ?? 0;
		const limits =
			typeof inputs.lastIndexingState?.limits?.workerCount === 'number' &&
			typeof inputs.lastIndexingState?.limits?.queueCapacity === 'number'
				? `\n并发：${inputs.lastIndexingState.limits.workerCount}，队列：${inputs.lastIndexingState.limits.queueCapacity}`
				: '';
		const pending = `\n排队：${queued}，运行：${running}`;
		tooltip = (message ? `正在索引：${message}` : '正在索引') + pending + limits + reason + unit;
	} else if (inputs.activeRpcCount > 0) {
		const short = getRpcShortLabel(inputs.lastRpcMethod);
		const label = short ? `\n标签：${short}` : '';
		const method = inputs.lastRpcMethod ? `\n方法：${inputs.lastRpcMethod}` : '';
		tooltip = `正在与服务端通信（${inputs.activeRpcCount}）` + label + method;
	} else if (inputs.clientStateLabel === 'ready') {
		tooltip = inputs.latestMetricsSummary
			? `NSF 语言服务已就绪\n${inputs.latestMetricsSummary}`
			: 'NSF 语言服务已就绪';
	} else if (inputs.clientStateLabel === 'starting') {
		tooltip = 'NSF 语言服务启动中';
	} else if (inputs.clientStateLabel === 'error') {
		tooltip = 'NSF 语言服务错误（点击查看状态）';
	} else if (inputs.lastIndexingEvent) {
		const extra = inputs.lastIndexingEvent.symbol
			? `${inputs.lastIndexingEvent.kind} ${inputs.lastIndexingEvent.symbol}`
			: inputs.lastIndexingEvent.kind;
		tooltip = `最近索引：${extra}`;
	}

	return { text, tooltip };
}
