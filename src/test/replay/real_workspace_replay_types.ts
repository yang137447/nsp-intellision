export type ReplayAnchor = {
	workspaceFolderSuffix: string;
	relativePath: string;
	anchorText: string;
	occurrence?: number;
	characterOffset?: number;
};

export type ReplaySamplingWindow = {
	label: string;
	delaysMs: number[];
	captureRuntimeDebug?: boolean;
	captureInteractiveDebug?: boolean;
};

export type ReplayStep =
	| {
			kind: 'openDocument';
			label: string;
			target: ReplayAnchor;
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'placeCursor';
			label: string;
			target: ReplayAnchor;
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'selectRange';
			label: string;
			target: ReplayAnchor;
			payload: { startOffset: number; endOffset: number };
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'typeText';
			label: string;
			payload: { text: string };
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'deleteLeft';
			label: string;
			payload?: { count?: number };
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'invokeCommand';
			label: string;
			payload: { command: string; args?: unknown[] };
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  };

export type ReplayScript = {
	id: string;
	title: string;
	workspaceHint: string;
	targetDocument: ReplayAnchor;
	intent: string;
	tags: string[];
	steps: ReplayStep[];
	cleanup?: { restoreTouchedDocuments: boolean };
};

export type ReplaySampleSnapshot = {
	offsetMs: number;
	internalStatus?: any;
	latestMetrics?: any;
	runtimeDebug?: any;
	interactiveDebug?: any;
};
