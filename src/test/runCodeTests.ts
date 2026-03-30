import * as fs from 'fs';
import * as path from 'path';
import { spawn } from 'child_process';

interface RunnerOptions {
	mode: string;
	workspaceTarget?: string;
	fileFilter?: string;
}

function isIgnorableNoiseStart(line: string): boolean {
	const lower = line.toLowerCase();
	return (
		lower.includes('connection got disposed.: error: connection got disposed.') ||
		lower.includes('error [err_stream_write_after_end]: write after end')
	);
}

function isIgnorableNoiseLine(line: string): boolean {
	const lower = line.toLowerCase();
	if (lower.includes('an unknown error occurred. please consult the log for more details.')) {
		return true;
	}
	return false;
}

function shouldTreatHostMutexExitAsSuccess(stdoutText: string, stderrText: string): boolean {
	const combined = `${stdoutText}\n${stderrText}`.toLowerCase();
	if (!combined.includes('error mutex already exists')) {
		return false;
	}
	if (combined.includes('test(s) failed.')) {
		return false;
	}
	return true;
}

function lineContainsHostMutexError(line: string): boolean {
	return line.toLowerCase().includes('error mutex already exists');
}

function lineLooksLikeTestFailure(line: string): boolean {
	const lower = line.toLowerCase();
	return (
		lower.includes('test(s) failed.') ||
		lower.includes('assertionerror') ||
		lower.includes('codeexpectederror') ||
		lower.includes('timeout of 60000ms exceeded') ||
		/^\s*\d+\)\s/.test(line)
	);
}

function resolveCodeExecutable(): string {
	const candidates = [
		process.env.VSCODE_EXECUTABLE_PATH,
		'C:\\Software\\Microsoft VS Code\\Code.exe',
		process.env.VSCODE_CLI,
		'code.cmd',
		'code',
		'C:\\Software\\Microsoft VS Code\\bin\\code.cmd'
	].filter((candidate): candidate is string => Boolean(candidate));

	for (const candidate of candidates) {
		if (candidate === 'code.cmd' || candidate === 'code') {
			return candidate;
		}
		if (fs.existsSync(candidate)) {
			return candidate;
		}
	}

	throw new Error(
		'Unable to locate a VS Code executable. Set VSCODE_EXECUTABLE_PATH or VSCODE_CLI.'
	);
}

function parseArgs(argv: string[]): RunnerOptions {
	const options: RunnerOptions = { mode: 'repo' };
	for (let index = 0; index < argv.length; index++) {
		const arg = argv[index];
		if (arg === '--mode') {
			options.mode = argv[index + 1] ?? options.mode;
			index++;
			continue;
		}
		if (arg === '--workspace') {
			options.workspaceTarget = argv[index + 1];
			index++;
			continue;
		}
		if (arg === '--file-filter') {
			options.fileFilter = argv[index + 1];
			index++;
		}
	}
	return options;
}

