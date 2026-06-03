import * as path from 'path';
import { commands, ConfigurationTarget, ExtensionContext, Uri, workspace, window } from 'vscode';

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
export const PREPROCESSOR_MACROS_SEEDED_KEY = 'nsf.preprocessorMacrosSeeded';
export const PREPROCESSOR_MACROS_PRESET_MIGRATION_KEY = 'nsf.preprocessorMacrosPresetMigrationVersion';
export const PREPROCESSOR_MACROS_PRESET_MIGRATION_VERSION = 2;

const PREPROCESSOR_MACRO_PRESET_MIGRATION_MIN_CONFIGURED_COUNT = 50;
const PREPROCESSOR_MACRO_PRESET_MIGRATION_MIN_OVERLAP_RATIO = 0.7;
const PREPROCESSOR_MACRO_PRESET_MIGRATION_MAX_MISSING_RATIO = 0.25;

export type PreprocessorMacroPresetResponse = {
	entries?: Array<{
		name?: unknown;
		replacement?: unknown;
	}>;
};

export type SeedPreprocessorMacroOptions = {
	context: ExtensionContext;
	isTestMode: boolean;
	fetchPreset: () => Promise<PreprocessorMacroPresetResponse>;
	logClient: (message: string) => void;
};

export type PreprocessorMacroPresetCompletionResult = {
	eligible: boolean;
	shouldUpdate: boolean;
	reason: string;
	configuredCount: number;
	presetCount: number;
	overlapCount: number;
	missingCount: number;
	missingNames: string[];
	merged: Record<string, unknown>;
};

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

export function normalizeOptionalPath(value: string | undefined): string {
	const trimmed = (value ?? '').trim();
	return trimmed.length > 0 ? path.resolve(trimmed) : '';
}

