import * as path from 'path';
import { commands, ExtensionContext, workspace, window } from 'vscode';

export type DiagnosticsMode = 'basic' | 'balanced' | 'full';

export type DiagnosticsRuntimeSettings = {
	mode: DiagnosticsMode;
	expensiveRules: boolean;
	indeterminate: {
		enabled: boolean;
		severity: number;
		maxItems: number;
		suppressWhenErrors: boolean;
	};
	timeBudgetMs: number;
	maxItems: number;
	workerCount: number;
	autoWorkerCount: boolean;
	fast: {
		enabled: boolean;
		delayMs: number;
		timeBudgetMs: number;
		maxItems: number;
	};
	full: {
		enabled: boolean;
		delayMs: number;
		expensiveRules: boolean;
		timeBudgetMs: number;
		maxItems: number;
	};
};

export const INTELLISION_PATH_PROMPT_DISMISSED_KEY = 'nsf.intellisionPathPromptDismissed';

export function resolveDiagnosticsMode(): DiagnosticsMode {
	const mode = workspace.getConfiguration('nsf').get<string>('diagnostics.mode', 'balanced');
	if (mode === 'basic' || mode === 'full') {
		return mode;
	}
	return 'balanced';
}

export function createDiagnosticsModeDefaults(mode: DiagnosticsMode): DiagnosticsRuntimeSettings {
	if (mode === 'basic') {
		return {
			mode,
			expensiveRules: false,
			indeterminate: {
				enabled: true,
				severity: 4,
				maxItems: 100,
				suppressWhenErrors: true
			},
			timeBudgetMs: 700,
			maxItems: 600,
			workerCount: 1,
			autoWorkerCount: true,
			fast: {
				enabled: true,
				delayMs: 120,
				timeBudgetMs: 120,
				maxItems: 120
			},
			full: {
				enabled: false,
				delayMs: 900,
				expensiveRules: false,
				timeBudgetMs: 700,
				maxItems: 600
			}
		};
	}
	if (mode === 'full') {
		return {
			mode,
			expensiveRules: true,
			indeterminate: {
				enabled: true,
				severity: 4,
				maxItems: 400,
				suppressWhenErrors: true
			},
			timeBudgetMs: 2000,
			maxItems: 2000,
			workerCount: 2,
			autoWorkerCount: true,
			fast: {
				enabled: true,
				delayMs: 60,
				timeBudgetMs: 240,
				maxItems: 400
			},
			full: {
				enabled: true,
				delayMs: 250,
				expensiveRules: true,
				timeBudgetMs: 2000,
				maxItems: 2000
			}
		};
	}
	return {
		mode,
		expensiveRules: true,
		indeterminate: {
			enabled: true,
			severity: 4,
			maxItems: 200,
			suppressWhenErrors: true
		},
		timeBudgetMs: 1200,
		maxItems: 1200,
		workerCount: 2,
		autoWorkerCount: true,
		fast: {
			enabled: true,
			delayMs: 90,
			timeBudgetMs: 180,
			maxItems: 240
		},
		full: {
			enabled: true,
			delayMs: 700,
			expensiveRules: true,
			timeBudgetMs: 1200,
			maxItems: 1200
		}
	};
}

export function clampDiagnosticsSeverity(value: number): number {
	return Math.min(4, Math.max(1, Math.trunc(value)));
}

export function readDiagnosticsRuntimeSettings(
	isTestMode: boolean,
	isPerfTestMode: boolean
): DiagnosticsRuntimeSettings {
	const defaults = createDiagnosticsModeDefaults(resolveDiagnosticsMode());
	return {
		mode: defaults.mode,
		expensiveRules: defaults.expensiveRules,
		indeterminate: {
			enabled: defaults.indeterminate.enabled,
			severity: clampDiagnosticsSeverity(defaults.indeterminate.severity),
			maxItems: Math.max(0, defaults.indeterminate.maxItems),
			suppressWhenErrors: defaults.indeterminate.suppressWhenErrors
		},
		timeBudgetMs: Math.max(defaults.timeBudgetMs, isTestMode ? 5000 : defaults.timeBudgetMs),
		maxItems: Math.max(defaults.maxItems, isTestMode ? 2400 : defaults.maxItems),
		workerCount: Math.max(1, defaults.workerCount),
		autoWorkerCount: defaults.autoWorkerCount,
		fast: {
			enabled: isTestMode ? isPerfTestMode : defaults.fast.enabled,
			delayMs: Math.max(0, defaults.fast.delayMs),
			timeBudgetMs: Math.max(defaults.fast.timeBudgetMs, 60),
			maxItems: Math.max(defaults.fast.maxItems, 60)
		},
		full: {
			enabled: defaults.full.enabled,
			delayMs: Math.max(0, defaults.full.delayMs),
			expensiveRules: defaults.full.expensiveRules,
			timeBudgetMs: Math.max(defaults.full.timeBudgetMs, isTestMode ? 5000 : defaults.full.timeBudgetMs),
			maxItems: Math.max(defaults.full.maxItems, isTestMode ? 2400 : defaults.full.maxItems)
		}
	};
}

