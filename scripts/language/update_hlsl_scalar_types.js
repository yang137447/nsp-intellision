const fs = require('fs');
const path = require('path');
const https = require('https');
const childProcess = require('child_process');

const ORIGIN = 'https://learn.microsoft.com';
const KEYWORDS_URL = `${ORIGIN}/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-appendix-keywords`;
const MARKDOWN_URL = `${KEYWORDS_URL}?accept=text/markdown`;

const repoRoot = path.resolve(__dirname, '..', '..');
const outDir = path.join(repoRoot, 'server_cpp', 'resources', 'types', 'scalar_types');
const outFile = path.join(outDir, 'base.json');
const validateScript = path.join(repoRoot, 'scripts', 'json', 'validate_resources.js');

const SCALAR_TYPE_METADATA = new Map([
	['bool', { kind: 'bool', generateVector: true, generateMatrix: true, documentation: 'Boolean scalar type.' }],
	['double', { kind: 'floating', generateVector: true, generateMatrix: true, documentation: 'Double-precision floating-point scalar type.' }],
	['dword', { kind: 'integer', generateVector: false, generateMatrix: false, documentation: 'Unsigned 32-bit scalar type alias.' }],
	['float', { kind: 'floating', generateVector: true, generateMatrix: true, documentation: 'Single-precision floating-point scalar type.' }],
	['half', { kind: 'floating', generateVector: true, generateMatrix: true, documentation: 'Half-precision floating-point scalar type.' }],
	['int', { kind: 'integer', generateVector: true, generateMatrix: true, documentation: 'Signed integer scalar type.' }],
	['matrix', { kind: 'special', generateVector: false, generateMatrix: false, documentation: 'Generic matrix type keyword.' }],
	['min10float', { kind: 'floating', generateVector: true, generateMatrix: true, minPrecision: true, documentation: 'Minimum 10-bit floating-point scalar type.' }],
	['min12int', { kind: 'integer', generateVector: true, generateMatrix: true, minPrecision: true, documentation: 'Minimum 12-bit signed integer scalar type.' }],
	['min16float', { kind: 'floating', generateVector: true, generateMatrix: true, minPrecision: true, documentation: 'Minimum 16-bit floating-point scalar type.' }],
	['min16int', { kind: 'integer', generateVector: true, generateMatrix: true, minPrecision: true, documentation: 'Minimum 16-bit signed integer scalar type.' }],
	['min16uint', { kind: 'integer', generateVector: true, generateMatrix: true, minPrecision: true, documentation: 'Minimum 16-bit unsigned integer scalar type.' }],
	['string', { kind: 'special', generateVector: false, generateMatrix: false, documentation: 'String type keyword.' }],
	['uint', { kind: 'integer', generateVector: true, generateMatrix: true, documentation: 'Unsigned integer scalar type.' }],
	['vector', { kind: 'special', generateVector: false, generateMatrix: false, documentation: 'Generic vector type keyword.' }],
	['void', { kind: 'special', generateVector: false, generateMatrix: false, documentation: 'Void return type keyword.' }]
]);

const DIMENSIONS = [1, 2, 3, 4];
const CURATED_EXPANSION_BASES = new Set([
	// Microsoft's keywords page documents these as scalar keywords, while the
	// numeric expansion paragraph currently omits them from its short base list.
	'double',
	'half'
]);