export function readPreprocessorMacroSettings(): Record<string, unknown> {
	const inspected = workspace.getConfiguration('nsf').inspect<Record<string, unknown>>('preprocessorMacros');
	return (
		inspected?.workspaceFolderValue ??
		inspected?.workspaceValue ??
		inspected?.globalValue ??
		{}
	);
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
		shaderCompilerPath: normalizeOptionalPath(config.get<string>('shaderCompilerPath', '')),
		shaderFileExtensions: config.get<string[]>(
			'shaderFileExtensions',
			['.nsf', '.hlsl']
		),
		defines: config.get<string[]>('defines', []),
		preprocessorMacros: readPreprocessorMacroSettings(),
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

function hasExplicitPreprocessorMacroSetting(): boolean {
	return readExplicitPreprocessorMacroSetting() !== undefined;
}

function readExplicitPreprocessorMacroSetting():
	| { value: Record<string, unknown>; target: ConfigurationTarget; resource?: Uri }
	| undefined {
	for (const folder of workspace.workspaceFolders ?? []) {
		const inspected = workspace
			.getConfiguration('nsf', folder.uri)
			.inspect<Record<string, unknown>>('preprocessorMacros');
		if (
			inspected?.workspaceFolderValue &&
			typeof inspected.workspaceFolderValue === 'object' &&
			!Array.isArray(inspected.workspaceFolderValue)
		) {
			return {
				value: inspected.workspaceFolderValue,
				target: ConfigurationTarget.WorkspaceFolder,
				resource: folder.uri
			};
		}
	}
	const inspected = workspace.getConfiguration('nsf').inspect<Record<string, unknown>>('preprocessorMacros');
	const selected =
		inspected?.workspaceValue !== undefined
			? { value: inspected.workspaceValue, target: ConfigurationTarget.Workspace }
			: inspected?.globalValue !== undefined
				? { value: inspected.globalValue, target: ConfigurationTarget.Global }
				: undefined;
	if (!selected || !selected.value || typeof selected.value !== 'object' || Array.isArray(selected.value)) {
		return undefined;
	}
	return selected;
}

function isSupportedPreprocessorMacroReplacement(value: unknown): boolean {
	return typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean';
}

function normalizePreprocessorMacroPreset(
	response: PreprocessorMacroPresetResponse
): Record<string, string | number | boolean> {
	const macros: Record<string, string | number | boolean> = {};
	const entries = Array.isArray(response.entries) ? response.entries : [];
	for (const entry of entries) {
		const name = typeof entry?.name === 'string' ? entry.name.trim() : '';
		const replacement = entry?.replacement;
		if (
			name.length === 0 ||
			!(
				typeof replacement === 'string' ||
				typeof replacement === 'number' ||
				typeof replacement === 'boolean'
			)
		) {
			continue;
		}
		macros[name] = replacement;
	}
	return macros;
}

export function computePreprocessorMacroPresetCompletion(
	configured: Record<string, unknown>,
	preset: Record<string, string | number | boolean>
): PreprocessorMacroPresetCompletionResult {
	const configuredEntries = Object.entries(configured).filter(
		([name, value]) => name.trim().length > 0 && isSupportedPreprocessorMacroReplacement(value)
	);
	const presetEntries = Object.entries(preset).filter(
		([name, value]) => name.trim().length > 0 && isSupportedPreprocessorMacroReplacement(value)
	);
	const presetNames = new Set(presetEntries.map(([name]) => name));
	const configuredCount = configuredEntries.length;
	const presetCount = presetEntries.length;
	const overlapCount = configuredEntries.filter(([name]) => presetNames.has(name)).length;
	const missingEntries = presetEntries.filter(([name]) => !Object.prototype.hasOwnProperty.call(configured, name));
	const missingCount = missingEntries.length;
	const overlapRatio = configuredCount > 0 ? overlapCount / configuredCount : 0;
	const missingRatio = presetCount > 0 ? missingCount / presetCount : 1;
	const eligible =
		presetCount > 0 &&
		configuredCount >= PREPROCESSOR_MACRO_PRESET_MIGRATION_MIN_CONFIGURED_COUNT &&
		overlapCount >= PREPROCESSOR_MACRO_PRESET_MIGRATION_MIN_CONFIGURED_COUNT &&
		overlapRatio >= PREPROCESSOR_MACRO_PRESET_MIGRATION_MIN_OVERLAP_RATIO &&
		missingRatio <= PREPROCESSOR_MACRO_PRESET_MIGRATION_MAX_MISSING_RATIO;
	let reason = 'eligible';
	if (presetCount === 0) {
		reason = 'empty-preset';
	} else if (configuredCount < PREPROCESSOR_MACRO_PRESET_MIGRATION_MIN_CONFIGURED_COUNT) {
		reason = 'too-few-configured-macros';
	} else if (overlapCount < PREPROCESSOR_MACRO_PRESET_MIGRATION_MIN_CONFIGURED_COUNT) {
		reason = 'too-few-preset-overlap-macros';
	} else if (overlapRatio < PREPROCESSOR_MACRO_PRESET_MIGRATION_MIN_OVERLAP_RATIO) {
		reason = 'low-preset-overlap-ratio';
	} else if (missingRatio > PREPROCESSOR_MACRO_PRESET_MIGRATION_MAX_MISSING_RATIO) {
		reason = 'too-many-missing-preset-macros';
	}

	const merged: Record<string, unknown> = { ...configured };
	if (eligible) {
		for (const [name, value] of missingEntries) {
			merged[name] = value;
		}
	}
	return {
		eligible,
		shouldUpdate: eligible && missingCount > 0,
		reason,
		configuredCount,
		presetCount,
		overlapCount,
		missingCount,
		missingNames: missingEntries.map(([name]) => name),
		merged
	};
}

export async function seedPreprocessorMacrosSettingIfMissing(
	options: SeedPreprocessorMacroOptions
): Promise<boolean> {
	if (options.isTestMode) {
		return false;
	}
	if (!workspace.workspaceFolders || workspace.workspaceFolders.length === 0) {
		return false;
	}
	if (hasExplicitPreprocessorMacroSetting()) {
		await options.context.workspaceState.update(PREPROCESSOR_MACROS_SEEDED_KEY, true);
		if (
			options.context.workspaceState.get<number>(PREPROCESSOR_MACROS_PRESET_MIGRATION_KEY, 0) >=
			PREPROCESSOR_MACROS_PRESET_MIGRATION_VERSION
		) {
			return false;
		}
		const explicitSetting = readExplicitPreprocessorMacroSetting();
		if (!explicitSetting) {
			await options.context.workspaceState.update(
				PREPROCESSOR_MACROS_PRESET_MIGRATION_KEY,
				PREPROCESSOR_MACROS_PRESET_MIGRATION_VERSION
			);
			return false;
		}
		const preset = normalizePreprocessorMacroPreset(await options.fetchPreset());
		const completion = computePreprocessorMacroPresetCompletion(explicitSetting.value, preset);
		if (!completion.shouldUpdate) {
			await options.context.workspaceState.update(
				PREPROCESSOR_MACROS_PRESET_MIGRATION_KEY,
				PREPROCESSOR_MACROS_PRESET_MIGRATION_VERSION
			);
			if (!completion.eligible) {
				options.logClient(
					`skip nsf.preprocessorMacros preset completion: ${completion.reason} ` +
					`configured=${completion.configuredCount} preset=${completion.presetCount} overlap=${completion.overlapCount}`
				);
			}
			return false;
		}
		await workspace
			.getConfiguration('nsf', explicitSetting.resource)
			.update('preprocessorMacros', completion.merged, explicitSetting.target);
		await options.context.workspaceState.update(
			PREPROCESSOR_MACROS_PRESET_MIGRATION_KEY,
			PREPROCESSOR_MACROS_PRESET_MIGRATION_VERSION
		);
		const sample = completion.missingNames.slice(0, 12).join(', ');
		options.logClient(
			`completed old nsf.preprocessorMacros preset with ${completion.missingCount} missing entries` +
			(sample ? `: ${sample}${completion.missingCount > 12 ? ', ...' : ''}` : '')
		);
		return true;
	}
	if (
		options.context.workspaceState.get<number>(PREPROCESSOR_MACROS_PRESET_MIGRATION_KEY, 0) <
		PREPROCESSOR_MACROS_PRESET_MIGRATION_VERSION
	) {
		await options.context.workspaceState.update(
			PREPROCESSOR_MACROS_PRESET_MIGRATION_KEY,
			PREPROCESSOR_MACROS_PRESET_MIGRATION_VERSION
		);
	}
	if (options.context.workspaceState.get<boolean>(PREPROCESSOR_MACROS_SEEDED_KEY, false)) {
		return false;
	}

	const preset = normalizePreprocessorMacroPreset(await options.fetchPreset());
	if (Object.keys(preset).length === 0) {
		options.logClient('preprocessor macro preset is empty; workspace setting seed skipped');
		return false;
	}

	await workspace
		.getConfiguration('nsf')
		.update('preprocessorMacros', preset, ConfigurationTarget.Workspace);
	await options.context.workspaceState.update(PREPROCESSOR_MACROS_SEEDED_KEY, true);
	await options.context.workspaceState.update(
		PREPROCESSOR_MACROS_PRESET_MIGRATION_KEY,
		PREPROCESSOR_MACROS_PRESET_MIGRATION_VERSION
	);
	options.logClient(`seeded nsf.preprocessorMacros workspace setting with ${Object.keys(preset).length} entries`);
	return true;
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
