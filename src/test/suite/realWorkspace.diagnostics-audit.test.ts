import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

type RuntimeDebugResponse = {
	documents?: Array<{
		uri?: string;
		exists?: boolean;
		activeUnitPath?: string;
		activeUnitProfileDefines?: Record<string, number>;
		activeUnitProfileShaderKey?: string;
		activeUnitProfileSourcePath?: string;
		activeUnitProfileSourceKind?: string;
		activeUnitProfileTotalRowCount?: number;
		activeUnitProfileSelectedRowCount?: number;
		activeUnitProfileSelectedRowSignature?: string;
		activeUnitProfileSelectionHintSourcePath?: string;
		activeUnitProfileUnresolvedMacros?: string[];
		activeUnitArtDefaultZeroDefines?: Record<string, number>;
		activeUnitArtDefaultZeroMacros?: Array<{
			name?: string;
			artType?: string;
			uri?: string;
			line?: number;
		}>;
		deferredHasFullDiagnostics?: boolean;
		lastDiagnosticsPublishLayer?: string;
	}>;
};

type RuntimeDebugDocument = NonNullable<RuntimeDebugResponse['documents']>[number];

type PreprocessorMacroPresetResponse = {
	entries?: Array<{
		name?: unknown;
		replacement?: unknown;
	}>;
};

type AuditDiagnostic = {
	unit: string;
	unitRelativePath: string;
	file: string;
	workspaceFolder: string;
	relativePath: string;
	extension: string;
	activeUnit?: string;
	severity: string;
	source: string;
	code: string;
	message: string;
	canonicalMessage: string;
	category: string;
	triage: string;
	line: number;
	character: number;
	lineText: string;
	nextLineText: string;
	reasonCode?: string;
};

type GroupSummary = {
	key: string;
	count: number;
	category: string;
	triage: string;
	samples: AuditDiagnostic[];
};

type UndefinedMacroOwner =
	| 'compiler-context-platform-quality'
	| 'enum-like-stable-constant'
	| 'selector-profile-macro'
	| 'source-generated-config';

type UndefinedMacroOwnerHint = {
	owner: UndefinedMacroOwner;
	sourceBoundary: string;
	defaultValue: string;
	risk: string;
	reason: string;
};

type UndefinedMacroHistogramEntry = UndefinedMacroOwnerHint & {
	macro: string;
	count: number;
	affectedUnitCount: number;
	affectedFileCount: number;
	samples: AuditDiagnostic[];
};

type UndefinedMacroOwnerSummary = {
	owner: UndefinedMacroOwner;
	macros: number;
	diagnostics: number;
};

type UndefinedMacroHistogram = {
	totalDiagnostics: number;
	macroCount: number;
	byOwner: UndefinedMacroOwnerSummary[];
	entries: UndefinedMacroHistogramEntry[];
};

type P14FocusMacroSummaryEntry = UndefinedMacroOwnerHint & {
	macro: string;
	diagnostics: number;
	affectedUnitCount: number;
	affectedFileCount: number;
	samples: AuditDiagnostic[];
	profileStatusSummary: Record<string, number>;
	profileEvidence: P14FocusProfileEvidenceEntry[];
};

type P14FocusProfileEvidenceEntry = {
	macro: string;
	unit: string;
	unitRelativePath: string;
	status: string;
	injectedValue?: number;
	activeUnitPath?: string;
	profileShaderKey?: string;
	profileSourcePath?: string;
	profileSourceKind?: string;
	profileTotalRowCount?: number;
	profileSelectedRowCount?: number;
	profileSelectedRowSignature?: string;
	profileSelectionHintSourcePath?: string;
	artDefaultZero?: boolean;
	artDefaultSourcePath?: string;
	artDefaultSourceLine?: number;
	artDefaultType?: string;
	unresolvedMacros: string[];
};

type P14CompilerContextProfileEvidenceEntry = {
	macro: string;
	unit: string;
	unitRelativePath: string;
	status: string;
	injectedValue?: number;
	activeUnitPath?: string;
	profileShaderKey?: string;
	profileSourcePath?: string;
	profileSourceKind?: string;
	profileTotalRowCount?: number;
	profileSelectedRowCount?: number;
	profileSelectedRowSignature?: string;
	profileSelectionHintSourcePath?: string;
	unresolvedMacros: string[];
};

type P14CompilerContextMacroEvidenceEntry = UndefinedMacroOwnerHint & {
	macro: string;
	diagnostics: number;
	affectedUnitCount: number;
	affectedFileCount: number;
	samples: AuditDiagnostic[];
	defaultPresetPresent: boolean;
	defaultPresetValue?: string;
	effectiveConfigPresent: boolean;
	effectiveConfigValue?: string;
	effectiveConfigEmpty: boolean;
	definesPresent: boolean;
	definesValue?: number;
	statusSummary: Record<string, number>;
	profileStatusSummary: Record<string, number>;
	profileEvidence: P14CompilerContextProfileEvidenceEntry[];
};

type MacroEvidenceMapEntry = {
	name: string;
	value: string;
};

type UnitClosureResponse = {
	unitUri?: string;
	unitPath?: string;
	ready?: boolean;
	files?: Array<{
		path?: string;
		uri?: string;
		extension?: string;
	}>;
};

type DebugBuildDiagnosticsResponse = {
	uri?: string;
	path?: string;
	activeUnitUri?: string;
	activeUnitPath?: string;
	loaded?: boolean;
	truncated?: boolean;
	heavyRulesSkipped?: boolean;
	timedOut?: boolean;
	indeterminateTotal?: number;
	prerequisiteSkips?: DiagnosticsPrerequisiteSkips;
	macroHealth?: MacroHealthMetrics;
	elapsedMs?: number;
	diagnostics?: unknown[];
};

type DiagnosticsPrerequisiteSkips = {
	total?: number;
	active_unit_not_ready?: number;
	include_closure_not_ready?: number;
	preprocessor_context_unreliable?: number;
	parser_region_unreliable?: number;
	semantic_snapshot_unavailable?: number;
	local_scope_unreliable?: number;
	expression_type_unavailable?: number;
};

type MacroHealthMetrics = {
	initialConfiguredMacroCount?: number;
	initialArtDefaultZeroMacroCount?: number;
	initialCompilerPrivateConstantCount?: number;
	initialCompilerMacroSnapshotCount?: number;
	initialNumericDefineCount?: number;
	initialMacroCount?: number;
	sourceDefineEvents?: number;
	ifndefDefaultDefineEvents?: number;
	sourceUndefEvents?: number;
	synthesizedZeroEvents?: number;
	conditionDiagnosticCount?: number;
	undefinedMacroDiagnosticCount?: number;
	expansionWarningDiagnosticCount?: number;
	inactiveBranchDiagnosticCount?: number;
	branchMergeCount?: number;
	activeIncludeCount?: number;
};

type UnitSummary = {
	unit: string;
	unitRelativePath: string;
	filesInClosure: number;
	filesScanned: number;
	filesWithDiagnostics: number;
	diagnosticsTotal: number;
	truncatedFiles: number;
	timedOutFiles: number;
	buildElapsedMs: number;
	topCategories: Array<{ key: string; count: number }>;
	topMessages: GroupSummary[];
};

type UnitFileStat = {
	unit: string;
	unitRelativePath: string;
	file: string;
	relativePath: string;
	extension: string;
	loaded: boolean;
	diagnosticsTotal: number;
	truncated: boolean;
	timedOut: boolean;
	heavyRulesSkipped: boolean;
	indeterminateTotal: number;
	prerequisiteSkippedTotal: number;
	prerequisiteSkips: DiagnosticsPrerequisiteSkips;
	macroHealth: MacroHealthMetrics;
	elapsedMs: number;
};

type AuditDiagnosticSampleConfig = {
	perGroup: number;
	perUnit: number;
	maxTotal: number;
};

type AuditTrendMetric = {
	key: string;
	baseline: number;
	current: number;
	delta: number;
	percentDelta?: number;
};

type AuditTrendMessageDelta = AuditTrendMetric & {
	category: string;
	triage: string;
};

type AuditTrendComparison = {
	baselinePath: string;
	baselineGeneratedAt: string;
	summary: AuditTrendMetric[];
	triage: AuditTrendMetric[];
	category: AuditTrendMetric[];
	topMessages: AuditTrendMessageDelta[];
	warnings: string[];
};

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const auditEnabled = process.env.NSF_REAL_DIAGNOSTICS_AUDIT === '1';
const realDescribe = testMode === 'real' && auditEnabled ? describe : describe.skip;

const DEFAULT_EXTENSIONS = ['.nsf', '.hlsl'];
const REPORT_DIR = path.resolve(__dirname, '..', '..', '..', 'out', 'test', 'diagnostics-audit');
const DEFAULT_BASELINE_REPORT = 'real-workspace-diagnostics-audit.baseline-2026-05-16.json';
const DEFAULT_SMOKE_BASELINE_REPORT = 'real-workspace-diagnostics-audit.phase-00-baseline-smoke-5.json';
const DEFAULT_TREND_BASELINE_REPORT = 'real-workspace-diagnostics-audit.phase-00-baseline-trend-50.json';
const REAL_DIAGNOSTICS_TIMEOUT_MAX_MS = 21600000;
const P14_FOCUS_MACROS = ['COLOR_CHANGE_MODE', 'EMISSIVE_MODE', 'FOLIAGE_MODE'];
const P14L_COMPILER_CONTEXT_MACROS = [
	'API_MOBILE_HIGH_QUALITY',
	'API_PC_HIGH_QUALITY',
	'API_SUPPORT_FRAGCOORD',
	'API_SUPPORT_SV_INSTANCE_ID',
	'API_SUPPORT_TEXFETCH',
	'GL3_PROFILE',
	'SYSTEM_SUPPORT_SRGB'
];