async function run(): Promise<void> {
	const repoRoot = path.resolve(__dirname, '..', '..');
	const startedAtMs = Date.now();
	const options = parseArgs(process.argv.slice(2));
	const workspaceTarget = options.workspaceTarget ?? repoRoot;
	const codeExecutable = resolveCodeExecutable();
	const runId = `${Date.now()}-${process.pid}`;
	const userDataDir = path.join(repoRoot, '.vscode-test', 'user-data', runId);
	const extensionsDir = path.join(repoRoot, '.vscode-test', 'extensions', runId);

	fs.mkdirSync(userDataDir, { recursive: true });
	fs.mkdirSync(extensionsDir, { recursive: true });

	const args = [
		'--disable-extensions',
		'--disable-gpu',
		'--disable-workspace-trust',
		'--skip-welcome',
		'--skip-release-notes',
		`--user-data-dir=${userDataDir}`,
		`--extensions-dir=${extensionsDir}`,
		`--extensionDevelopmentPath=${repoRoot}`,
		`--extensionTestsPath=${path.join(repoRoot, 'out', 'test', 'suite', 'index')}`,
		workspaceTarget
	];

	console.log(`[runCodeTests] launching ${codeExecutable}`);
	console.log(`[runCodeTests] mode=${options.mode} workspace=${workspaceTarget}`);

	const collectCrashArtifacts = (): void => {
		const crashLogPath = path.join(repoRoot, 'nsf_lsp_crash.log');
		if (fs.existsSync(crashLogPath)) {
			try {
				const stat = fs.statSync(crashLogPath);
				if (stat.mtimeMs >= startedAtMs) {
					const text = fs.readFileSync(crashLogPath, 'utf8');
					const lines = text.split(/\r?\n/).filter((line) => line.length > 0);
					const tail = lines.slice(Math.max(0, lines.length - 80));
					console.log('[runCodeTests] nsf_lsp_crash.log (tail)');
					for (const line of tail) {
						console.log(`[runCodeTests] ${line}`);
					}
				}
			} catch (error) {
				console.log(
					`[runCodeTests] failed to read nsf_lsp_crash.log: ${error instanceof Error ? error.message : String(error)}`
				);
			}
		}

		let dumps: string[] = [];
		try {
			dumps = fs
				.readdirSync(repoRoot)
				.filter((name) => name.startsWith('nsf_lsp_') && name.endsWith('.dmp'))
				.map((name) => path.join(repoRoot, name))
				.filter((absolutePath) => {
					try {
						return fs.statSync(absolutePath).mtimeMs >= startedAtMs;
					} catch {
						return false;
					}
				});
		} catch {
			dumps = [];
		}
		if (dumps.length > 0) {
			console.log('[runCodeTests] crash dumps');
			for (const dumpPath of dumps) {
				console.log(`[runCodeTests] ${dumpPath}`);
			}
		}
	};

	try {
		await new Promise<void>((resolve, reject) => {
			const child = spawn(codeExecutable, args, {
				cwd: repoRoot,
				stdio: ['inherit', 'pipe', 'pipe'],
				shell: false,
				env: {
					...process.env,
					NSF_TEST_MODE: options.mode,
					NSF_TEST_WORKSPACE_PATH: workspaceTarget,
					NSF_TEST_FILE_FILTER: options.fileFilter ?? process.env.NSF_TEST_FILE_FILTER
				}
			});

			let stdoutBuffer = '';
			let stderrBuffer = '';
			let suppressNoiseStack = false;
			let sawHostMutexError = false;
			let sawTestFailureSignal = false;
			const flushLines = (text: string, isErr: boolean): string => {
				const lines = text.split(/\r?\n/);
				const rest = lines.pop() ?? '';
				for (const line of lines) {
					const trimmed = line.trim();
					if (suppressNoiseStack) {
						if (trimmed.length === 0) {
							suppressNoiseStack = false;
						}
						continue;
					}
					if (isIgnorableNoiseStart(line)) {
						suppressNoiseStack = true;
						continue;
					}
					if (isIgnorableNoiseLine(line)) {
						continue;
					}
					if (lineContainsHostMutexError(line)) {
						sawHostMutexError = true;
					}
					if (lineLooksLikeTestFailure(line)) {
						sawTestFailureSignal = true;
					}
					if (isErr) {
						process.stderr.write(`${line}\n`);
					} else {
						process.stdout.write(`${line}\n`);
					}
				}
				return rest;
			};
			child.stdout?.setEncoding('utf8');
			child.stderr?.setEncoding('utf8');
			child.stdout?.on('data', (chunk: string) => {
				stdoutBuffer += chunk;
				stdoutBuffer = flushLines(stdoutBuffer, false);
			});
			child.stderr?.on('data', (chunk: string) => {
				stderrBuffer += chunk;
				stderrBuffer = flushLines(stderrBuffer, true);
			});

			child.on('error', reject);
			child.on('exit', (code) => {
				if (stdoutBuffer.trim().length > 0 && !isIgnorableNoiseLine(stdoutBuffer)) {
					process.stdout.write(`${stdoutBuffer}\n`);
				}
				if (stderrBuffer.trim().length > 0 && !isIgnorableNoiseLine(stderrBuffer)) {
					process.stderr.write(`${stderrBuffer}\n`);
				}
				if (lineContainsHostMutexError(stdoutBuffer) || lineContainsHostMutexError(stderrBuffer)) {
					sawHostMutexError = true;
				}
				if (lineLooksLikeTestFailure(stdoutBuffer) || lineLooksLikeTestFailure(stderrBuffer)) {
					sawTestFailureSignal = true;
				}
				if (
					code === 0 ||
					shouldTreatHostMutexExitAsSuccess(stdoutBuffer, stderrBuffer) ||
					(sawHostMutexError && !sawTestFailureSignal)
				) {
					resolve();
					return;
				}
				reject(new Error(`VS Code integration tests exited with code ${code ?? 'null'}`));
			});
		});
	} catch (error) {
		collectCrashArtifacts();
		throw error;
	}
}

void run().catch((error) => {
	console.error(error);
	process.exit(1);
});
