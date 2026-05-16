import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

type RuntimeDebugResponse = {
	documents?: Array<{
		uri?: string;
		exists?: boolean;
		deferredHasFullDiagnostics?: boolean;
		lastDiagnosticsPublishLayer?: string;
	}>;
};

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
	elapsedMs?: number;
	diagnostics?: unknown[];
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
		'filesDiscovered',
		'filesScanned',
		'filesWithDiagnostics',
		'diagnosticsTotal',
		'truncatedFiles',
		'timedOutFiles',
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

function defaultBaselineCandidates(maxUnits: number): string[] {
	const candidates: string[] = [];
	if (maxUnits === 5) {
		candidates.push(DEFAULT_SMOKE_BASELINE_REPORT);
	}
	if (maxUnits === 50) {
		candidates.push(DEFAULT_TREND_BASELINE_REPORT);
	}
	candidates.push(DEFAULT_BASELINE_REPORT);
	return candidates.map((name) => path.join(REPORT_DIR, name));
}

function loadBaselineReport(maxUnits: number): { baselinePath: string; report: any } | undefined {
	const rawPath = readStringEnv('NSF_REAL_DIAGNOSTICS_BASELINE_JSON');
	const lowered = rawPath.toLowerCase();
	if (lowered === '0' || lowered === 'false' || lowered === 'none') {
		return undefined;
	}
	const explicit = rawPath.length > 0;
	const candidates = explicit ? [path.resolve(rawPath)] : defaultBaselineCandidates(maxUnits);
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

async function findNsfUnits(maxUnits: number, searchRoots: string[]): Promise<vscode.Uri[]> {
	const units = await findShaderFiles(['.nsf'], maxUnits, searchRoots);
	return units;
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
		this.timeout(readIntEnv('NSF_REAL_DIAGNOSTICS_TIMEOUT_MS', 1800000, 60000, 7200000));

		const extensions = configuredShaderExtensions();
		const maxUnits = readIntEnv(
			'NSF_REAL_DIAGNOSTICS_MAX_UNITS',
			readIntEnv('NSF_REAL_DIAGNOSTICS_MAX_FILES', 0, 0, 100000),
			0,
			100000
		);
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
		const units = await findNsfUnits(maxUnits, searchRoots);
		assert.ok(units.length > 0, 'Expected .nsf units in real workspace.');

		const activationDocument = await vscode.workspace.openTextDocument(units[0]);
		await vscode.window.showTextDocument(activationDocument, { preview: false });
		await waitForCommandAvailable('nsf._getInternalStatus');
		await waitForClientReady('client ready before real workspace diagnostics audit');
		const seededPreprocessorMacroCount = await seedAuditPreprocessorMacrosIfMissing();
		await waitForIndexingIdle('real workspace diagnostics audit initial indexing idle');

		const diagnostics: AuditDiagnostic[] = [];
		const fileStats: UnitFileStat[] = [];
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
									expensiveRules: true
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
				unitClosureLimit: closureLimit,
				diagnosticBuildBudgetMs: diagnosticTimeBudgetMs,
				diagnosticBuildMaxItems: diagnosticMaxItems,
				diagnosticSample: sampleConfig
			},
			summary: {
				unitsDiscovered: units.length,
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
				fileErrors: fileErrors.length
			},
			counts,
			groups: groupDiagnostics(diagnostics),
			units: buildUnitSummaries(diagnostics, fileStats),
			fileStats,
			fileErrors,
			diagnosticSamples: sampleDiagnostics(diagnostics, sampleConfig)
		};

		const baseline = loadBaselineReport(maxUnits);
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