function sleep(ms) {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

function httpGet(url, attempt = 0) {
	return new Promise((resolve, reject) => {
		const req = https.get(
			url,
			{
				headers: {
					'user-agent': 'nsf-lsp scalar type fetcher',
					accept: 'text/markdown,text/html,application/xhtml+xml'
				}
			},
			(res) => {
				if (res.statusCode && res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
					const redirected = res.headers.location.startsWith('http')
						? res.headers.location
						: ORIGIN + res.headers.location;
					res.resume();
					httpGet(redirected, attempt).then(resolve, reject);
					return;
				}
				if (res.statusCode !== 200) {
					const retryable = res.statusCode === 429 || res.statusCode === 503 || res.statusCode === 504;
					const chunks = [];
					res.on('data', (d) => chunks.push(d));
					res.on('end', async () => {
						if (retryable && attempt < 4) {
							await sleep(500 * Math.pow(2, attempt));
							httpGet(url, attempt + 1).then(resolve, reject);
							return;
						}
						reject(
							new Error(
								`HTTP ${res.statusCode} for ${url}: ${Buffer.concat(chunks)
									.toString('utf8')
									.slice(0, 200)}`
							)
						);
					});
					return;
				}
				const chunks = [];
				res.on('data', (d) => chunks.push(d));
				res.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
			}
		);
		req.on('error', reject);
	});
}

function decodeHtmlEntities(value) {
	return String(value || '')
		.replace(/&nbsp;/g, ' ')
		.replace(/&amp;/g, '&')
		.replace(/&lt;/g, '<')
		.replace(/&gt;/g, '>')
		.replace(/&quot;/g, '"')
		.replace(/&#39;/g, "'");
}

function normalizeMarkdownToken(value) {
	return String(value || '')
		.replace(/\\_/g, '_')
		.replace(/\*\*/g, '')
		.replace(/`/g, '')
		.trim();
}

function stripFrontMatter(markdown) {
	const text = String(markdown || '').replace(/\r/g, '');
	if (!text.startsWith('---\n')) return text;
	const end = text.indexOf('\n---', 4);
	if (end < 0) return text;
	return text.slice(end + 4);
}

function markdownLinksToText(text) {
	let result = String(text || '');
	let previous = '';
	while (result !== previous) {
		previous = result;
		result = result.replace(/\[([^\[\]]+)\]\([^\)]*\)/g, '$1');
	}
	return normalizeMarkdownToken(result);
}

function addToken(names, value) {
	const token = normalizeMarkdownToken(value);
	if (/^[A-Za-z_][A-Za-z0-9_]*$/.test(token)) names.add(token);
}

function addTokenList(names, text) {
	for (const rawPart of markdownLinksToText(text).split(',')) {
		addToken(names, rawPart);
	}
}

function extractMarkdownBulletKeywords(markdown) {
	const body = stripFrontMatter(markdown);
	const beforeRemarks = body.split(/\n##\s+Remarks\b/i)[0] || body;
	const names = new Set();
	for (const line of beforeRemarks.split('\n')) {
		const match = line.match(/^\s*-\s+(.+)$/);
		if (!match) continue;
		addTokenList(names, match[1]);
	}
	return names;
}

function extractExpansionBases(markdown) {
	const body = stripFrontMatter(markdown);
	const remarksMatch = body.match(/These numeric types have scalar, vector, and matrix keyword expansions:[\s\S]*?The expansions/i);
	if (!remarksMatch) return [];
	const names = new Set();
	for (const line of remarksMatch[0].split('\n')) {
		const match = line.match(/^\s*-\s+(.+)$/);
		if (!match) continue;
		addTokenList(names, match[1]);
	}
	return Array.from(names).sort((a, b) => a.localeCompare(b));
}

function extractHtmlMetaKeywords(html) {
	const names = new Set();
	const re = /<meta\s+[^>]*name="keywords"[^>]*content="([^"]+)"/gi;
	let match;
	while ((match = re.exec(html))) {
		const content = decodeHtmlEntities(match[1] || '');
		const suffix = ', HLSL keyword';
		if (!content.endsWith(suffix)) continue;
		addToken(names, content.slice(0, -suffix.length));
	}
	return names;
}

async function fetchKeywordSourceFacts() {
	try {
		const markdown = await httpGet(MARKDOWN_URL);
		return {
			typeKeywordNames: extractMarkdownBulletKeywords(markdown),
			expansionBases: new Set(extractExpansionBases(markdown))
		};
	} catch (e) {
		process.stderr.write(`warning: markdown fetch failed, falling back to HTML meta extraction: ${e.message || e}\n`);
		return {
			typeKeywordNames: extractHtmlMetaKeywords(await httpGet(KEYWORDS_URL)),
			expansionBases: new Set()
		};
	}
}

function buildEntries(typeKeywordNames, expansionBases) {
	const entries = [];
	for (const [name, info] of SCALAR_TYPE_METADATA) {
		if (!typeKeywordNames.has(name)) continue;
		const entry = {
			name,
			kind: info.kind,
			generateVector: info.generateVector,
			generateMatrix: info.generateMatrix
		};
		if (info.generateVector) {
			const hasExpansionEvidence =
				expansionBases.has(name) ||
				CURATED_EXPANSION_BASES.has(name) ||
				(typeKeywordNames.has(`${name}4`) && typeKeywordNames.has(`${name}4x4`));
			if (!hasExpansionEvidence) {
				throw new Error(`Scalar type ${name} is expected to be an official numeric expansion base`);
			}
			entry.vectorDimensions = DIMENSIONS;
		}
		if (info.generateMatrix) {
			entry.matrixRows = DIMENSIONS;
			entry.matrixColumns = DIMENSIONS;
		}
		if (info.legacy) entry.legacy = true;
		if (info.minPrecision) entry.minPrecision = true;
		entry.documentation = info.documentation;
		entries.push(entry);
	}
	entries.sort((a, b) => a.name.localeCompare(b.name));
	return entries;
}

function writeValidatedBundle(out) {
	const tmpOutFile = `${outFile}.tmp`;
	let committed = false;
	try {
		fs.writeFileSync(tmpOutFile, JSON.stringify(out, null, 2) + '\n', 'utf8');
		const validate = childProcess.spawnSync(process.execPath, [validateScript, '--scalar-types-base', tmpOutFile], {
			cwd: repoRoot,
			stdio: 'inherit'
		});
		if (validate.status !== 0) {
			throw new Error(`validate failed with exit code ${validate.status}`);
		}
		fs.renameSync(tmpOutFile, outFile);
		committed = true;
	} finally {
		if (!committed && fs.existsSync(tmpOutFile)) {
			try {
				fs.unlinkSync(tmpOutFile);
			} catch {}
		}
	}
}

async function main() {
	fs.mkdirSync(outDir, { recursive: true });
	const { typeKeywordNames, expansionBases } = await fetchKeywordSourceFacts();
	for (const required of ['bool', 'float', 'int', 'uint', 'half', 'double', 'matrix', 'vector', 'void']) {
		if (!typeKeywordNames.has(required)) {
			throw new Error(`Extracted type keywords missing scalar/vector token: ${required}`);
		}
	}
	const entries = buildEntries(typeKeywordNames, expansionBases);
	if (entries.length < SCALAR_TYPE_METADATA.size) {
		const generated = new Set(entries.map((entry) => entry.name));
		const missing = Array.from(SCALAR_TYPE_METADATA.keys()).filter((name) => !generated.has(name));
		throw new Error(`Generated scalar type resource missing expected official types: ${missing.join(', ')}`);
	}
	const out = { version: 1, entries };
	writeValidatedBundle(out);
	process.stdout.write(`written ${outFile} entries=${out.entries.length}\n`);
}

main().catch((e) => {
	process.stderr.write(String(e && e.stack ? e.stack : e) + '\n');
	process.exit(1);
});
