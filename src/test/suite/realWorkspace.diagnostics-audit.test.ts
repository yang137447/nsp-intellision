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

const testMode = process.env.NSF_TEST_MODE ?? 'repo';
const auditEnabled = process.env.NSF_REAL_DIAGNOSTICS_AUDIT === '1';
const realDescribe = testMode === 'real' && auditEnabled ? describe : describe.skip;

const DEFAULT_EXTENSIONS = ['.nsf', '.hlsl'];
const REPORT_DIR = path.resolve(__dirname, '..', '..', '..', 'out', 'test', 'diagnostics-audit');

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
		return { category: 'numeric-literal', triage: 'likely-real-source' };
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
	lines.push(`- Workspace: ${report.workspaceTarget}`);
	lines.push(`- Diagnostics mode: ${report.settings.diagnosticsMode}`);
	lines.push(`- Search roots: ${report.settings.searchRoots.join(', ')}`);
	lines.push(`- Files discovered: ${report.summary.filesDiscovered}`);
	lines.push(`- Files scanned: ${report.summary.filesScanned}`);
	lines.push(`- Files with diagnostics: ${report.summary.filesWithDiagnostics}`);
	lines.push(`- Diagnostics: ${report.summary.diagnosticsTotal}`);
	lines.push(`- Diagnostic wait timeouts: ${report.summary.waitTimeouts}`);
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
			? `${sample.relativePath}:${sample.line}:${sample.character} ${sample.lineText}`
			: '';
		lines.push(
			`| ${group.count} | ${markdownCell(group.triage)} | ${markdownCell(group.category)} | ` +
			`${markdownCell(group.key)} | ${markdownCell(sampleText)} |`
		);
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
	return `${lines.join('\n')}\n`;
}

function writeReport(report: any): { jsonPath: string; markdownPath: string } {
	fs.mkdirSync(REPORT_DIR, { recursive: true });
	const stamp = new Date().toISOString().replace(/[:.]/g, '-');
	const jsonPath = path.join(REPORT_DIR, `real-workspace-diagnostics-audit.${stamp}.json`);
	const markdownPath = path.join(REPORT_DIR, `real-workspace-diagnostics-audit.${stamp}.md`);
	const latestJsonPath = path.join(REPORT_DIR, 'real-workspace-diagnostics-audit.latest.json');
	const latestMarkdownPath = path.join(REPORT_DIR, 'real-workspace-diagnostics-audit.latest.md');
	const json = `${JSON.stringify(report, null, 2)}\n`;
	const markdown = buildMarkdownReport(report);
	fs.writeFileSync(jsonPath, json, 'utf8');
	fs.writeFileSync(markdownPath, markdown, 'utf8');
	fs.writeFileSync(latestJsonPath, json, 'utf8');
	fs.writeFileSync(latestMarkdownPath, markdown, 'utf8');
	return { jsonPath, markdownPath };
}