function delay(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

function readIntEnv(name: string, fallback: number, min: number, max: number): number {
	const raw = process.env[name];
	if (!raw) {
		return fallback;
	}
	const parsed = Number.parseInt(raw, 10);
	if (!Number.isFinite(parsed)) {
		return fallback;
	}
	return Math.max(min, Math.min(max, parsed));
}

function readStringEnv(name: string): string {
	return (process.env[name] ?? '').trim();
}

function sanitizeReportLabel(value: string): string | undefined {
	const compact = value
		.trim()
		.replace(/\s+/g, '-')
		.replace(/[^A-Za-z0-9._-]/g, '-')
		.replace(/-+/g, '-')
		.replace(/^[.-]+|[.-]+$/g, '');
	return compact.length > 0 ? compact : undefined;
}

function numericValue(value: unknown): number {
	return typeof value === 'number' && Number.isFinite(value) ? value : 0;
}

function prerequisiteSkipNumber(
	value: DiagnosticsPrerequisiteSkips | undefined,
	key: keyof DiagnosticsPrerequisiteSkips
): number {
	return Math.max(0, Math.trunc(numericValue(value?.[key])));
}

function aggregatePrerequisiteSkips(fileStats: UnitFileStat[]): DiagnosticsPrerequisiteSkips {
	const keys: Array<keyof DiagnosticsPrerequisiteSkips> = [
		'active_unit_not_ready',
		'include_closure_not_ready',
		'preprocessor_context_unreliable',
		'parser_region_unreliable',
		'semantic_snapshot_unavailable',
		'local_scope_unreliable',
		'expression_type_unavailable'
	];
	const out: DiagnosticsPrerequisiteSkips = {
		total: fileStats.reduce((sum, item) => sum + item.prerequisiteSkippedTotal, 0)
	};
	for (const key of keys) {
		out[key] = fileStats.reduce(
			(sum, item) => sum + prerequisiteSkipNumber(item.prerequisiteSkips, key),
			0
		);
	}
	return out;
}

function macroHealthNumber(value: MacroHealthMetrics | undefined, key: keyof MacroHealthMetrics): number {
	return Math.max(0, Math.trunc(numericValue(value?.[key])));
}

function aggregateMacroHealth(fileStats: UnitFileStat[]): Required<MacroHealthMetrics> {
	const keys: Array<keyof MacroHealthMetrics> = [
		'initialConfiguredMacroCount',
		'initialArtDefaultZeroMacroCount',
		'initialCompilerPrivateConstantCount',
		'initialCompilerMacroSnapshotCount',
		'initialNumericDefineCount',
		'initialMacroCount',
		'sourceDefineEvents',
		'ifndefDefaultDefineEvents',
		'sourceUndefEvents',
		'synthesizedZeroEvents',
		'conditionDiagnosticCount',
		'undefinedMacroDiagnosticCount',
		'expansionWarningDiagnosticCount',
		'inactiveBranchDiagnosticCount',
		'branchMergeCount',
		'activeIncludeCount'
	];
	const out: Required<MacroHealthMetrics> = {
		initialConfiguredMacroCount: 0,
		initialArtDefaultZeroMacroCount: 0,
		initialCompilerPrivateConstantCount: 0,
		initialCompilerMacroSnapshotCount: 0,
		initialNumericDefineCount: 0,
		initialMacroCount: 0,
		sourceDefineEvents: 0,
		ifndefDefaultDefineEvents: 0,
		sourceUndefEvents: 0,
		synthesizedZeroEvents: 0,
		conditionDiagnosticCount: 0,
		undefinedMacroDiagnosticCount: 0,
		expansionWarningDiagnosticCount: 0,
		inactiveBranchDiagnosticCount: 0,
		branchMergeCount: 0,
		activeIncludeCount: 0
	};
	for (const key of keys) {
		out[key] = fileStats.reduce((sum, item) => sum + macroHealthNumber(item.macroHealth, key), 0);
	}
	return out;
}

function trendMetric(key: string, baseline: number, current: number): AuditTrendMetric {
	const delta = current - baseline;
	return {
		key,
		baseline,
		current,
		delta,
		percentDelta: baseline === 0 ? undefined : (delta / baseline) * 100
	};
}

function trendCountEntries(
	currentMap: Record<string, number> | undefined,
	baselineMap: Record<string, number> | undefined,
	limit: number
): AuditTrendMetric[] {
	const keys = new Set<string>([
		...Object.keys(currentMap ?? {}),
		...Object.keys(baselineMap ?? {})
	]);
	return Array.from(keys)
		.map((key) => trendMetric(key, numericValue(baselineMap?.[key]), numericValue(currentMap?.[key])))
		.sort(
			(lhs, rhs) =>
				Math.abs(rhs.delta) - Math.abs(lhs.delta) ||
				rhs.current - lhs.current ||
				lhs.key.localeCompare(rhs.key)
		)
		.slice(0, limit);
}

function groupCountMap(groups: unknown): Map<string, AuditTrendMessageDelta> {
	const out = new Map<string, AuditTrendMessageDelta>();
	if (!Array.isArray(groups)) {
		return out;
	}
	for (const group of groups) {
		const entry = group as { key?: unknown; category?: unknown; triage?: unknown; count?: unknown };
		const key = typeof entry.key === 'string' ? entry.key : '';
		const category = typeof entry.category === 'string' ? entry.category : '';
		const triage = typeof entry.triage === 'string' ? entry.triage : '';
		if (key.length === 0) {
			continue;
		}
		const mapKey = `${triage}|${category}|${key}`;
		out.set(mapKey, {
			key,
			category,
			triage,
			baseline: 0,
			current: numericValue(entry.count),
			delta: 0,
			percentDelta: undefined
		});
	}
	return out;
}

function trendMessageEntries(
	currentGroups: unknown,
	baselineGroups: unknown,
	limit: number
): AuditTrendMessageDelta[] {
	const current = groupCountMap(currentGroups);
	const baseline = groupCountMap(baselineGroups);
	const keys = new Set<string>([...current.keys(), ...baseline.keys()]);
	const entries: AuditTrendMessageDelta[] = [];
	for (const key of keys) {
		const currentEntry = current.get(key);
		const baselineEntry = baseline.get(key);
		const label = currentEntry ?? baselineEntry;
		if (!label) {
			continue;
		}
		entries.push({
			...trendMetric(
				label.key,
				numericValue(baselineEntry?.current),
				numericValue(currentEntry?.current)
			),
			category: label.category,
			triage: label.triage
		});
	}
	return entries
		.sort(
			(lhs, rhs) =>
				Math.abs(rhs.delta) - Math.abs(lhs.delta) ||
				rhs.current - lhs.current ||
				lhs.key.localeCompare(rhs.key)
		)
		.slice(0, limit);
}

function buildTrendComparison(report: any, baseline: any, baselinePath: string): AuditTrendComparison {
	const summaryKeys = [
		'unitsDiscovered',
		'unitsScanned',
		'unitsWithDiagnostics',
		'unitFileVisits',
		'unitOffset',
		'unitLimit',
		'filesDiscovered',
		'filesScanned',
		'filesWithDiagnostics',
		'diagnosticsTotal',
		'truncatedFiles',
		'timedOutFiles',
		'prerequisiteSkippedTotal',
		'fileErrors'
	];
	const summary = summaryKeys.map((key) =>
		trendMetric(key, numericValue(baseline?.summary?.[key]), numericValue(report?.summary?.[key]))
	);
	const warnings: string[] = [];
	for (const key of ['truncatedFiles', 'timedOutFiles', 'fileErrors']) {
		const entry = summary.find((item) => item.key === key);
		if (entry && entry.delta > 0) {
			warnings.push(`${key} increased by ${entry.delta}.`);
		}
	}
	return {
		baselinePath,
		baselineGeneratedAt: typeof baseline?.generatedAt === 'string' ? baseline.generatedAt : '',
		summary,
		triage: trendCountEntries(report?.counts?.byTriage, baseline?.counts?.byTriage, 20),
		category: trendCountEntries(report?.counts?.byCategory, baseline?.counts?.byCategory, 30),
		topMessages: trendMessageEntries(report?.groups, baseline?.groups, 30),
		warnings
	};
}

function defaultBaselineCandidates(maxUnits: number, unitOffset: number): string[] {
	if (unitOffset > 0) {
		return [];
	}
	const candidates: string[] = [];
	if (maxUnits === 5) {
		candidates.push(DEFAULT_SMOKE_BASELINE_REPORT);
	}
	if (maxUnits === 50) {
		candidates.push(DEFAULT_TREND_BASELINE_REPORT);
	}
	if (maxUnits === 0 || maxUnits === 5 || maxUnits === 50) {
		candidates.push(DEFAULT_BASELINE_REPORT);
	}
	return candidates.map((name) => path.join(REPORT_DIR, name));
}

function loadBaselineReport(maxUnits: number, unitOffset: number): { baselinePath: string; report: any } | undefined {
	const rawPath = readStringEnv('NSF_REAL_DIAGNOSTICS_BASELINE_JSON');
	const lowered = rawPath.toLowerCase();
	if (lowered === '0' || lowered === 'false' || lowered === 'none') {
		return undefined;
	}
	const explicit = rawPath.length > 0;
	const candidates = explicit ? [path.resolve(rawPath)] : defaultBaselineCandidates(maxUnits, unitOffset);
	const baselinePath = candidates.find((candidate) => fs.existsSync(candidate));
	if (!baselinePath) {
		if (explicit) {
			throw new Error(`Diagnostics audit baseline report not found: ${candidates[0]}`);
		}
		return undefined;
	}
	return {
		baselinePath,
		report: JSON.parse(fs.readFileSync(baselinePath, 'utf8'))
	};
}

function severityName(severity: vscode.DiagnosticSeverity | undefined): string {
	switch (severity) {
		case vscode.DiagnosticSeverity.Error:
			return 'error';
		case vscode.DiagnosticSeverity.Warning:
			return 'warning';
		case vscode.DiagnosticSeverity.Information:
			return 'information';
		case vscode.DiagnosticSeverity.Hint:
			return 'hint';
		default:
			return 'unknown';
	}
}

function diagnosticCodeText(diagnostic: vscode.Diagnostic): string {
	const code = diagnostic.code;
	if (typeof code === 'string' || typeof code === 'number') {
		return String(code);
	}
	if (!code || typeof code !== 'object') {
		return '';
	}
	const value = (code as { value?: unknown }).value;
	if (typeof value === 'string' || typeof value === 'number') {
		return String(value);
	}
	return '';
}

function diagnosticReasonCode(diagnostic: vscode.Diagnostic): string | undefined {
	const data = (diagnostic as unknown as { data?: { reasonCode?: unknown } }).data;
	return typeof data?.reasonCode === 'string' ? data.reasonCode : undefined;
}

function severityNameFromWire(severity: unknown): string {
	if (typeof severity !== 'number') {
		return 'unknown';
	}
	switch (severity) {
		case 1:
			return 'error';
		case 2:
			return 'warning';
		case 3:
			return 'information';
		case 4:
			return 'hint';
		default:
			return 'unknown';
	}
}

function diagnosticCodeTextFromWire(diagnostic: unknown): string {
	const value = (diagnostic as { code?: unknown })?.code;
	if (typeof value === 'string' || typeof value === 'number') {
		return String(value);
	}
	if (!value || typeof value !== 'object') {
		return '';
	}
	const nested = (value as { value?: unknown }).value;
	return typeof nested === 'string' || typeof nested === 'number' ? String(nested) : '';
}

function diagnosticReasonCodeFromWire(diagnostic: unknown): string | undefined {
	const data = (diagnostic as { data?: { reasonCode?: unknown } })?.data;
	return typeof data?.reasonCode === 'string' ? data.reasonCode : undefined;
}

function diagnosticRangeStartFromWire(diagnostic: unknown): { line: number; character: number } {
	const start = (diagnostic as { range?: { start?: { line?: unknown; character?: unknown } } })?.range?.start;
	return {
		line: typeof start?.line === 'number' ? start.line : 0,
		character: typeof start?.character === 'number' ? start.character : 0
	};
}

function diagnosticMessageFromWire(diagnostic: unknown): string {
	const message = (diagnostic as { message?: unknown })?.message;
	return typeof message === 'string' ? message : '';
}

function diagnosticSourceFromWire(diagnostic: unknown): string {
	const source = (diagnostic as { source?: unknown })?.source;
	return typeof source === 'string' ? source : '';
}

function configuredShaderExtensions(): string[] {
	const configured = vscode.workspace.getConfiguration('nsf').get<string[]>('shaderFileExtensions', []);
	const allowed = new Set(DEFAULT_EXTENSIONS);
	const merged = [...configured, ...DEFAULT_EXTENSIONS]
		.map((item) => item.trim().toLowerCase())
		.filter((item) => item.length > 0)
		.map((item) => (item.startsWith('.') ? item : `.${item}`))
		.filter((item) => allowed.has(item));
	const seen = new Set<string>();
	const out: string[] = [];
	for (const item of merged) {
		if (seen.has(item)) {
			continue;
		}
		seen.add(item);
		out.push(item);
	}
	return out;
}

function workspaceFolderLabel(uri: vscode.Uri): string {
	const folder = vscode.workspace.getWorkspaceFolder(uri);
	return folder?.name ?? '';
}

function relativePathForUri(uri: vscode.Uri): string {
	const folder = vscode.workspace.getWorkspaceFolder(uri);
	if (!folder) {
		return uri.fsPath;
	}
	return path.relative(folder.uri.fsPath, uri.fsPath).replace(/\\/g, '/');
}

async function waitForCommandAvailable(command: string, timeoutMs = 120000): Promise<void> {
	const deadline = Date.now() + timeoutMs;
	while (Date.now() < deadline) {
		const commands = await vscode.commands.getCommands(true);
		if (commands.includes(command)) {
			return;
		}
		await delay(500);
	}
	throw new Error(`Timed out waiting for command ${command}`);
}

async function waitForClientReady(label: string, timeoutMs = 120000): Promise<void> {
	const deadline = Date.now() + timeoutMs;
	let lastState = '';
	while (Date.now() < deadline) {
		const value = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
		lastState = JSON.stringify(value ?? null);
		if (value?.clientState === 'ready') {
			return;
		}
		await delay(250);
	}
	throw new Error(`Timed out waiting for ${label}. Last client state: ${lastState}`);
}

async function waitForIndexingIdle(label: string, timeoutMs = 180000): Promise<void> {
	const deadline = Date.now() + timeoutMs;
	let lastState = '';
	while (Date.now() < deadline) {
		const value = await vscode.commands.executeCommand<any>('nsf._getInternalStatus');
		const state = value?.indexingState;
		lastState = JSON.stringify(state ?? null);
		const idle =
			!state ||
			(state.state === 'Idle' &&
				(state.pending?.queuedTasks ?? 0) === 0 &&
				(state.pending?.runningWorkers ?? 0) === 0);
		if (idle) {
			return;
		}
		await delay(500);
	}
	throw new Error(`Timed out waiting for ${label}. Last indexing state: ${lastState}`);
}

async function waitForDiagnosticsReady(uri: vscode.Uri, timeoutMs: number): Promise<boolean> {
	const expectedUri = uri.toString().toLowerCase();
	const deadline = Date.now() + timeoutMs;
	while (Date.now() < deadline) {
		const response = await vscode.commands.executeCommand<RuntimeDebugResponse>(
			'nsf._getDocumentRuntimeDebug',
			{ uris: [uri.toString()] }
		);
		const entry = response?.documents?.[0];
		const layer = entry?.lastDiagnosticsPublishLayer ?? '';
		const ready =
			Boolean(entry?.exists) &&
			(Boolean(entry?.deferredHasFullDiagnostics) ||
				layer === 'GlobalContext' ||
				layer === 'CurrentDocSemantic' ||
				layer === 'LocalStructural');
		if (ready && (entry?.uri ?? '').toLowerCase() === expectedUri) {
			return true;
		}
		await delay(120);
	}
	return false;
}

function normalizeSearchRoots(paths: string[]): string[] {
	const roots = paths
		.map((item) => item.trim())
		.filter((item) => item.length > 0)
		.map((item) => path.resolve(item))
		.filter((item) => {
			try {
				return fs.statSync(item).isDirectory();
			} catch {
				return false;
			}
		});
	const seen = new Set<string>();
	const out: string[] = [];
	for (const root of roots) {
		const key = root.toLowerCase();
		if (seen.has(key)) {
			continue;
		}
		seen.add(key);
		out.push(root);
	}
	return out;
}

function configuredAuditSearchRoots(): string[] {
	const configured = vscode.workspace.getConfiguration('nsf').get<string[]>('intellisionPath', []);
	const configuredRoots = normalizeSearchRoots(configured);
	if (configuredRoots.length > 0) {
		return configuredRoots;
	}
	return normalizeSearchRoots((vscode.workspace.workspaceFolders ?? []).map((folder) => folder.uri.fsPath));
}

function normalizedPathKey(value: string): string {
	return path.resolve(value).replace(/\\/g, '/').toLowerCase();
}

function profileNumber(value: unknown): number | undefined {
	return typeof value === 'number' && Number.isFinite(value) ? Math.trunc(value) : undefined;
}

function profileString(value: unknown): string | undefined {
	return typeof value === 'string' && value.length > 0 ? value : undefined;
}

function profileStringArray(value: unknown): string[] {
	return Array.isArray(value) ? value.filter((item): item is string => typeof item === 'string') : [];
}

async function collectP14FocusProfileEvidenceForUnit(
	unit: vscode.Uri,
	unitRelativePath: string
): Promise<P14FocusProfileEvidenceEntry[]> {
	const debugDocument = await findP14FocusRuntimeDebugDocument(unit);
	return buildP14FocusProfileEvidenceForRuntimeDebug(unit, unitRelativePath, debugDocument);
}

function buildP14FocusProfileEvidenceForRuntimeDebug(
	unit: vscode.Uri,
	unitRelativePath: string,
	debugDocument: RuntimeDebugDocument | undefined
): P14FocusProfileEvidenceEntry[] {
	const unitKey = normalizedPathKey(unit.fsPath);
	const activeUnitPath = profileString(debugDocument?.activeUnitPath);
	const activeUnitMatches =
		Boolean(activeUnitPath) && normalizedPathKey(activeUnitPath ?? '') === unitKey;
	const defines = debugDocument?.activeUnitProfileDefines ?? {};
	const artDefines = debugDocument?.activeUnitArtDefaultZeroDefines ?? {};
	const artMacros = Array.isArray(debugDocument?.activeUnitArtDefaultZeroMacros)
		? debugDocument.activeUnitArtDefaultZeroMacros
		: [];
	const unresolvedMacros = profileStringArray(debugDocument?.activeUnitProfileUnresolvedMacros);
	const unresolvedUpper = new Set(unresolvedMacros.map((item) => item.toUpperCase()));
	const sourcePath = profileString(debugDocument?.activeUnitProfileSourcePath);
	const totalRows = profileNumber(debugDocument?.activeUnitProfileTotalRowCount);
	const selectedRows = profileNumber(debugDocument?.activeUnitProfileSelectedRowCount);

	return P14_FOCUS_MACROS.map((macro) => {
		const injectedValue = defines[macro];
		const artDefaultValue = artDefines[macro];
		const artEntry = artMacros.find((entry) => typeof entry?.name === 'string' && entry.name.toUpperCase() === macro);
		const artSourceUri = profileString(artEntry?.uri);
		let artSourcePath: string | undefined;
		if (artSourceUri) {
			try {
				artSourcePath = vscode.Uri.parse(artSourceUri).fsPath;
			} catch {
				artSourcePath = artSourceUri;
			}
		}
		let status = 'runtime-debug-missing';
		if (debugDocument?.exists) {
			if (!activeUnitMatches) {
				status = 'active-unit-mismatch';
			} else if (typeof injectedValue === 'number') {
				status = 'profile-injected';
			} else if (typeof artDefaultValue === 'number') {
				status = 'art-default-zero';
			} else if (unresolvedUpper.has(macro)) {
				status = 'profile-unresolved';
			} else if (!sourcePath) {
				status = 'profile-source-missing';
			} else if ((totalRows ?? 0) <= 0) {
				status = 'profile-source-empty';
			} else if ((selectedRows ?? 0) <= 0) {
				status = 'profile-no-selected-row';
			} else {
				status = 'profile-source-no-macro';
			}
		}
		return {
			macro,
			unit: unit.fsPath,
			unitRelativePath,
			status,
			injectedValue: typeof injectedValue === 'number' ? injectedValue : undefined,
			activeUnitPath,
			profileShaderKey: profileString(debugDocument?.activeUnitProfileShaderKey),
			profileSourcePath: sourcePath,
			profileSourceKind: profileString(debugDocument?.activeUnitProfileSourceKind),
			profileTotalRowCount: totalRows,
			profileSelectedRowCount: selectedRows,
			profileSelectedRowSignature: profileString(debugDocument?.activeUnitProfileSelectedRowSignature),
			profileSelectionHintSourcePath: profileString(debugDocument?.activeUnitProfileSelectionHintSourcePath),
			artDefaultZero: typeof artDefaultValue === 'number',
			artDefaultSourcePath: artSourcePath,
			artDefaultSourceLine: profileNumber(artEntry?.line) !== undefined ? profileNumber(artEntry?.line)! + 1 : undefined,
			artDefaultType: profileString(artEntry?.artType),
			unresolvedMacros
		};
	});
}

function buildP14CompilerContextProfileEvidenceForRuntimeDebug(
	unit: vscode.Uri,
	unitRelativePath: string,
	debugDocument: RuntimeDebugDocument | undefined
): P14CompilerContextProfileEvidenceEntry[] {
	const unitKey = normalizedPathKey(unit.fsPath);
	const activeUnitPath = profileString(debugDocument?.activeUnitPath);
	const activeUnitMatches =
		Boolean(activeUnitPath) && normalizedPathKey(activeUnitPath ?? '') === unitKey;
	const defines = debugDocument?.activeUnitProfileDefines ?? {};
	const unresolvedMacros = profileStringArray(debugDocument?.activeUnitProfileUnresolvedMacros);
	const unresolvedUpper = new Set(unresolvedMacros.map((item) => item.toUpperCase()));
	const sourcePath = profileString(debugDocument?.activeUnitProfileSourcePath);
	const totalRows = profileNumber(debugDocument?.activeUnitProfileTotalRowCount);
	const selectedRows = profileNumber(debugDocument?.activeUnitProfileSelectedRowCount);

	return P14L_COMPILER_CONTEXT_MACROS.map((macro) => {
		const injectedValue = defines[macro];
		let status = 'runtime-debug-missing';
		if (debugDocument?.exists) {
			if (!activeUnitMatches) {
				status = 'active-unit-mismatch';
			} else if (typeof injectedValue === 'number') {
				status = 'profile-injected';
			} else if (unresolvedUpper.has(macro)) {
				status = 'profile-unresolved';
			} else if (!sourcePath) {
				status = 'profile-source-missing';
			} else if ((totalRows ?? 0) <= 0) {
				status = 'profile-source-empty';
			} else if ((selectedRows ?? 0) <= 0) {
				status = 'profile-no-selected-row';
			} else {
				status = 'profile-source-no-macro';
			}
		}
		return {
			macro,
			unit: unit.fsPath,
			unitRelativePath,
			status,
			injectedValue: typeof injectedValue === 'number' ? injectedValue : undefined,
			activeUnitPath,
			profileShaderKey: profileString(debugDocument?.activeUnitProfileShaderKey),
			profileSourcePath: sourcePath,
			profileSourceKind: profileString(debugDocument?.activeUnitProfileSourceKind),
			profileTotalRowCount: totalRows,
			profileSelectedRowCount: selectedRows,
			profileSelectedRowSignature: profileString(debugDocument?.activeUnitProfileSelectedRowSignature),
			profileSelectionHintSourcePath: profileString(debugDocument?.activeUnitProfileSelectionHintSourcePath),
			unresolvedMacros
		};
	});
}

async function readRuntimeDebugDocuments(uris?: string[]): Promise<RuntimeDebugDocument[]> {
	try {
		const response = await vscode.commands.executeCommand<RuntimeDebugResponse>(
			'nsf._getDocumentRuntimeDebug',
			uris ? { uris } : undefined
		);
		return response?.documents ?? [];
	} catch {
		return [];
	}
}

function findRuntimeDebugDocumentForActiveUnit(
	documents: RuntimeDebugDocument[],
	unit: vscode.Uri
): RuntimeDebugDocument | undefined {
	const unitKey = normalizedPathKey(unit.fsPath);
	return documents.find((entry) => {
		const activeUnitPath = profileString(entry?.activeUnitPath);
		return Boolean(entry?.exists) && Boolean(activeUnitPath) && normalizedPathKey(activeUnitPath ?? '') === unitKey;
	});
}

async function findP14FocusRuntimeDebugDocument(unit: vscode.Uri): Promise<RuntimeDebugDocument | undefined> {
	let fallback: RuntimeDebugDocument | undefined;
	try {
		await vscode.workspace.openTextDocument(unit);
	} catch {
		// The audit will report runtime-debug-missing below; file load failures are handled by diagnostics scan.
	}

	for (let attempt = 0; attempt < 6; attempt++) {
		const targeted = await readRuntimeDebugDocuments([unit.toString()]);
		const targetedEntry = targeted[0];
		fallback = targetedEntry ?? fallback;
		const targetedMatch = findRuntimeDebugDocumentForActiveUnit(targeted, unit);
		if (targetedMatch) {
			return targetedMatch;
		}

		const allDocuments = await readRuntimeDebugDocuments();
		const allMatch = findRuntimeDebugDocumentForActiveUnit(allDocuments, unit);
		if (allMatch) {
			return allMatch;
		}
		fallback = allDocuments.find((entry) => Boolean(entry?.exists)) ?? fallback;
		await delay(50);
	}

	return fallback;
}

async function findShaderFiles(
	extensions: string[],
	maxFiles: number,
	searchRoots: string[]
): Promise<vscode.Uri[]> {
	const exclude = process.env.NSF_REAL_DIAGNOSTICS_EXCLUDE_GLOB ??
		'**/{.git,node_modules,.vscode-test}/**';
	const all: vscode.Uri[] = [];
	for (const root of searchRoots) {
		for (const extension of extensions) {
			const found = await vscode.workspace.findFiles(new vscode.RelativePattern(root, `**/*${extension}`), exclude);
			all.push(...found);
		}
	}
	const seen = new Set<string>();
	const deduped = all
		.filter((uri) => uri.scheme === 'file')
		.filter((uri) => {
			const key = uri.fsPath.toLowerCase();
			if (seen.has(key)) {
				return false;
			}
			seen.add(key);
			return true;
		})
		.sort((lhs, rhs) => lhs.fsPath.localeCompare(rhs.fsPath));
	if (maxFiles > 0) {
		return deduped.slice(0, maxFiles);
	}
	return deduped;
}

async function findNsfUnits(
	maxUnits: number,
	unitOffset: number,
	searchRoots: string[]
): Promise<{ allUnits: vscode.Uri[]; units: vscode.Uri[] }> {
	const allUnits = await findShaderFiles(['.nsf'], 0, searchRoots);
	const units = maxUnits > 0
		? allUnits.slice(unitOffset, unitOffset + maxUnits)
		: allUnits.slice(unitOffset);
	return { allUnits, units };
}

async function getUnitIncludeClosure(unit: vscode.Uri, limit: number): Promise<vscode.Uri[]> {
	const response = await vscode.commands.executeCommand<UnitClosureResponse>(
		'nsf._sendServerRequest',
		{
			method: 'nsf/_debugIncludeClosureForUnit',
			params: {
				uri: unit.toString(),
				limit
			}
		}
	);
	const files = Array.isArray(response?.files) ? response.files : [];
	const uris: vscode.Uri[] = [];
	const seen = new Set<string>();
	for (const item of files) {
		const rawUri = typeof item?.uri === 'string' ? item.uri : '';
		const rawPath = typeof item?.path === 'string' ? item.path : '';
		let uri: vscode.Uri | undefined;
		if (rawUri.length > 0) {
			uri = vscode.Uri.parse(rawUri);
		} else if (rawPath.length > 0) {
			uri = vscode.Uri.file(rawPath);
		}
		if (!uri || uri.scheme !== 'file') {
			continue;
		}
		const ext = path.extname(uri.fsPath).toLowerCase();
		if (!DEFAULT_EXTENSIONS.includes(ext)) {
			continue;
		}
		const key = normalizedPathKey(uri.fsPath);
		if (seen.has(key)) {
			continue;
		}
		seen.add(key);
		uris.push(uri);
	}
	if (!seen.has(normalizedPathKey(unit.fsPath))) {
		uris.unshift(unit);
	}
	return uris;
}

function hasExplicitPreprocessorMacroSetting(): boolean {
	const inspected = vscode.workspace.getConfiguration('nsf').inspect<Record<string, unknown>>('preprocessorMacros');
	return (
		inspected?.globalValue !== undefined ||
		inspected?.workspaceValue !== undefined ||
		inspected?.workspaceFolderValue !== undefined
	);
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

function settingReplacementToString(value: unknown): string | undefined {
	if (typeof value === 'string') {
		return value;
	}
	if (typeof value === 'number') {
		return Number.isFinite(value) ? String(value) : undefined;
	}
	if (typeof value === 'boolean') {
		return value ? '1' : '0';
	}
	return undefined;
}

function buildMacroEvidenceMap(root: Record<string, unknown> | undefined): Map<string, MacroEvidenceMapEntry> {
	const map = new Map<string, MacroEvidenceMapEntry>();
	if (!root || typeof root !== 'object' || Array.isArray(root)) {
		return map;
	}
	for (const [rawName, rawValue] of Object.entries(root)) {
		const name = rawName.trim();
		const value = settingReplacementToString(rawValue);
		if (name.length === 0 || value === undefined) {
			continue;
		}
		map.set(name.toUpperCase(), { name, value });
	}
	return map;
}

async function readServerPreprocessorMacroPresetMap(): Promise<Map<string, MacroEvidenceMapEntry>> {
	const response = await vscode.commands.executeCommand<PreprocessorMacroPresetResponse>(
		'nsf._sendServerRequest',
		{ method: 'nsf/getPreprocessorMacroPreset', params: {} }
	);
	return buildMacroEvidenceMap(normalizePreprocessorMacroPreset(response ?? {}));
}

function readEffectivePreprocessorMacroSettings(): Record<string, unknown> {
	const inspected = vscode.workspace.getConfiguration('nsf').inspect<Record<string, unknown>>('preprocessorMacros');
	const value =
		inspected?.workspaceFolderValue ??
		inspected?.workspaceValue ??
		inspected?.globalValue ??
		{};
	return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

function readEffectivePreprocessorMacroSettingsMap(): Map<string, MacroEvidenceMapEntry> {
	return buildMacroEvidenceMap(readEffectivePreprocessorMacroSettings());
}

function readConfiguredDefinesMap(): Map<string, { name: string; value: number }> {
	const map = new Map<string, { name: string; value: number }>();
	const defines = vscode.workspace.getConfiguration('nsf').get<string[]>('defines', []);
	for (const raw of defines) {
		if (typeof raw !== 'string') {
			continue;
		}
		let value = raw;
		if (value.startsWith('-D')) {
			value = value.slice(2);
		}
		const eq = value.indexOf('=');
		const name = eq === -1 ? value : value.slice(0, eq);
		const rhs = eq === -1 ? '1' : value.slice(eq + 1);
		if (name.length === 0) {
			continue;
		}
		const parsed = Number.parseInt(rhs, 10);
		map.set(name.toUpperCase(), {
			name,
			value: Number.isFinite(parsed) ? parsed : 1
		});
	}
	return map;
}

async function seedAuditPreprocessorMacrosIfMissing(): Promise<number> {
	if (hasExplicitPreprocessorMacroSetting()) {
		return 0;
	}
	const response = await vscode.commands.executeCommand<PreprocessorMacroPresetResponse>(
		'nsf._sendServerRequest',
		{ method: 'nsf/getPreprocessorMacroPreset', params: {} }
	);
	const preset = normalizePreprocessorMacroPreset(response ?? {});
	const count = Object.keys(preset).length;
	assert.ok(count > 0, 'Expected preprocessor macro preset entries from server registry.');

	await vscode.workspace
		.getConfiguration('nsf')
		.update('preprocessorMacros', preset, vscode.ConfigurationTarget.Global);
	await vscode.commands.executeCommand('nsf.restartServer');
	await waitForClientReady('client ready after audit preprocessor macro seed');
	return count;
}

function findSiblingNsf(uri: vscode.Uri): vscode.Uri | undefined {
	const dir = path.dirname(uri.fsPath);
	try {
		const names = fs
			.readdirSync(dir, { withFileTypes: true })
			.filter((entry) => entry.isFile() && entry.name.toLowerCase().endsWith('.nsf'))
			.map((entry) => entry.name)
			.sort((lhs, rhs) => lhs.localeCompare(rhs));
		const first = names[0];
		return first ? vscode.Uri.file(path.join(dir, first)) : undefined;
	} catch {
		return undefined;
	}
}

function resolveActiveUnitForTarget(uri: vscode.Uri): vscode.Uri | undefined {
	const ext = path.extname(uri.fsPath).toLowerCase();
	if (ext === '.nsf') {
		return uri;
	}
	return findSiblingNsf(uri);
}

async function setActiveUnitForTarget(uri: vscode.Uri): Promise<string | undefined> {
	const activeUnit = resolveActiveUnitForTarget(uri);
	if (activeUnit) {
		await vscode.commands.executeCommand('nsf._setActiveUnitForTests', activeUnit.toString());
		return relativePathForUri(activeUnit);
	}
	await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
	return undefined;
}

function classifyDiagnostic(
	message: string,
	code: string,
	lineText: string,
	nextLineText: string,
	extension: string
): { category: string; triage: string } {
	if (code.startsWith('NSF_INDET') || message.startsWith('Indeterminate ')) {
		return { category: 'indeterminate-analysis', triage: 'likely-plugin-limitation' };
	}
	if (message.startsWith('Function-like macro is not supported in preprocessor expression:')) {
		return { category: 'preprocessor-macro-model', triage: 'likely-plugin-limitation' };
	}
	if (message.startsWith('Undefined macro in preprocessor expression:')) {
		return { category: 'preprocessor-context', triage: 'check-config-or-source' };
	}
	if (message.startsWith('Undefined identifier:')) {
		return { category: 'undefined-identifier', triage: 'likely-plugin-limitation' };
	}
	if (message.startsWith('Cannot resolve include:')) {
		return { category: 'include-resolution', triage: 'check-config-or-source' };
	}
	if (
		message.includes('Function call argument mismatch: GetVisibility.') &&
		message.includes('Expected: (float, float3). Got: (float, float2).')
	) {
		return { category: 'call-type-analysis', triage: 'needs-manual-review' };
	}
	if (
		message.includes('Function call argument count mismatch: SampleTexArryPkgNormalBias.') &&
		message.includes('Expected 5 but got 4.')
	) {
		return { category: 'call-type-analysis', triage: 'needs-manual-review' };
	}
	if (message === 'Assignment type mismatch: half4 = half3.') {
		return { category: 'expression-type-analysis', triage: 'needs-manual-review' };
	}
	if (message.startsWith('Function call argument') ||
		message.startsWith('Builtin call type mismatch:') ||
		message.startsWith('Built-in method call type mismatch:')) {
		return { category: 'call-type-analysis', triage: 'likely-plugin-limitation' };
	}
	if (message.startsWith('Assignment type mismatch:') ||
		message.startsWith('Binary operator type mismatch:') ||
		message.startsWith('Return type mismatch:')) {
		return { category: 'expression-type-analysis', triage: 'likely-plugin-limitation' };
	}
	if (message.startsWith('Implicit truncation conversion:') ||
		message.startsWith('Implicit boolean conversion:') ||
		message.startsWith('Implicit floating-integral conversion:') ||
		message.startsWith('Implicit signedness conversion:') ||
		message.startsWith('Implicit narrowing conversion:')) {
		return { category: 'type-conversion-risk', triage: 'needs-manual-review' };
	}
	if (message === 'Missing semicolon.') {
		const trimmed = lineText.trim();
		const nextTrimmed = nextLineText.trim();
		const looksLikeEffectAnnotation =
			/:\s*Sas[A-Za-z_]*\b/.test(trimmed) ||
			/^\w[\w\s<>]*\s+\w+\s*:/.test(trimmed) ||
			/^(texture|samplerstate|sampler|rasterizerstate|depthstencilstate|blendstate)\b/i.test(trimmed) ||
			trimmed.endsWith('<') ||
			trimmed.endsWith(',') ||
			nextTrimmed === '<' ||
			nextTrimmed.startsWith('<') ||
			nextTrimmed === '{' ||
			nextTrimmed === '};';
		if (looksLikeEffectAnnotation) {
			return { category: 'effect-syntax-or-macro', triage: 'likely-plugin-limitation' };
		}
		return { category: 'syntax-structure', triage: 'needs-manual-review' };
	}
	if (message.startsWith('Unmatched ') ||
		message.startsWith('Unterminated ') ||
		message.startsWith('Invalid #include syntax') ||
		message.startsWith('Missing parentheses after')) {
		return { category: 'syntax-structure', triage: 'likely-real-source' };
	}
	if (message.startsWith('Invalid numeric literal suffix:')) {
		if (/[0-9](?:\.[0-9]*)?[eE][+-]?[0-9]+/.test(lineText)) {
			return { category: 'numeric-literal', triage: 'likely-plugin-limitation' };
		}
		return { category: 'numeric-literal', triage: 'likely-real-source' };
	}
	if (message.startsWith('Deprecated numeric literal suffix:')) {
		return { category: 'numeric-literal', triage: 'check-config-or-source' };
	}
	if (message === 'Unreachable code.') {
		return { category: 'semantic-source-rule', triage: 'needs-manual-review' };
	}
	if (message.startsWith('Duplicate ') ||
		message.startsWith('Local shadows parameter:') ||
		message.startsWith('Missing return') ||
		message === 'Return value in void function.') {
		return { category: 'semantic-source-rule', triage: 'needs-manual-review' };
	}
	return { category: 'other', triage: 'needs-manual-review' };
}

function canonicalizeMessage(message: string): string {
	if (message.startsWith('Undefined identifier:')) {
		return 'Undefined identifier: <symbol>.';
	}
	if (message.startsWith('Cannot resolve include:')) {
		return 'Cannot resolve include: <path>';
	}
	if (message.startsWith('Undefined macro in preprocessor expression:')) {
		return 'Undefined macro in preprocessor expression: <macro>.';
	}
	if (message.startsWith('Function-like macro is not supported in preprocessor expression:')) {
		return 'Function-like macro is not supported in preprocessor expression: <macro>.';
	}
	if (message.startsWith('Function call argument mismatch:')) {
		return 'Function call argument mismatch: <function>.';
	}
	if (message.startsWith('Function call argument count mismatch:')) {
		return 'Function call argument count mismatch: <function>.';
	}
	if (message.startsWith('Builtin call type mismatch:')) {
		return 'Builtin call type mismatch: <function>.';
	}
	if (message.startsWith('Built-in method call type mismatch:')) {
		return 'Built-in method call type mismatch: <method>.';
	}
	if (message.startsWith('Assignment type mismatch:')) {
		return 'Assignment type mismatch: <lhs> = <rhs>.';
	}
	if (message.startsWith('Binary operator type mismatch:')) {
		return 'Binary operator type mismatch: <lhs> <op> <rhs>.';
	}
	if (message.startsWith('Return type mismatch:')) {
		return 'Return type mismatch: <expected> vs <actual>.';
	}
	if (message.startsWith('Implicit truncation conversion:')) {
		return 'Implicit truncation conversion: <from> -> <to>.';
	}
	if (message.startsWith('Implicit boolean conversion:')) {
		return 'Implicit boolean conversion: <from> -> <to>.';
	}
	if (message.startsWith('Implicit floating-integral conversion:')) {
		return 'Implicit floating-integral conversion: <from> -> <to>.';
	}
	if (message.startsWith('Implicit signedness conversion:')) {
		return 'Implicit signedness conversion: <from> -> <to>.';
	}
	if (message.startsWith('Implicit narrowing conversion:')) {
		return 'Implicit narrowing conversion: <from> -> <to>.';
	}
	if (message.startsWith('Duplicate global declaration:')) {
		return 'Duplicate global declaration: <symbol>.';
	}
	if (message.startsWith('Duplicate local declaration:')) {
		return 'Duplicate local declaration: <symbol>.';
	}
	if (message.startsWith('Duplicate parameter declaration:')) {
		return 'Duplicate parameter declaration: <symbol>.';
	}
	if (message.startsWith('Local shadows parameter:')) {
		return 'Local shadows parameter: <symbol>.';
	}
	if (message.startsWith('Invalid numeric literal suffix:')) {
		return 'Invalid numeric literal suffix: <suffix>.';
	}
	if (message.startsWith('Deprecated numeric literal suffix:')) {
		return 'Deprecated numeric literal suffix: <suffix>.';
	}
	if (message.startsWith('Missing parentheses after')) {
		return 'Missing parentheses after <keyword>.';
	}
	if (message.startsWith('Unmatched preprocessor directive:')) {
		return 'Unmatched preprocessor directive: <directive>.';
	}
	if (message.startsWith('Unterminated preprocessor conditional:')) {
		return 'Unterminated preprocessor conditional: <directive>.';
	}
	return message;
}

function undefinedMacroNameFromMessage(message: string): string | undefined {
	const match = /^Undefined macro in preprocessor expression:\s*([A-Za-z_][A-Za-z0-9_]*)\.$/.exec(
		message.trim()
	);
	return match?.[1];
}

const selectorProfileUndefinedMacros = new Set<string>([
	'CHANGE_INOUTDOOR',
	'COLOR_CHANGE_MODE',
	'DYNAMIC_GI_TYPE',
	'EMISSIVE_MODE',
	'FOLIAGE_MODE',
	'HAIR_COLOR_MODE',
	'SHADINGMODELID',
	'SMAA_QUALITY',
	'SPARKLE_ENABLE',
	'TERRAIN_TECH_TYPE'
]);

const sourceGeneratedUndefinedMacros = new Set<string>([
	'NEOX_COMPUTE_SHADER',
	'NEOX_PARAM_DECLARE_DOMAIN',
	'PS_INPUT_HAS_INSTANCE_ID',
	'VAT_PER_INSTANCE_INPUT'
]);

function isEnumLikeStableConstantMacro(upper: string): boolean {
	return (
		/^SHADINGMODELID_[A-Z0-9_]+$/.test(upper) ||
		/^DYNAMIC_GI_(?!TYPE$)[A-Z0-9_]+$/.test(upper) ||
		/^COLOR_CHANGE_(?!MODE$|ENABLE$)[A-Z0-9_]+$/.test(upper) ||
		/^CHANNEL_COLOR_CHANGE(?:_[A-Z0-9_]+)?$/.test(upper) ||
		/^EMISSIVE_(?!MODE$|ENABLE$|ENBALE$|FLOW_ENABLE$)[A-Z0-9_]+$/.test(upper) ||
		/^FOLIAGE_(?:GRASS|TREE)_(?:BRANCH|LEAF)$/.test(upper) ||
		/^SPARKLE_MODE_[A-Z0-9_]+$/.test(upper) ||
		/^SMAA_PRESET_[A-Z0-9_]+$/.test(upper) ||
		/^HAIR_COLOR_(?!MODE$|ENABLE$)[A-Z0-9_]+$/.test(upper) ||
		/^CHANGE_INOUTDOOR_(?:INDOOR|OUTDOOR|INDOORCUBE)$/.test(upper)
	);
}

function classifyUndefinedMacroOwner(macro: string): UndefinedMacroOwnerHint {
	const upper = macro.toUpperCase();
	if (
		/^(API_|SYSTEM_|GLES_|SHADER_API_|PLATFORM_|DEVICE_)/.test(upper) ||
		/^(NEOX_(?:D3D|GLES|GLSL|HLSL|METAL|VULKAN|FLOATRT)|MTL_)/.test(upper) ||
		/^GL\d*_PROFILE$/.test(upper) ||
		/^IS_ADRENO_\d+XX$/.test(upper) ||
		upper === 'SHADER_QUALITY' ||
		upper.startsWith('QUALITY_')
	) {
		return {
			owner: 'compiler-context-platform-quality',
			sourceBoundary: 'shadercompiler context or platform/quality preset candidate',
			defaultValue: 'not assigned by audit',
			risk:
				'Only promote after confirming the macro is a stable shadercompiler context input; a wrong global default changes active branches for every unit.',
			reason: 'Name matches platform, API, system, or quality context macro conventions.'
		};
	}
	if (selectorProfileUndefinedMacros.has(upper)) {
		return {
			owner: 'selector-profile-macro',
			sourceBoundary: 'active unit compile profile, parameter include, nsf.preprocessorMacros, or nsf.defines',
			defaultValue: 'not assigned by audit',
			risk:
				'Do not assign a global default; selector/profile macros choose material variants and must come from the real unit context.',
			reason: 'Name is a selector/profile macro that chooses among stable constants or feature branches.'
		};
	}
	if (isEnumLikeStableConstantMacro(upper)) {
		return {
			owner: 'enum-like-stable-constant',
			sourceBoundary: 'source constant definition or generated shader parameter include review',
			defaultValue: 'stable value candidate; not assigned by audit',
			risk:
				'Only promote after confirming the source definition and value; the audit reports ownership but does not add defaults.',
			reason: 'Name matches enum-like constant conventions used as selector comparison values.'
		};
	}
	if (sourceGeneratedUndefinedMacros.has(upper)) {
		return {
			owner: 'source-generated-config',
			sourceBoundary: 'source include, generated header, shader stage injection, or workspace configuration review',
			defaultValue: 'not assigned by audit',
			risk:
				'Keep the diagnostic until generated/source ownership is known; suppressing it could hide a real missing definition.',
			reason: 'Name matches known source/generated configuration macro conventions.'
		};
	}
	if (
		/^(ADAPTIVE_|AHD_|ALPHA_|BATCH_|BILLBOARD_|BLEND_|CASCADE_|CHANNEL_|CLUSTERED_|COLOR_|CONTACT_|CUSTOMIZED_|DECAL_|DETAIL_|DYNAMIC_|EMISSIV|ENABLE_|ENV_|FALLOFF_|FEATURE_|FOLIAGE_|FORCE_|FOREST_|FRUSTUM_|GENERATE_|GPU_|GRASS_|HAIR_|HAS_|INSTANCE_|LIGHT_|LOD_|MATERIAL_|MEADOW_|METALLIC_|NORMAL_|PBR_|PEARL_|PENUMBRA_|POINT_|POST_|PROBE_|REFLECT_|REFLECTION_|RENDER_|SCREEN_|SHADOW_|SKIN_|SPARKLE_|SPOT_|SSS_|SUPPORT_|TERRAIN_|TEXTURE_|TWO_SIDE_|USE_|UV_|VLM_|WATER_|WITH_)/.test(upper) ||
		/_(ENABLE|MODE|TYPE|SUPPORT|QUALITY|SHADOW|LIGHT|COLOR|MAP|TEXTURE|FOG|DEBUG)$/.test(upper)
	) {
		return {
			owner: 'selector-profile-macro',
			sourceBoundary: 'active unit compile profile, nsf.preprocessorMacros, or nsf.defines',
			defaultValue: 'not assigned by audit',
			risk:
				'Do not add as a global resource default without a real unit compile profile; feature macros are often variant-specific.',
			reason: 'Name matches material, feature, pipeline, or unit-local variant macro conventions.'
		};
	}
	return {
		owner: 'source-generated-config',
		sourceBoundary: 'source include, generated header, shader stage injection, or workspace configuration review',
		defaultValue: 'not assigned by audit',
		risk:
			'Keep the diagnostic until source/config ownership is known; suppressing it could hide a real missing definition.',
		reason: 'Name does not match stable constant, compiler context, or known feature-profile conventions.'
	};
}

function buildUndefinedMacroHistogram(diagnostics: AuditDiagnostic[]): UndefinedMacroHistogram {
	const byMacro = new Map<
		string,
		{
			count: number;
			units: Set<string>;
			files: Set<string>;
			samples: AuditDiagnostic[];
		}
	>();
	for (const diagnostic of diagnostics) {
		const macro = undefinedMacroNameFromMessage(diagnostic.message);
		if (!macro) {
			continue;
		}
		let entry = byMacro.get(macro);
		if (!entry) {
			entry = {
				count: 0,
				units: new Set<string>(),
				files: new Set<string>(),
				samples: []
			};
			byMacro.set(macro, entry);
		}
		entry.count++;
		entry.units.add(normalizedPathKey(diagnostic.unit));
		entry.files.add(normalizedPathKey(diagnostic.file));
		if (entry.samples.length < 5) {
			entry.samples.push(diagnostic);
		}
	}
	const entries = Array.from(byMacro.entries())
		.map(([macro, entry]) => ({
			macro,
			count: entry.count,
			affectedUnitCount: entry.units.size,
			affectedFileCount: entry.files.size,
			samples: entry.samples,
			...classifyUndefinedMacroOwner(macro)
		}))
		.sort(
			(lhs, rhs) =>
				rhs.count - lhs.count ||
				rhs.affectedUnitCount - lhs.affectedUnitCount ||
				lhs.macro.localeCompare(rhs.macro)
		);
	const byOwnerMap = new Map<UndefinedMacroOwner, UndefinedMacroOwnerSummary>();
	for (const entry of entries) {
		const summary = byOwnerMap.get(entry.owner) ?? {
			owner: entry.owner,
			macros: 0,
			diagnostics: 0
		};
		summary.macros++;
		summary.diagnostics += entry.count;
		byOwnerMap.set(entry.owner, summary);
	}
	return {
		totalDiagnostics: entries.reduce((sum, entry) => sum + entry.count, 0),
		macroCount: entries.length,
		byOwner: Array.from(byOwnerMap.values()).sort(
			(lhs, rhs) => rhs.diagnostics - lhs.diagnostics || lhs.owner.localeCompare(rhs.owner)
		),
		entries
	};
}

function buildP14FocusProfileStatusSummary(
	evidence: P14FocusProfileEvidenceEntry[]
): Record<string, number> {
	const summary: Record<string, number> = {};
	for (const entry of evidence) {
		increment(summary, entry.status || 'unknown');
	}
	return summary;
}

function buildP14FocusMacroSummary(
	undefinedMacros: UndefinedMacroHistogram,
	profileEvidence: P14FocusProfileEvidenceEntry[]
): P14FocusMacroSummaryEntry[] {
	const undefinedByMacro = new Map(
		undefinedMacros.entries.map((entry) => [entry.macro.toUpperCase(), entry])
	);
	const profileByMacro = new Map<string, P14FocusProfileEvidenceEntry[]>();
	for (const entry of profileEvidence) {
		const key = entry.macro.toUpperCase();
		const items = profileByMacro.get(key) ?? [];
		items.push(entry);
		profileByMacro.set(key, items);
	}
	return P14_FOCUS_MACROS.map((macro) => {
		const entry = undefinedByMacro.get(macro);
		const macroEvidence = (profileByMacro.get(macro) ?? [])
			.slice()
			.sort(
				(lhs, rhs) =>
					lhs.status.localeCompare(rhs.status) ||
					lhs.unitRelativePath.localeCompare(rhs.unitRelativePath)
			);
		const ownerHint = entry ?? classifyUndefinedMacroOwner(macro);
		return {
			macro: entry?.macro ?? macro,
			diagnostics: entry?.count ?? 0,
			affectedUnitCount: entry?.affectedUnitCount ?? 0,
			affectedFileCount: entry?.affectedFileCount ?? 0,
			samples: entry?.samples ?? [],
			owner: ownerHint.owner,
			sourceBoundary: ownerHint.sourceBoundary,
			defaultValue: ownerHint.defaultValue,
			risk: ownerHint.risk,
			reason: ownerHint.reason,
			profileStatusSummary: buildP14FocusProfileStatusSummary(macroEvidence),
			profileEvidence: macroEvidence
		};
	}).sort((lhs, rhs) => lhs.macro.localeCompare(rhs.macro));
}

function buildP14CompilerContextProfileStatusSummary(
	evidence: P14CompilerContextProfileEvidenceEntry[]
): Record<string, number> {
	const summary: Record<string, number> = {};
	for (const entry of evidence) {
		increment(summary, entry.status || 'unknown');
	}
	return summary;
}

function buildP14CompilerContextStatusSummary(
	defaultPreset: MacroEvidenceMapEntry | undefined,
	effectiveConfig: MacroEvidenceMapEntry | undefined,
	defines: { name: string; value: number } | undefined
): Record<string, number> {
	const summary: Record<string, number> = {};
	if (defaultPreset) {
		increment(summary, 'default-preset-present');
	} else {
		increment(summary, 'missing-from-default-preset');
	}
	if (effectiveConfig) {
		if (effectiveConfig.value.trim().length === 0) {
			increment(summary, 'effective-config-empty');
		} else {
			increment(summary, 'effective-config-present');
		}
	} else {
		increment(summary, 'missing-from-effective-preset');
		if (defaultPreset) {
			increment(summary, 'preset-drift-candidate');
		}
	}
	if (defines) {
		increment(summary, 'defines-injected');
	} else {
		increment(summary, 'missing-from-defines');
	}
	return summary;
}

function buildP14CompilerContextMacroEvidence(
	undefinedMacros: UndefinedMacroHistogram,
	profileEvidence: P14CompilerContextProfileEvidenceEntry[],
	defaultPresetMap: Map<string, MacroEvidenceMapEntry>,
	effectiveConfigMap: Map<string, MacroEvidenceMapEntry>,
	definesMap: Map<string, { name: string; value: number }>
): P14CompilerContextMacroEvidenceEntry[] {
	const undefinedByMacro = new Map(
		undefinedMacros.entries.map((entry) => [entry.macro.toUpperCase(), entry])
	);
	const profileByMacro = new Map<string, P14CompilerContextProfileEvidenceEntry[]>();
	for (const entry of profileEvidence) {
		const key = entry.macro.toUpperCase();
		const items = profileByMacro.get(key) ?? [];
		items.push(entry);
		profileByMacro.set(key, items);
	}
	return P14L_COMPILER_CONTEXT_MACROS.map((macro) => {
		const key = macro.toUpperCase();
		const entry = undefinedByMacro.get(key);
		const defaultPreset = defaultPresetMap.get(key);
		const effectiveConfig = effectiveConfigMap.get(key);
		const defines = definesMap.get(key);
		const macroProfileEvidence = (profileByMacro.get(key) ?? [])
			.slice()
			.sort(
				(lhs, rhs) =>
					lhs.status.localeCompare(rhs.status) ||
					lhs.unitRelativePath.localeCompare(rhs.unitRelativePath)
			);
		const ownerHint = entry ?? classifyUndefinedMacroOwner(macro);
		return {
			macro: entry?.macro ?? macro,
			diagnostics: entry?.count ?? 0,
			affectedUnitCount: entry?.affectedUnitCount ?? 0,
			affectedFileCount: entry?.affectedFileCount ?? 0,
			samples: entry?.samples ?? [],
			owner: ownerHint.owner,
			sourceBoundary: ownerHint.sourceBoundary,
			defaultValue: ownerHint.defaultValue,
			risk: ownerHint.risk,
			reason: ownerHint.reason,
			defaultPresetPresent: Boolean(defaultPreset),
			defaultPresetValue: defaultPreset?.value,
			effectiveConfigPresent: Boolean(effectiveConfig),
			effectiveConfigValue: effectiveConfig?.value,
			effectiveConfigEmpty: Boolean(effectiveConfig && effectiveConfig.value.trim().length === 0),
			definesPresent: Boolean(defines),
			definesValue: defines?.value,
			statusSummary: buildP14CompilerContextStatusSummary(defaultPreset, effectiveConfig, defines),
			profileStatusSummary: buildP14CompilerContextProfileStatusSummary(macroProfileEvidence),
			profileEvidence: macroProfileEvidence
		};
	}).sort((lhs, rhs) => lhs.macro.localeCompare(rhs.macro));
}

function truncate(value: string, maxLength: number): string {
	const compact = value.replace(/\s+/g, ' ').trim();
	if (compact.length <= maxLength) {
		return compact;
	}
	return `${compact.slice(0, Math.max(0, maxLength - 3))}...`;
}

function markdownCell(value: string): string {
	return truncate(value, 140).replace(/\|/g, '\\|').replace(/\r?\n/g, ' ');
}

function compactCountMap(map: Record<string, number>): string {
	return Object.keys(map)
		.sort()
		.map((key) => `${key}=${map[key]}`)
		.join(', ');
}

function p14UnresolvedMacroCell(entry: { macro: string; unresolvedMacros: string[] }): string {
	if (entry.unresolvedMacros.includes(entry.macro)) {
		return `yes (${entry.unresolvedMacros.length})`;
	}
	if (entry.unresolvedMacros.length > 0) {
		return `other (${entry.unresolvedMacros.length})`;
	}
	return 'no';
}

function optionalNumberText(value: number | undefined): string {
	return typeof value === 'number' && Number.isFinite(value) ? String(value) : '';
}

function presentValueCell(present: boolean, value: string | number | undefined): string {
	if (!present) {
		return 'missing';
	}
	if (value === undefined || String(value).length === 0) {
		return 'present(empty)';
	}
	return `present(${value})`;
}

function increment(map: Record<string, number>, key: string): void {
	map[key] = (map[key] ?? 0) + 1;
}

function signedNumber(value: number): string {
	return value > 0 ? `+${value}` : String(value);
}

function percentText(value: number | undefined): string {
	return value === undefined ? 'n/a' : `${value >= 0 ? '+' : ''}${value.toFixed(2)}%`;
}

function appendTrendMetricTable(lines: string[], entries: AuditTrendMetric[], keyLabel: string): void {
	lines.push(`| ${keyLabel} | Baseline | Current | Delta | Delta % |`);
	lines.push('| --- | ---: | ---: | ---: | ---: |');
	for (const entry of entries) {
		lines.push(
			`| ${markdownCell(entry.key)} | ${entry.baseline} | ${entry.current} | ` +
			`${signedNumber(entry.delta)} | ${percentText(entry.percentDelta)} |`
		);
	}
}

function groupDiagnostics(diagnostics: AuditDiagnostic[]): GroupSummary[] {
	const groups = new Map<string, GroupSummary>();
	for (const diagnostic of diagnostics) {
		const key = `${diagnostic.triage}|${diagnostic.category}|${diagnostic.canonicalMessage}`;
		let group = groups.get(key);
		if (!group) {
			group = {
				key: diagnostic.canonicalMessage,
				count: 0,
				category: diagnostic.category,
				triage: diagnostic.triage,
				samples: []
			};
			groups.set(key, group);
		}
		group.count++;
		if (group.samples.length < 8) {
			group.samples.push(diagnostic);
		}
	}
	return Array.from(groups.values()).sort((lhs, rhs) => rhs.count - lhs.count);
}

function buildUnitSummaries(diagnostics: AuditDiagnostic[], fileStats: UnitFileStat[]): UnitSummary[] {
	const byUnit = new Map<string, AuditDiagnostic[]>();
	for (const diagnostic of diagnostics) {
		const items = byUnit.get(diagnostic.unit) ?? [];
		items.push(diagnostic);
		byUnit.set(diagnostic.unit, items);
	}
	const fileStatsByUnit = new Map<string, UnitFileStat[]>();
	for (const stat of fileStats) {
		const items = fileStatsByUnit.get(stat.unit) ?? [];
		items.push(stat);
		fileStatsByUnit.set(stat.unit, items);
	}
	const units = new Set<string>([...byUnit.keys(), ...fileStatsByUnit.keys()]);
	const summaries: UnitSummary[] = [];
	for (const unit of units) {
		const items = byUnit.get(unit) ?? [];
		const stats = fileStatsByUnit.get(unit) ?? [];
		const byCategory: Record<string, number> = {};
		for (const item of items) {
			increment(byCategory, item.category);
		}
		summaries.push({
			unit,
			unitRelativePath: stats[0]?.unitRelativePath ?? items[0]?.unitRelativePath ?? unit,
			filesInClosure: stats.length,
			filesScanned: stats.filter((item) => item.loaded).length,
			filesWithDiagnostics: new Set(items.map((item) => normalizedPathKey(item.file))).size,
			diagnosticsTotal: items.length,
			truncatedFiles: stats.filter((item) => item.truncated).length,
			timedOutFiles: stats.filter((item) => item.timedOut).length,
			buildElapsedMs: stats.reduce((sum, item) => sum + item.elapsedMs, 0),
			topCategories: topEntries(byCategory, 8),
			topMessages: groupDiagnostics(items).slice(0, 8)
		});
	}
	return summaries.sort(
		(lhs, rhs) =>
			rhs.diagnosticsTotal - lhs.diagnosticsTotal ||
			rhs.filesWithDiagnostics - lhs.filesWithDiagnostics ||
			lhs.unitRelativePath.localeCompare(rhs.unitRelativePath)
	);
}

function sampleDiagnostics(
	diagnostics: AuditDiagnostic[],
	config: AuditDiagnosticSampleConfig
): AuditDiagnostic[] {
	if (config.maxTotal <= 0) {
		return [];
	}
	const selected: AuditDiagnostic[] = [];
	const selectedKeys = new Set<string>();
	const groupCounts = new Map<string, number>();
	const unitCounts = new Map<string, number>();
	for (const diagnostic of diagnostics) {
		if (selected.length >= config.maxTotal) {
			break;
		}
		const groupKey = `${diagnostic.triage}|${diagnostic.category}|${diagnostic.canonicalMessage}`;
		const unitKey = normalizedPathKey(diagnostic.unit);
		if ((groupCounts.get(groupKey) ?? 0) >= config.perGroup) {
			continue;
		}
		if ((unitCounts.get(unitKey) ?? 0) >= config.perUnit) {
			continue;
		}
		const key = [
			unitKey,
			normalizedPathKey(diagnostic.file),
			diagnostic.line,
			diagnostic.character,
			diagnostic.message
		].join('|');
		if (selectedKeys.has(key)) {
			continue;
		}
		selectedKeys.add(key);
		selected.push(diagnostic);
		groupCounts.set(groupKey, (groupCounts.get(groupKey) ?? 0) + 1);
		unitCounts.set(unitKey, (unitCounts.get(unitKey) ?? 0) + 1);
	}
	return selected;
}

function topEntries(map: Record<string, number>, limit: number): Array<{ key: string; count: number }> {
	return Object.keys(map)
		.map((key) => ({ key, count: map[key] }))
		.sort((lhs, rhs) => rhs.count - lhs.count || lhs.key.localeCompare(rhs.key))
		.slice(0, limit);
}

function buildMarkdownReport(report: any): string {
	const lines: string[] = [];
	lines.push('# Real Workspace Diagnostics Audit');
	lines.push('');
	lines.push(`- Generated: ${report.generatedAt}`);
	if (report.runLabel) {
		lines.push(`- Report label: ${report.runLabel}`);
	}
	lines.push(`- Workspace: ${report.workspaceTarget}`);
	lines.push(`- Diagnostics mode: ${report.settings.diagnosticsMode}`);
	lines.push(`- Search roots: ${report.settings.searchRoots.join(', ')}`);
	if (report.summary.unitsDiscovered !== undefined) {
		lines.push(`- NSF units discovered: ${report.summary.unitsDiscovered}`);
		if (report.summary.unitOffset !== undefined || report.summary.unitLimit !== undefined) {
			lines.push(`- NSF unit batch offset: ${report.summary.unitOffset ?? 0}`);
			lines.push(`- NSF unit batch limit: ${report.summary.unitLimit ?? 0}`);
		}
		lines.push(`- NSF units scanned: ${report.summary.unitsScanned}`);
		lines.push(`- NSF units with diagnostics: ${report.summary.unitsWithDiagnostics}`);
		lines.push(`- Unit file visits: ${report.summary.unitFileVisits}`);
	}
	lines.push(`- Files discovered: ${report.summary.filesDiscovered}`);
	lines.push(`- Files scanned: ${report.summary.filesScanned}`);
	lines.push(`- Files with diagnostics: ${report.summary.filesWithDiagnostics}`);
	lines.push(`- Diagnostics: ${report.summary.diagnosticsTotal}`);
	lines.push(`- Diagnostic wait timeouts: ${report.summary.waitTimeouts}`);
	if (report.summary.truncatedFiles !== undefined) {
		lines.push(`- Truncated file builds: ${report.summary.truncatedFiles}`);
		lines.push(`- Timed-out file builds: ${report.summary.timedOutFiles}`);
	}
	if (report.summary.prerequisiteSkippedTotal !== undefined) {
		lines.push(`- Prerequisite-skipped semantic rules: ${report.summary.prerequisiteSkippedTotal}`);
		const skippedByReason = report.summary.prerequisiteSkippedByReason ?? {};
		const nonZeroReasons = Object.keys(skippedByReason)
			.filter((key) => key !== 'total' && numericValue(skippedByReason[key]) > 0)
			.map((key) => `${key}=${numericValue(skippedByReason[key])}`);
		if (nonZeroReasons.length > 0) {
			lines.push(`- Prerequisite skip reasons: ${nonZeroReasons.join(', ')}`);
		}
	}
	const macroHealth = report.summary.macroHealth as MacroHealthMetrics | undefined;
	if (macroHealth) {
		lines.push(`- Macro synthesized-zero events: ${numericValue(macroHealth.synthesizedZeroEvents)}`);
		lines.push(`- Macro branch merges: ${numericValue(macroHealth.branchMergeCount)}`);
		lines.push(`- Macro expansion warnings: ${numericValue(macroHealth.expansionWarningDiagnosticCount)}`);
	}
	const trend = report.trendComparison as AuditTrendComparison | undefined;
	if (trend) {
		lines.push('');
		lines.push('## Baseline Trend');
		lines.push('');
		lines.push(`- Baseline: ${trend.baselinePath}`);
		if (trend.baselineGeneratedAt) {
			lines.push(`- Baseline generated: ${trend.baselineGeneratedAt}`);
		}
		if (trend.warnings.length > 0) {
			lines.push(`- Warnings: ${trend.warnings.join(' ')}`);
		}
		lines.push('');
		lines.push('### Summary Delta');
		lines.push('');
		appendTrendMetricTable(lines, trend.summary, 'Metric');
		lines.push('');
		lines.push('### Triage Delta');
		lines.push('');
		appendTrendMetricTable(lines, trend.triage, 'Triage');
		lines.push('');
		lines.push('### Category Delta');
		lines.push('');
		appendTrendMetricTable(lines, trend.category, 'Category');
		lines.push('');
		lines.push('### Top Message Delta');
		lines.push('');
		lines.push('| Message | Triage | Category | Baseline | Current | Delta | Delta % |');
		lines.push('| --- | --- | --- | ---: | ---: | ---: | ---: |');
		for (const entry of trend.topMessages) {
			lines.push(
				`| ${markdownCell(entry.key)} | ${markdownCell(entry.triage)} | ` +
				`${markdownCell(entry.category)} | ${entry.baseline} | ${entry.current} | ` +
				`${signedNumber(entry.delta)} | ${percentText(entry.percentDelta)} |`
			);
		}
	}
	lines.push('');
	lines.push('## Triage Summary');
	lines.push('');
	lines.push('| Triage | Count |');
	lines.push('| --- | ---: |');
	for (const entry of topEntries(report.counts.byTriage, 20)) {
		lines.push(`| ${markdownCell(entry.key)} | ${entry.count} |`);
	}
	lines.push('');
	lines.push('## Category Summary');
	lines.push('');
	lines.push('| Category | Count |');
	lines.push('| --- | ---: |');
	for (const entry of topEntries(report.counts.byCategory, 30)) {
		lines.push(`| ${markdownCell(entry.key)} | ${entry.count} |`);
	}
	const undefinedMacros = report.undefinedMacros as UndefinedMacroHistogram | undefined;
	if (undefinedMacros && undefinedMacros.totalDiagnostics > 0) {
		lines.push('');
		lines.push('## Undefined Macro Histogram');
		lines.push('');
		lines.push(`- Undefined macro diagnostics: ${undefinedMacros.totalDiagnostics}`);
		lines.push(`- Unique undefined macros: ${undefinedMacros.macroCount}`);
		lines.push('');
		lines.push('### Owner Summary');
		lines.push('');
		lines.push('| Owner | Macros | Diagnostics |');
		lines.push('| --- | ---: | ---: |');
		for (const entry of undefinedMacros.byOwner) {
			lines.push(`| ${markdownCell(entry.owner)} | ${entry.macros} | ${entry.diagnostics} |`);
		}
		lines.push('');
		lines.push('### Top Undefined Macros');
		lines.push('');
		lines.push('| Diagnostics | Units | Files | Macro | Owner | Default | Source / Risk | Sample |');
		lines.push('| ---: | ---: | ---: | --- | --- | --- | --- | --- |');
		for (const entry of undefinedMacros.entries.slice(0, 80)) {
			const sample = entry.samples[0];
			const sampleText = sample
				? `${sample.unitRelativePath} -> ${sample.relativePath}:${sample.line}:${sample.character} ${sample.lineText}`
				: '';
			lines.push(
				`| ${entry.count} | ${entry.affectedUnitCount} | ${entry.affectedFileCount} | ` +
				`${markdownCell(entry.macro)} | ${markdownCell(entry.owner)} | ` +
				`${markdownCell(entry.defaultValue)} | ` +
				`${markdownCell(`${entry.sourceBoundary}; ${entry.risk}`)} | ${markdownCell(sampleText)} |`
			);
		}
	}
	const compilerContextMacroEvidence =
		report.compilerContextMacroEvidence as P14CompilerContextMacroEvidenceEntry[] | undefined;
	if (Array.isArray(compilerContextMacroEvidence) && compilerContextMacroEvidence.length > 0) {
		lines.push('');
		lines.push('## P14L Compiler Context Macro Evidence');
		lines.push('');
		lines.push('| Macro | Remaining undefined diagnostics | Units | Files | Default preset | Effective config | Defines | Profile statuses | Status | Sample |');
		lines.push('| --- | ---: | ---: | ---: | --- | --- | --- | --- | --- | --- |');
		for (const entry of compilerContextMacroEvidence) {
			const sample = entry.samples[0];
			const sampleText = sample
				? `${sample.unitRelativePath} -> ${sample.relativePath}:${sample.line}:${sample.character} ${sample.lineText}`
				: '';
			lines.push(
				`| ${markdownCell(entry.macro)} | ${entry.diagnostics} | ${entry.affectedUnitCount} | ` +
				`${entry.affectedFileCount} | ` +
				`${markdownCell(presentValueCell(entry.defaultPresetPresent, entry.defaultPresetValue))} | ` +
				`${markdownCell(presentValueCell(entry.effectiveConfigPresent, entry.effectiveConfigValue))} | ` +
				`${markdownCell(presentValueCell(entry.definesPresent, entry.definesValue))} | ` +
				`${markdownCell(compactCountMap(entry.profileStatusSummary))} | ` +
				`${markdownCell(compactCountMap(entry.statusSummary))} | ${markdownCell(sampleText)} |`
			);
		}
		const profileEvidenceRows = compilerContextMacroEvidence.flatMap((entry) => entry.profileEvidence);
		if (profileEvidenceRows.length > 0) {
			lines.push('');
			lines.push('### P14L Compiler Context Profile Evidence');
			lines.push('');
			lines.push(`- Rows shown: ${Math.min(profileEvidenceRows.length, 200)} / ${profileEvidenceRows.length}`);
			lines.push('');
			lines.push('| Macro | Status | Unit | Source kind | Rows | Selected | Hint source | Unresolved | Injected value | Source path |');
			lines.push('| --- | --- | --- | --- | ---: | ---: | --- | --- | ---: | --- |');
			for (const evidence of profileEvidenceRows.slice(0, 200)) {
				lines.push(
					`| ${markdownCell(evidence.macro)} | ${markdownCell(evidence.status)} | ` +
					`${markdownCell(evidence.unitRelativePath)} | ` +
					`${markdownCell(evidence.profileSourceKind ?? '')} | ` +
					`${numericValue(evidence.profileTotalRowCount)} | ` +
					`${numericValue(evidence.profileSelectedRowCount)} | ` +
					`${markdownCell(evidence.profileSelectionHintSourcePath ?? '')} | ` +
					`${markdownCell(p14UnresolvedMacroCell(evidence))} | ` +
					`${optionalNumberText(evidence.injectedValue)} | ` +
					`${markdownCell(evidence.profileSourcePath ?? '')} |`
				);
			}
		}
	}
	if (macroHealth) {
		lines.push('');
		lines.push('## Macro Health');
		lines.push('');
		lines.push('| Metric | Count |');
		lines.push('| --- | ---: |');
		const metricLabels: Array<[keyof MacroHealthMetrics, string]> = [
			['initialConfiguredMacroCount', 'Initial configured macros'],
			['initialArtDefaultZeroMacroCount', 'Initial #art default-zero macros'],
			['initialCompilerPrivateConstantCount', 'Initial compiler private constants'],
			['initialCompilerMacroSnapshotCount', 'Initial compiler macro snapshot entries'],
			['initialNumericDefineCount', 'Initial numeric defines'],
			['initialMacroCount', 'Initial macro state entries'],
			['sourceDefineEvents', 'Source define events'],
			['ifndefDefaultDefineEvents', '#ifndef default define events'],
			['sourceUndefEvents', 'Source undef events'],
			['synthesizedZeroEvents', 'Synthesized zero events'],
			['conditionDiagnosticCount', 'Preprocessor condition diagnostics'],
			['undefinedMacroDiagnosticCount', 'Undefined macro diagnostics'],
			['expansionWarningDiagnosticCount', 'Macro expansion warnings'],
			['inactiveBranchDiagnosticCount', 'Inactive branch diagnostics'],
			['branchMergeCount', 'Branch merges'],
			['activeIncludeCount', 'Active include references']
		];
		for (const [key, label] of metricLabels) {
			lines.push(`| ${markdownCell(label)} | ${numericValue(macroHealth[key])} |`);
		}
		const focus = report.macroFocus as P14FocusMacroSummaryEntry[] | undefined;
		if (Array.isArray(focus) && focus.length > 0) {
			lines.push('');
			lines.push('### P14 Focus Macros');
			lines.push('');
			lines.push('| Macro | Remaining undefined diagnostics | Units | Files | Owner | Profile statuses | Source boundary | Sample |');
			lines.push('| --- | ---: | ---: | ---: | --- | --- | --- | --- |');
			for (const entry of focus) {
				const sample = entry.samples[0];
				const sampleText = sample
					? `${sample.unitRelativePath} -> ${sample.relativePath}:${sample.line}:${sample.character} ${sample.lineText}`
					: '';
				lines.push(
					`| ${markdownCell(entry.macro)} | ${entry.diagnostics} | ${entry.affectedUnitCount} | ` +
					`${entry.affectedFileCount} | ${markdownCell(entry.owner)} | ` +
					`${markdownCell(compactCountMap(entry.profileStatusSummary))} | ` +
					`${markdownCell(entry.sourceBoundary)} | ${markdownCell(sampleText)} |`
				);
			}
			const profileEvidenceRows = focus.flatMap((entry) => entry.profileEvidence);
			if (profileEvidenceRows.length > 0) {
				lines.push('');
				lines.push('### P14 Focus Profile Evidence');
				lines.push('');
				lines.push(`- Rows shown: ${Math.min(profileEvidenceRows.length, 150)} / ${profileEvidenceRows.length}`);
				lines.push('');
				lines.push('| Macro | Status | Unit | Source kind | Rows | Selected | Hint source | Unresolved | Injected value | #art default | Source path |');
				lines.push('| --- | --- | --- | --- | ---: | ---: | --- | --- | ---: | --- | --- |');
				for (const evidence of profileEvidenceRows.slice(0, 150)) {
					const artDefaultText = evidence.artDefaultZero
						? `${evidence.artDefaultType ?? ''} ${evidence.artDefaultSourcePath ?? ''}:${optionalNumberText(evidence.artDefaultSourceLine)}`
						: '';
					lines.push(
						`| ${markdownCell(evidence.macro)} | ${markdownCell(evidence.status)} | ` +
						`${markdownCell(evidence.unitRelativePath)} | ` +
						`${markdownCell(evidence.profileSourceKind ?? '')} | ` +
						`${numericValue(evidence.profileTotalRowCount)} | ` +
						`${numericValue(evidence.profileSelectedRowCount)} | ` +
						`${markdownCell(evidence.profileSelectionHintSourcePath ?? '')} | ` +
						`${markdownCell(p14UnresolvedMacroCell(evidence))} | ` +
						`${optionalNumberText(evidence.injectedValue)} | ` +
						`${markdownCell(artDefaultText)} | ` +
						`${markdownCell(evidence.profileSourcePath ?? '')} |`
					);
				}
			}
		}
	}
	lines.push('');
	lines.push('## Top Message Groups');
	lines.push('');
	lines.push('| Count | Triage | Category | Message | Sample |');
	lines.push('| ---: | --- | --- | --- | --- |');
	for (const group of report.groups.slice(0, 50) as GroupSummary[]) {
		const sample = group.samples[0];
		const sampleText = sample
			? `${sample.unitRelativePath} -> ${sample.relativePath}:${sample.line}:${sample.character} ${sample.lineText}`
			: '';
		lines.push(
			`| ${group.count} | ${markdownCell(group.triage)} | ${markdownCell(group.category)} | ` +
			`${markdownCell(group.key)} | ${markdownCell(sampleText)} |`
		);
	}
	if (Array.isArray(report.units) && report.units.length > 0) {
		lines.push('');
		lines.push('## Top Units');
		lines.push('');
		lines.push('| Diagnostics | Files | Unit | Top Categories | Top Message |');
		lines.push('| ---: | ---: | --- | --- | --- |');
		for (const unit of report.units.slice(0, 50) as UnitSummary[]) {
			const categories = unit.topCategories
				.slice(0, 4)
				.map((entry) => `${entry.key}=${entry.count}`)
				.join(', ');
			const message = unit.topMessages[0]
				? `${unit.topMessages[0].key} (${unit.topMessages[0].count})`
				: '';
			lines.push(
				`| ${unit.diagnosticsTotal} | ${unit.filesWithDiagnostics}/${unit.filesScanned} | ` +
				`${markdownCell(unit.unitRelativePath)} | ${markdownCell(categories)} | ${markdownCell(message)} |`
			);
		}
	}
	lines.push('');
	lines.push('## Top Files');
	lines.push('');
	lines.push('| Diagnostics | File |');
	lines.push('| ---: | --- |');
	for (const entry of topEntries(report.counts.byFile, 50)) {
		lines.push(`| ${entry.count} | ${markdownCell(entry.key)} |`);
	}
	lines.push('');
	lines.push('## Notes');
	lines.push('');
	lines.push('- `likely-plugin-limitation` is a triage hint for diagnostics that depend heavily on incomplete language modeling, include context, type inference, overload resolution, or macro expansion.');
	lines.push('- `likely-real-source` is still not a compiler verdict; it means the diagnostic is structural or source-rule-like enough to review as a real issue first.');
	lines.push('- `check-config-or-source` usually means include roots, generated files, or missing files must be checked before blaming either source or plugin.');
	lines.push('- Unit-based audit counts diagnostics per NSF unit, so a shared include can be counted once for each unit that uses it.');
	return `${lines.join('\n')}\n`;
}

function writeReport(report: any): {
	jsonPath: string;
	markdownPath: string;
	labeledJsonPath?: string;
	labeledMarkdownPath?: string;
} {
	fs.mkdirSync(REPORT_DIR, { recursive: true });
	const stamp = new Date().toISOString().replace(/[:.]/g, '-');
	const jsonPath = path.join(REPORT_DIR, `real-workspace-diagnostics-audit.${stamp}.json`);
	const markdownPath = path.join(REPORT_DIR, `real-workspace-diagnostics-audit.${stamp}.md`);
	const latestJsonPath = path.join(REPORT_DIR, 'real-workspace-diagnostics-audit.latest.json');
	const latestMarkdownPath = path.join(REPORT_DIR, 'real-workspace-diagnostics-audit.latest.md');
	const reportLabel = sanitizeReportLabel(typeof report.runLabel === 'string' ? report.runLabel : '');
	const labeledJsonPath = reportLabel
		? path.join(REPORT_DIR, `real-workspace-diagnostics-audit.${reportLabel}.json`)
		: undefined;
	const labeledMarkdownPath = reportLabel
		? path.join(REPORT_DIR, `real-workspace-diagnostics-audit.${reportLabel}.md`)
		: undefined;
	const json = `${JSON.stringify(report, null, 2)}\n`;
	const markdown = buildMarkdownReport(report);
	fs.writeFileSync(jsonPath, json, 'utf8');
	fs.writeFileSync(markdownPath, markdown, 'utf8');
	fs.writeFileSync(latestJsonPath, json, 'utf8');
	fs.writeFileSync(latestMarkdownPath, markdown, 'utf8');
	if (labeledJsonPath && labeledMarkdownPath) {
		fs.writeFileSync(labeledJsonPath, json, 'utf8');
		fs.writeFileSync(labeledMarkdownPath, markdown, 'utf8');
	}
	return { jsonPath, markdownPath, labeledJsonPath, labeledMarkdownPath };
}

realDescribe('NSF real workspace diagnostics audit', () => {
	it('collects and classifies diagnostics by NSF unit include closure', async function () {
		this.timeout(readIntEnv('NSF_REAL_DIAGNOSTICS_TIMEOUT_MS', 1800000, 60000, REAL_DIAGNOSTICS_TIMEOUT_MAX_MS));

		const extensions = configuredShaderExtensions();
		const maxUnits = readIntEnv(
			'NSF_REAL_DIAGNOSTICS_MAX_UNITS',
			readIntEnv('NSF_REAL_DIAGNOSTICS_MAX_FILES', 0, 0, 100000),
			0,
			100000
		);
		const unitOffset = readIntEnv('NSF_REAL_DIAGNOSTICS_UNIT_OFFSET', 0, 0, 100000);
		const closureLimit = readIntEnv('NSF_REAL_DIAGNOSTICS_CLOSURE_LIMIT', 1024, 1, 10000);
		const diagnosticTimeBudgetMs = readIntEnv('NSF_REAL_DIAGNOSTICS_BUILD_BUDGET_MS', 5000, 30, 120000);
		const diagnosticMaxItems = readIntEnv('NSF_REAL_DIAGNOSTICS_BUILD_MAX_ITEMS', 2400, 20, 100000);
		const sampleConfig: AuditDiagnosticSampleConfig = {
			perGroup: readIntEnv('NSF_REAL_DIAGNOSTICS_SAMPLE_PER_GROUP', 20, 0, 1000),
			perUnit: readIntEnv('NSF_REAL_DIAGNOSTICS_SAMPLE_PER_UNIT', 80, 0, 5000),
			maxTotal: readIntEnv('NSF_REAL_DIAGNOSTICS_SAMPLE_MAX_TOTAL', 5000, 0, 100000)
		};
		const reportLabel = sanitizeReportLabel(readStringEnv('NSF_REAL_DIAGNOSTICS_REPORT_LABEL'));
		const searchRoots = configuredAuditSearchRoots();
		const { allUnits, units } = await findNsfUnits(maxUnits, unitOffset, searchRoots);
		assert.ok(allUnits.length > 0, 'Expected .nsf units in real workspace.');
		assert.ok(
			units.length > 0,
			`Expected .nsf units in selected audit batch. Discovered ${allUnits.length}, offset ${unitOffset}, max ${maxUnits}.`
		);

		const activationDocument = await vscode.workspace.openTextDocument(units[0]);
		await vscode.window.showTextDocument(activationDocument, { preview: false });
		await waitForCommandAvailable('nsf._getInternalStatus');
		await waitForClientReady('client ready before real workspace diagnostics audit');
		const seededPreprocessorMacroCount = await seedAuditPreprocessorMacrosIfMissing();
		const compilerContextDefaultPresetMap = await readServerPreprocessorMacroPresetMap();
		const compilerContextEffectiveConfigMap = readEffectivePreprocessorMacroSettingsMap();
		const compilerContextDefinesMap = readConfiguredDefinesMap();
		await waitForIndexingIdle('real workspace diagnostics audit initial indexing idle');

		const diagnostics: AuditDiagnostic[] = [];
		const fileStats: UnitFileStat[] = [];
		const p14FocusProfileEvidence: P14FocusProfileEvidenceEntry[] = [];
		const p14CompilerContextProfileEvidence: P14CompilerContextProfileEvidenceEntry[] = [];
		const fileErrors: Array<{ unit: string; file: string; error: string }> = [];
		const discoveredFiles = new Set<string>();

		for (let unitIndex = 0; unitIndex < units.length; unitIndex++) {
			const unit = units[unitIndex];
			const unitRelativePath = relativePathForUri(unit);
			if (unitIndex > 0 && unitIndex % 25 === 0) {
				console.log(`[real-diagnostics-audit] scanned ${unitIndex}/${units.length} nsf units`);
			}
			try {
				await vscode.commands.executeCommand('nsf._setActiveUnitForTests', unit.toString());
				await vscode.commands.executeCommand('nsf._sendServerRequest', { method: 'nsf/ping', params: {} });
				const p14RuntimeDebugDocument = await findP14FocusRuntimeDebugDocument(unit);
				p14FocusProfileEvidence.push(
					...buildP14FocusProfileEvidenceForRuntimeDebug(unit, unitRelativePath, p14RuntimeDebugDocument)
				);
				p14CompilerContextProfileEvidence.push(
					...buildP14CompilerContextProfileEvidenceForRuntimeDebug(
						unit,
						unitRelativePath,
						p14RuntimeDebugDocument
					)
				);
				const closure = await getUnitIncludeClosure(unit, closureLimit);
				for (const fileUri of closure) {
					discoveredFiles.add(normalizedPathKey(fileUri.fsPath));
					const relativePath = relativePathForUri(fileUri);
					const extension = path.extname(fileUri.fsPath).toLowerCase();
					let lines: string[] = [];
					try {
						lines = fs.readFileSync(fileUri.fsPath, 'utf8').replace(/\r/g, '').split('\n');
					} catch {
						lines = [];
					}
					try {
						const response = await vscode.commands.executeCommand<DebugBuildDiagnosticsResponse>(
							'nsf._sendServerRequest',
							{
								method: 'nsf/_debugBuildDiagnostics',
								params: {
									uri: fileUri.toString(),
									activeUnitUri: unit.toString(),
									timeBudgetMs: diagnosticTimeBudgetMs,
									maxItems: diagnosticMaxItems,
									expensiveRules: true,
									compilerPrivateConstantCacheScope: `real-diagnostics-audit:${unit.toString()}`
								}
							}
						);
						const currentDiagnostics = Array.isArray(response?.diagnostics) ? response.diagnostics : [];
						fileStats.push({
							unit: unit.fsPath,
							unitRelativePath,
							file: fileUri.fsPath,
							relativePath,
							extension,
							loaded: Boolean(response?.loaded),
							diagnosticsTotal: currentDiagnostics.length,
							truncated: Boolean(response?.truncated),
							timedOut: Boolean(response?.timedOut),
							heavyRulesSkipped: Boolean(response?.heavyRulesSkipped),
							indeterminateTotal: Math.max(0, Math.trunc(response?.indeterminateTotal ?? 0)),
							prerequisiteSkippedTotal: prerequisiteSkipNumber(
								response?.prerequisiteSkips,
								'total'
							),
							prerequisiteSkips: response?.prerequisiteSkips ?? {},
							macroHealth: response?.macroHealth ?? {},
							elapsedMs: Math.max(0, response?.elapsedMs ?? 0)
						});
						if (!response?.loaded) {
							fileErrors.push({
								unit: unit.fsPath,
								file: fileUri.fsPath,
								error: 'debug diagnostics did not load file text'
							});
							continue;
						}
						for (const diagnostic of currentDiagnostics) {
							const message = diagnosticMessageFromWire(diagnostic);
							const code = diagnosticCodeTextFromWire(diagnostic);
							const start = diagnosticRangeStartFromWire(diagnostic);
							const lineText = lines[start.line]?.trim() ?? '';
							const nextLineText = lines[start.line + 1]?.trim() ?? '';
							const classification = classifyDiagnostic(message, code, lineText, nextLineText, extension);
							diagnostics.push({
								unit: unit.fsPath,
								unitRelativePath,
								file: fileUri.fsPath,
								workspaceFolder: workspaceFolderLabel(fileUri),
								relativePath,
								extension,
								activeUnit: unitRelativePath,
								severity: severityNameFromWire((diagnostic as { severity?: unknown })?.severity),
								source: diagnosticSourceFromWire(diagnostic),
								code,
								message,
								canonicalMessage: canonicalizeMessage(message),
								category: classification.category,
								triage: classification.triage,
								line: start.line + 1,
								character: start.character + 1,
								lineText,
								nextLineText,
								reasonCode: diagnosticReasonCodeFromWire(diagnostic)
							});
						}
					} catch (error) {
						fileErrors.push({
							unit: unit.fsPath,
							file: fileUri.fsPath,
							error: error instanceof Error ? error.message : String(error)
						});
					}
				}
			} catch (error) {
				fileErrors.push({
					unit: unit.fsPath,
					file: unit.fsPath,
					error: error instanceof Error ? error.message : String(error)
				});
			}
		}

		await vscode.commands.executeCommand('nsf._clearActiveUnitForTests');
		await waitForIndexingIdle('real workspace diagnostics audit final indexing idle');

		const counts = {
			bySeverity: {} as Record<string, number>,
			bySource: {} as Record<string, number>,
			byCode: {} as Record<string, number>,
			byCategory: {} as Record<string, number>,
			byTriage: {} as Record<string, number>,
			byExtension: {} as Record<string, number>,
			byWorkspaceFolder: {} as Record<string, number>,
			byFile: {} as Record<string, number>,
			byUnit: {} as Record<string, number>
		};
		for (const diagnostic of diagnostics) {
			increment(counts.bySeverity, diagnostic.severity);
			increment(counts.bySource, diagnostic.source || '(none)');
			increment(counts.byCode, diagnostic.code || '(none)');
			increment(counts.byCategory, diagnostic.category);
			increment(counts.byTriage, diagnostic.triage);
			increment(counts.byExtension, diagnostic.extension || '(none)');
			increment(counts.byWorkspaceFolder, diagnostic.workspaceFolder || '(none)');
			increment(counts.byFile, diagnostic.relativePath);
			increment(counts.byUnit, diagnostic.unitRelativePath);
		}

		const unitsWithDiagnostics = new Set(diagnostics.map((item) => normalizedPathKey(item.unit))).size;
		const prerequisiteSkippedByReason = aggregatePrerequisiteSkips(fileStats);
		const macroHealth = aggregateMacroHealth(fileStats);
		const undefinedMacros = buildUndefinedMacroHistogram(diagnostics);
		const report = {
			generatedAt: new Date().toISOString(),
			runLabel: reportLabel,
			workspaceTarget: process.env.NSF_TEST_WORKSPACE_PATH ?? '',
			workspaceFolders: (vscode.workspace.workspaceFolders ?? []).map((folder) => ({
				name: folder.name,
				path: folder.uri.fsPath
			})),
			settings: {
				diagnosticsMode: vscode.workspace.getConfiguration('nsf').get<string>('diagnostics.mode', 'balanced'),
				shaderFileExtensions: extensions,
				searchRoots,
				intellisionPath: vscode.workspace.getConfiguration('nsf').get<string[]>('intellisionPath', []),
				preprocessorMacroCount: Object.keys(
					vscode.workspace.getConfiguration('nsf').get<Record<string, unknown>>('preprocessorMacros', {})
				).length,
				preprocessorMacrosSeededForAudit: seededPreprocessorMacroCount,
				unitOffset,
				unitLimit: maxUnits,
				unitClosureLimit: closureLimit,
				diagnosticBuildBudgetMs: diagnosticTimeBudgetMs,
				diagnosticBuildMaxItems: diagnosticMaxItems,
				diagnosticSample: sampleConfig
			},
			summary: {
				unitsDiscovered: allUnits.length,
				unitOffset,
				unitLimit: maxUnits,
				unitsScanned: units.length,
				unitsWithDiagnostics,
				unitFileVisits: fileStats.length,
				filesDiscovered: discoveredFiles.size,
				filesScanned: new Set(fileStats.filter((item) => item.loaded).map((item) => normalizedPathKey(item.file))).size,
				filesWithDiagnostics: new Set(diagnostics.map((item) => normalizedPathKey(item.file))).size,
				diagnosticsTotal: diagnostics.length,
				waitTimeouts: 0,
				truncatedFiles: fileStats.filter((item) => item.truncated).length,
				timedOutFiles: fileStats.filter((item) => item.timedOut).length,
				heavyRulesSkippedFiles: fileStats.filter((item) => item.heavyRulesSkipped).length,
				prerequisiteSkippedTotal: prerequisiteSkippedByReason.total ?? 0,
				prerequisiteSkippedByReason,
				macroHealth,
				fileErrors: fileErrors.length
			},
			counts,
			undefinedMacros,
			macroFocus: buildP14FocusMacroSummary(undefinedMacros, p14FocusProfileEvidence),
			compilerContextMacroEvidence: buildP14CompilerContextMacroEvidence(
				undefinedMacros,
				p14CompilerContextProfileEvidence,
				compilerContextDefaultPresetMap,
				compilerContextEffectiveConfigMap,
				compilerContextDefinesMap
			),
			groups: groupDiagnostics(diagnostics),
			units: buildUnitSummaries(diagnostics, fileStats),
			fileStats,
			fileErrors,
			diagnosticSamples: sampleDiagnostics(diagnostics, sampleConfig)
		};

		const baseline = loadBaselineReport(maxUnits, unitOffset);
		if (baseline) {
			(report as any).trendComparison = buildTrendComparison(report, baseline.report, baseline.baselinePath);
		}

		const paths = writeReport(report);
		console.log(`[real-diagnostics-audit] wrote ${paths.jsonPath}`);
		console.log(`[real-diagnostics-audit] wrote ${paths.markdownPath}`);
		if (paths.labeledJsonPath && paths.labeledMarkdownPath) {
			console.log(`[real-diagnostics-audit] wrote ${paths.labeledJsonPath}`);
			console.log(`[real-diagnostics-audit] wrote ${paths.labeledMarkdownPath}`);
		}

		assert.strictEqual(fileErrors.length, 0, `Failed to scan files: ${JSON.stringify(fileErrors.slice(0, 5))}`);
	});
});