export function normalizeIncludePaths(paths: string[]): string[] {
	const resolved = paths
		.map((item) => item.trim())
		.filter((item) => item.length > 0)
		.map((item) => path.resolve(item));
	const deduped: string[] = [];
	const seen = new Set<string>();
	for (const item of resolved) {
		const key = item.toLowerCase();
		if (seen.has(key)) {
			continue;
		}
		seen.add(key);
		deduped.push(item);
	}
	const kept: string[] = [];
	for (const candidate of deduped) {
		const candidateKey = candidate.toLowerCase();
		let nested = false;
		for (const parent of deduped) {
			if (parent === candidate) {
				continue;
			}
			const parentKey = parent.toLowerCase();
			const relative = path.relative(parentKey, candidateKey);
			if (!relative || (!relative.startsWith('..') && !path.isAbsolute(relative))) {
				nested = true;
				break;
			}
		}
		if (!nested) {
			kept.push(candidate);
		}
	}
	return kept;
}

export function buildRuntimeSettings(
	isTestMode: boolean,
	isPerfTestMode: boolean,
	includePathsOverride?: string[]
) {
	const config = workspace.getConfiguration('nsf');
	return {
		intellisionPath:
			includePathsOverride ?? normalizeIncludePaths(config.get<string[]>('intellisionPath', [])),
		shaderFileExtensions: config.get<string[]>(
			'shaderFileExtensions',
			['.nsf', '.hlsl', '.hlsli', '.fx', '.usf', '.ush']
		),
		defines: config.get<string[]>('defines', []),
		debugDefinitionTrace: false,
		inlayHints: {
			enabled: config.get<boolean>('inlayHints.enabled', true),
			parameterNames: config.get<boolean>('inlayHints.parameterNames', true),
			slowPath: true
		},
		semanticTokens: {
			enabled: config.get<boolean>('semanticTokens.enabled', true)
		},
		semanticCache: {
			enabled: true,
			shadowCompare: {
				enabled: false
			}
		},
		overloadResolver: {
			enabled: false,
			shadowCompare: {
				enabled: false
			}
		},
		diagnostics: readDiagnosticsRuntimeSettings(isTestMode, isPerfTestMode),
		metrics: {
			enabled: !isTestMode
		},
		indexing: {
			workerCount: 16,
			queueCapacity: 4096
		}
	};
}

export async function promptIntellisionPathIfMissing(
	context: ExtensionContext,
	isTestMode: boolean
): Promise<void> {
	if (isTestMode) {
		return;
	}
	const config = workspace.getConfiguration('nsf');
	const configured = normalizeIncludePaths(config.get<string[]>('intellisionPath', []));
	if (configured.length > 0) {
		await context.workspaceState.update(INTELLISION_PATH_PROMPT_DISMISSED_KEY, false);
		return;
	}
	const dismissed = context.workspaceState.get<boolean>(INTELLISION_PATH_PROMPT_DISMISSED_KEY, false);
	if (dismissed) {
		return;
	}
	const openSettingsAction = '打开设置';
	const dismissAction = '稍后';
	const pick = await window.showWarningMessage(
		'NSF: 未配置 nsf.intellisionPath。请填写 Shader 根目录，否则索引与 include-context 功能不可用。',
		openSettingsAction,
		dismissAction
	);
	if (pick === openSettingsAction) {
		await commands.executeCommand('workbench.action.openSettings', 'nsf.intellisionPath');
		return;
	}
	if (pick === dismissAction) {
		await context.workspaceState.update(INTELLISION_PATH_PROMPT_DISMISSED_KEY, true);
	}
}
