const fs = require('fs');
const path = require('path');
const https = require('https');
const childProcess = require('child_process');

const ORIGIN = 'https://learn.microsoft.com';
const KEYWORDS_URL = `${ORIGIN}/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-appendix-keywords`;
const MARKDOWN_URL = `${KEYWORDS_URL}?accept=text/markdown`;

const repoRoot = path.resolve(__dirname, '..', '..');
const outDir = path.join(repoRoot, 'server_cpp', 'resources', 'types', 'object_types');
const outFile = path.join(outDir, 'base.json');
const validateScript = path.join(repoRoot, 'scripts', 'json', 'validate_resources.js');

const OBJECT_TYPE_METADATA = new Map([
	['Buffer', { family: 'Buffer', coordDim: 1, isRw: false, isArray: false }],
	['RWBuffer', { family: 'RWBuffer', coordDim: 1, isRw: true, isArray: false }],
	['Texture1D', { family: 'Texture', coordDim: 1, isRw: false, isArray: false }],
	['Texture1DArray', { family: 'Texture', coordDim: 1, isRw: false, isArray: true }],
	['Texture2D', { family: 'Texture', coordDim: 2, isRw: false, isArray: false }],
	['Texture2DArray', { family: 'Texture', coordDim: 2, isRw: false, isArray: true }],
	['Texture2DMS', { family: 'Texture', coordDim: 2, isRw: false, isArray: false }],
	['Texture2DMSArray', { family: 'Texture', coordDim: 2, isRw: false, isArray: true }],
	['Texture3D', { family: 'Texture', coordDim: 3, isRw: false, isArray: false }],
	['TextureCube', { family: 'Texture', coordDim: 3, isRw: false, isArray: false }],
	['TextureCubeArray', { family: 'Texture', coordDim: 3, isRw: false, isArray: true }],
	['texture', { family: 'Texture', coordDim: 2, isRw: false, isArray: false }],
	['RWTexture1D', { family: 'RWTexture', coordDim: 1, isRw: true, isArray: false }],
	['RWTexture1DArray', { family: 'RWTexture', coordDim: 1, isRw: true, isArray: true }],
	['RWTexture2D', { family: 'RWTexture', coordDim: 2, isRw: true, isArray: false }],
	['RWTexture2DArray', { family: 'RWTexture', coordDim: 2, isRw: true, isArray: true }],
	['RWTexture3D', { family: 'RWTexture', coordDim: 3, isRw: true, isArray: false }],
	['StructuredBuffer', { family: 'StructuredBuffer', coordDim: 1, isRw: false, isArray: false }],
	['RWStructuredBuffer', { family: 'RWStructuredBuffer', coordDim: 1, isRw: true, isArray: false }],
	['ByteAddressBuffer', { family: 'ByteAddressBuffer', coordDim: 1, isRw: false, isArray: false }],
	['RWByteAddressBuffer', { family: 'RWByteAddressBuffer', coordDim: 1, isRw: true, isArray: false }],
	['AppendStructuredBuffer', { family: 'RWStructuredBuffer', coordDim: 1, isRw: true, isArray: false }],
	['ConsumeStructuredBuffer', { family: 'StructuredBuffer', coordDim: 1, isRw: false, isArray: false }],
	['SamplerState', { family: 'Sampler', coordDim: 0, isRw: false, isArray: false }],
	['sampler', { family: 'Sampler', coordDim: 0, isRw: false, isArray: false }],
	['SamplerComparisonState', { family: 'SamplerComparisonState', coordDim: 0, isRw: false, isArray: false }],
	['InputPatch', { family: 'Patch', coordDim: 0, isRw: false, isArray: false }],
	['OutputPatch', { family: 'Patch', coordDim: 0, isRw: false, isArray: false }]
]);

const REQUIRED_SCALAR_TYPE_KEYWORDS = [
	'bool',
	'float',
	'float4',
	'int',
	'uint',
	'half',
	'double',
	'matrix',
	'vector',
	'void'
];

function sleep(ms) {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

function httpGet(url, attempt = 0) {
	return new Promise((resolve, reject) => {
		const req = https.get(
			url,
			{
				headers: {
					'user-agent': 'nsf-lsp type fetcher',
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

function addNumericExpansions(names, bases) {
	for (const base of bases) {
		addToken(names, base);
		for (let dim = 1; dim <= 4; dim++) {
			addToken(names, `${base}${dim}`);
		}
		for (let rows = 1; rows <= 4; rows++) {
			for (let cols = 1; cols <= 4; cols++) {
				addToken(names, `${base}${rows}x${cols}`);
			}
		}
	}
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

async function fetchTypeKeywordNames() {
	try {
		const markdown = await httpGet(MARKDOWN_URL);
		const names = extractMarkdownBulletKeywords(markdown);
		addNumericExpansions(names, extractExpansionBases(markdown));
		return names;
	} catch (e) {
		process.stderr.write(`warning: markdown fetch failed, falling back to HTML meta extraction: ${e.message || e}\n`);
		return extractHtmlMetaKeywords(await httpGet(KEYWORDS_URL));
	}
}

function buildObjectTypeEntries(typeKeywordNames) {
	const entries = [];
	for (const [name, info] of OBJECT_TYPE_METADATA) {
		if (!typeKeywordNames.has(name)) continue;
		entries.push({
			name,
			family: info.family,
			coordDim: info.coordDim,
			isRw: info.isRw,
			isArray: info.isArray
		});
	}
	entries.sort((a, b) => a.name.toLowerCase().localeCompare(b.name.toLowerCase()) || a.name.localeCompare(b.name));
	return entries;
}

function writeValidatedBundle(out) {
	const tmpOutFile = `${outFile}.tmp`;
	let committed = false;
	try {
		fs.writeFileSync(tmpOutFile, JSON.stringify(out, null, 2) + '\n', 'utf8');
		const validate = childProcess.spawnSync(process.execPath, [validateScript, '--object-types-base', tmpOutFile], {
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
	const typeKeywordNames = await fetchTypeKeywordNames();
	for (const required of REQUIRED_SCALAR_TYPE_KEYWORDS) {
		if (!typeKeywordNames.has(required)) {
			throw new Error(`Extracted type keywords missing scalar/vector token: ${required}`);
		}
	}
	for (const required of ['Texture2D', 'SamplerState', 'texture', 'sampler']) {
		if (!typeKeywordNames.has(required)) {
			throw new Error(`Extracted type keywords missing object token: ${required}`);
		}
	}

	const entries = buildObjectTypeEntries(typeKeywordNames);
	if (entries.length < OBJECT_TYPE_METADATA.size) {
		const generated = new Set(entries.map((entry) => entry.name));
		const missing = Array.from(OBJECT_TYPE_METADATA.keys()).filter((name) => !generated.has(name));
		throw new Error(`Generated object type resource missing expected official types: ${missing.join(', ')}`);
	}

	const out = { version: 1, entries };
	writeValidatedBundle(out);
	process.stdout.write(`written ${outFile} entries=${out.entries.length}\n`);
}

main().catch((e) => {
	process.stderr.write(String(e && e.stack ? e.stack : e) + '\n');
	process.exit(1);
});