realDescribe('NSF real workspace diagnostics audit', () => {
	it('collects and classifies diagnostics across shader files', async function () {
		this.timeout(readIntEnv('NSF_REAL_DIAGNOSTICS_TIMEOUT_MS', 1800000, 60000, 7200000));

		const extensions = configuredShaderExtensions();
		const maxFiles = readIntEnv('NSF_REAL_DIAGNOSTICS_MAX_FILES', 0, 0, 100000);
		const perFileTimeoutMs = readIntEnv('NSF_REAL_DIAGNOSTICS_PER_FILE_TIMEOUT_MS', 12000, 1000, 120000);
		const searchRoots = configuredAuditSearchRoots();
		const files = await findShaderFiles(extensions, maxFiles, searchRoots);
		assert.ok(files.length > 0, 'Expected shader files in real workspace.');

		const activationUri = files.find((uri) => path.extname(uri.fsPath).toLowerCase() === '.nsf') ?? files[0];
		const activationDocument = await vscode.workspace.openTextDocument(activationUri);
		await vscode.window.showTextDocument(activationDocument, { preview: false });
		await waitForCommandAvailable('nsf._getInternalStatus');
		await waitForClientReady('client ready before real workspace diagnostics audit');
		const seededPreprocessorMacroCount = await seedAuditPreprocessorMacrosIfMissing();
		await waitForIndexingIdle('real workspace diagnostics audit initial indexing idle');

		const diagnostics: AuditDiagnostic[] = [];
		const fileErrors: Array<{ file: string; error: string }> = [];
		let waitTimeouts = 0;

		for (let index = 0; index < files.length; index++) {
			const uri = files[index];
			if (index > 0 && index % 50 === 0) {
				console.log(`[real-diagnostics-audit] scanned ${index}/${files.length}`);
			}
			try {
				const activeUnit = await setActiveUnitForTarget(uri);
				const document = await vscode.workspace.openTextDocument(uri);
				const ready = await waitForDiagnosticsReady(uri, perFileTimeoutMs);
				if (!ready) {
					waitTimeouts++;
				}
				const currentDiagnostics = vscode.languages.getDiagnostics(uri);
				for (const diagnostic of currentDiagnostics) {
					const code = diagnosticCodeText(diagnostic);
					const line = diagnostic.range.start.line;
					const character = diagnostic.range.start.character;
					const lineText = line >= 0 && line < document.lineCount ? document.lineAt(line).text.trim() : '';
					const nextLineText = line + 1 >= 0 && line + 1 < document.lineCount
						? document.lineAt(line + 1).text.trim()
						: '';
					const extension = path.extname(uri.fsPath).toLowerCase();
					const classification = classifyDiagnostic(diagnostic.message, code, lineText, nextLineText, extension);
					diagnostics.push({
						file: uri.fsPath,
						workspaceFolder: workspaceFolderLabel(uri),
						relativePath: relativePathForUri(uri),
						extension,
						activeUnit,
						severity: severityName(diagnostic.severity),
						source: diagnostic.source ?? '',
						code,
						message: diagnostic.message,
						canonicalMessage: canonicalizeMessage(diagnostic.message),
						category: classification.category,
						triage: classification.triage,
						line: line + 1,
						character: character + 1,
						lineText,
						nextLineText,
						reasonCode: diagnosticReasonCode(diagnostic)
					});
				}
			} catch (error) {
				fileErrors.push({
					file: uri.fsPath,
					error: error instanceof Error ? error.message : String(error)
				});
			}
		}

		await waitForIndexingIdle('real workspace diagnostics audit final indexing idle');

		const counts = {
			bySeverity: {} as Record<string, number>,
			bySource: {} as Record<string, number>,
			byCode: {} as Record<string, number>,
			byCategory: {} as Record<string, number>,
			byTriage: {} as Record<string, number>,
			byExtension: {} as Record<string, number>,
			byWorkspaceFolder: {} as Record<string, number>,
			byFile: {} as Record<string, number>
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
		}

		const report = {
			generatedAt: new Date().toISOString(),
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
				preprocessorMacrosSeededForAudit: seededPreprocessorMacroCount
			},
			summary: {
				filesDiscovered: files.length,
				filesScanned: files.length - fileErrors.length,
				filesWithDiagnostics: new Set(diagnostics.map((item) => item.file.toLowerCase())).size,
				diagnosticsTotal: diagnostics.length,
				waitTimeouts,
				fileErrors: fileErrors.length
			},
			counts,
			groups: groupDiagnostics(diagnostics),
			fileErrors,
			diagnostics
		};

		const paths = writeReport(report);
		console.log(`[real-diagnostics-audit] wrote ${paths.jsonPath}`);
		console.log(`[real-diagnostics-audit] wrote ${paths.markdownPath}`);

		assert.strictEqual(fileErrors.length, 0, `Failed to scan files: ${JSON.stringify(fileErrors.slice(0, 5))}`);
	});
});
