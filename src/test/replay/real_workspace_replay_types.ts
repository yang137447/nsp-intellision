export type ReplayAnchor = {
	workspaceFolderSuffix: string;
	relativePath: string;
	relativePathAlternatives?: string[];
	anchorText: string;
	anchorTextAlternatives?: string[];
	occurrence?: number;
	characterOffset?: number;
};

export type ReplaySamplingWindow = {
	label: string;
	delaysMs?: number[];
	sampleCount?: number;
	sampleIntervalMs?: number;
	captureRuntimeDebug?: boolean;
	captureInteractiveDebug?: boolean;
};

export type ReplayTypingProbe = {
	label: string;
	category?: string;
	kind: 'completion' | 'signatureHelp' | 'diagnostics';
	afterText: string;
	triggerText?: string;
	occurrence?: number;
	payload?: {
		triggerCharacter?: string;
		retrigger?: boolean;
		nativeTrigger?: boolean;
		triggerSuggestUi?: boolean;
		completionUiMode?: 'nativeOnly' | 'explicitSuggest';
		triggerParameterHintsUi?: boolean;
		uiTriggerDelayMs?: number;
		expectedLabels?: string[];
		expectedSubstrings?: string[];
		maxLabels?: number;
		maxSignatures?: number;
		maxDiagnostics?: number;
		maxErrors?: number;
		waitForReadyMs?: number;
		requireRuntimeReady?: boolean;
		touchEveryMs?: number;
		maxTouches?: number;
	};
	samplingWindow?: ReplaySamplingWindow;
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
			kind: 'setActiveUnit';
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
			kind: 'typeDocumentFromDisk';
			label: string;
			payload?: {
				charactersPerEdit?: number;
				checkpointEveryLines?: number;
				checkpointSamplingDelaysMs?: number[];
				nativeTrigger?: boolean;
				triggerSuggestUi?: boolean;
				completionUiMode?: 'nativeOnly' | 'explicitSuggest';
				triggerParameterHintsUi?: boolean;
				captureInlayContinuity?: boolean;
				uiTriggerDelayMs?: number;
				probes?: ReplayTypingProbe[];
			};
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
	  }
	| {
			kind: 'captureCompletion';
			label: string;
			payload?: {
				triggerCharacter?: string;
				expectedLabels?: string[];
				maxLabels?: number;
				triggerSuggestUi?: boolean;
				completionUiMode?: 'nativeOnly' | 'explicitSuggest';
				uiTriggerDelayMs?: number;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'captureSignatureHelp';
			label: string;
			payload?: {
				triggerCharacter?: string;
				retrigger?: boolean;
				expectedSubstrings?: string[];
				maxSignatures?: number;
				triggerParameterHintsUi?: boolean;
				uiTriggerDelayMs?: number;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'captureHover';
			label: string;
			payload?: {
				expectedSubstrings?: string[];
				maxContents?: number;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'captureDefinition';
			label: string;
			payload?: {
				expectedUriSubstrings?: string[];
				minLocations?: number;
				maxLocations?: number;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'captureReferences';
			label: string;
			payload?: {
				expectedUriSubstrings?: string[];
				minLocations?: number;
				maxLocations?: number;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'captureDocumentSymbols';
			label: string;
			payload?: {
				expectedNames?: string[];
				maxNames?: number;
				waitForReadyMs?: number;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'captureInlayHints';
			label: string;
			payload?: {
				lineDeltaBefore?: number;
				lineDeltaAfter?: number;
				expectedLabels?: string[];
				minHints?: number;
				maxHints?: number;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'capturePrepareRename';
			label: string;
			payload?: {
				expectedPlaceholder?: string;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'captureRenameEdit';
			label: string;
			payload: {
				newName: string;
				minChanges?: number;
				maxChanges?: number;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'captureSemanticTokens';
			label: string;
			payload?: {
				minDataLength?: number;
				maxDataLength?: number;
				waitForReadyMs?: number;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'captureWorkspaceSymbols';
			label: string;
			payload: {
				query: string;
				expectedNames?: string[];
				maxNames?: number;
			};
			afterActionPauseMs?: number;
			samplingWindow?: ReplaySamplingWindow;
	  }
	| {
			kind: 'waitForInternalStatus';
			label: string;
			payload: {
				mode: 'idle' | 'quiescent';
				timeoutMs?: number;
			};
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
	baselineInternalStatus?: any;
	latestMetrics?: any;
	runtimeDebug?: any;
	interactiveDebug?: any;
};
