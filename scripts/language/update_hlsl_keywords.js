const fs = require('fs');
const path = require('path');
const https = require('https');
const childProcess = require('child_process');

const ORIGIN = 'https://learn.microsoft.com';
const KEYWORDS_URL = `${ORIGIN}/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-appendix-keywords`;
const MARKDOWN_URL = `${KEYWORDS_URL}?accept=text/markdown`;

const repoRoot = path.resolve(__dirname, '..', '..');
const outDir = path.join(repoRoot, 'server_cpp', 'resources', 'language', 'keywords');
const outFile = path.join(outDir, 'base.json');
const validateScript = path.join(repoRoot, 'scripts', 'json', 'validate_resources.js');

const MIN_KEYWORD_COUNT = 40;

const EXPLICIT_TYPE_KEYWORDS = new Set([
	'AppendStructuredBuffer',
	'BlendState',
	'bool',
	'Buffer',
	'ByteAddressBuffer',
	'ConsumeStructuredBuffer',
	'DepthStencilState',
	'DepthStencilView',
	'double',
	'dword',
	'float',
	'half',
	'InputPatch',
	'int',
	'line',
	'lineadj',
	'LineStream',
	'matrix',
	'min10float',
	'min12int',
	'min16float',
	'min16int',
	'min16uint',
	'OutputPatch',
	'point',
	'PointStream',
	'RasterizerState',
	'RenderTargetView',
	'RWBuffer',
	'RWByteAddressBuffer',
	'RWStructuredBuffer',
	'RWTexture1D',
	'RWTexture1DArray',
	'RWTexture2D',
	'RWTexture2DArray',
	'RWTexture3D',
	'sampler',
	'SamplerComparisonState',
	'SamplerState',
	'snorm',
	'string',
	'StructuredBuffer',
	'texture',
	'Texture1D',
	'Texture1DArray',
	'Texture2D',
	'Texture2DArray',
	'Texture2DMS',
	'Texture2DMSArray',
	'Texture3D',
	'TextureCube',
	'TextureCubeArray',
	'triangle',
	'triangleadj',
	'TriangleStream',
	'uint',
	'unorm',
	'unsigned',
	'vector',
	'void'
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
					'user-agent': 'nsf-lsp keyword fetcher',
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

function htmlToText(html) {
	return decodeHtmlEntities(String(html || '').replace(/<[^>]+>/g, ' '))
		.replace(/\s+/g, ' ')
		.trim();
}

function stripHtml(html) {
	return String(html || '')
		.replace(/<script[\s\S]*?<\/script>/gi, '\n')
		.replace(/<style[\s\S]*?<\/style>/gi, '\n')
		.replace(/<[^>]+>/g, '\n')
		.replace(/\r/g, '')
		.split('\n')
		.map((line) => decodeHtmlEntities(line).trim())
		.filter((line) => line.length > 0);
}

function normalizeMarkdownToken(value) {
	return String(value || '')
		.replace(/\\_/g, '_')
		.replace(/\*\*/g, '')
		.replace(/`/g, '')
		.trim();
}

function isIdentifierLike(value) {
	return /^[A-Za-z_][A-Za-z0-9_]*$/.test(value || '');
}

function readExistingDocs() {
	const docs = new Map();
	if (!fs.existsSync(outFile)) return docs;
	const current = JSON.parse(fs.readFileSync(outFile, 'utf8'));
	for (const entry of current.entries || []) {
		if (entry && typeof entry.name === 'string' && typeof entry.documentation === 'string') {
			if (entry.documentation === 'HLSL reserved keyword.' || entry.documentation === 'HLSL numeric type keyword.') {
				continue;
			}
			docs.set(entry.name, entry.documentation);
			docs.set(entry.name.toLowerCase(), entry.documentation);
		}
	}
	return docs;
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

function addKeyword(names, value) {
	const token = normalizeMarkdownToken(value);
	if (!isIdentifierLike(token)) return;
	names.add(token);
}

function addKeywordList(names, text) {
	for (const rawPart of markdownLinksToText(text).split(',')) {
		addKeyword(names, rawPart);
	}
}

function extractMarkdownBulletKeywords(markdown) {
	const body = stripFrontMatter(markdown);
	const beforeRemarks = body.split(/\n##\s+Remarks\b/i)[0] || body;
	const names = new Set();
	for (const line of beforeRemarks.split('\n')) {
		const match = line.match(/^\s*-\s+(.+)$/);
		if (!match) continue;
		addKeywordList(names, match[1]);
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
		addKeywordList(names, match[1]);
	}
	return Array.from(names).sort((a, b) => a.localeCompare(b));
}

function addNumericExpansions(names, bases) {
	for (const base of bases) {
		addKeyword(names, base);
		for (let dim = 1; dim <= 4; dim++) {
			addKeyword(names, `${base}${dim}`);
		}
		for (let rows = 1; rows <= 4; rows++) {
			for (let cols = 1; cols <= 4; cols++) {
				addKeyword(names, `${base}${rows}x${cols}`);
			}
		}
	}
}

function isNumericExpansion(name, bases) {
	for (const base of bases) {
		if (name === base) return true;
		const escaped = base.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
		if (new RegExp(`^${escaped}[1-4](?:x[1-4])?$`).test(name)) return true;
	}
	return false;
}

function isTypeKeyword(name, expansionBases) {
	return EXPLICIT_TYPE_KEYWORDS.has(name) || isNumericExpansion(name, expansionBases);
}

function extractHtmlMetaKeywords(html) {
	const names = new Set();
	const re = /<meta\s+[^>]*name="keywords"[^>]*content="([^"]+)"/gi;
	let match;
	while ((match = re.exec(html))) {
		const content = decodeHtmlEntities(match[1] || '');
		const suffix = ', HLSL keyword';
		if (!content.endsWith(suffix)) continue;
		addKeyword(names, content.slice(0, -suffix.length));
	}
	return names;
}

function extractHtmlBodyKeywords(html) {
	const names = new Set();
	const lines = stripHtml(html);
	const start = lines.findIndex((line) => line.startsWith('AppendStructuredBuffer'));
	const end = lines.findIndex((line) => /^Remarks$/i.test(line));
	const slice = start >= 0 ? lines.slice(start, end > start ? end : undefined) : [];
	for (const line of slice) {
		addKeywordList(names, line);
	}
	return names;
}

function mergeSets(...sets) {
	const merged = new Set();
	for (const set of sets) {
		for (const value of set) merged.add(value);
	}
	return merged;
}

function sortKeywordNames(names) {
	return Array.from(names).sort((a, b) => a.toLowerCase().localeCompare(b.toLowerCase()) || a.localeCompare(b));
}

async function fetchKeywordSources() {
	try {
		const markdown = await httpGet(MARKDOWN_URL);
		return { markdown, html: '' };
	} catch (e) {
		process.stderr.write(`warning: markdown fetch failed, falling back to HTML: ${e.message || e}\n`);
		const html = await httpGet(KEYWORDS_URL);
		return { markdown: '', html };
	}
}

function buildEntries(names, docs) {
	return sortKeywordNames(names).map((name) => {
		let documentation = docs.get(name) || docs.get(name.toLowerCase());
		if (!documentation) {
			documentation = 'HLSL reserved keyword.';
		}
		return { name, documentation };
	});
}

function writeValidatedBundle(out) {
	const tmpOutFile = `${outFile}.tmp`;
	let committed = false;
	try {
		fs.writeFileSync(tmpOutFile, JSON.stringify(out, null, 2) + '\n', 'utf8');
		const validate = childProcess.spawnSync(process.execPath, [validateScript, '--keywords-base', tmpOutFile], {
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
	const docs = readExistingDocs();
	const { markdown, html } = await fetchKeywordSources();

	let keywordNames = new Set();
	let expansionBases = [];
	if (markdown) {
		keywordNames = extractMarkdownBulletKeywords(markdown);
		expansionBases = extractExpansionBases(markdown);
	} else {
		keywordNames = mergeSets(extractHtmlMetaKeywords(html), extractHtmlBodyKeywords(html));
	}
	keywordNames = new Set(
		Array.from(keywordNames).filter((name) => !isTypeKeyword(name, expansionBases))
	);

	if (keywordNames.size < MIN_KEYWORD_COUNT) {
		throw new Error(`Extracted keyword count too small: ${keywordNames.size}`);
	}
	for (const required of ['discard', 'groupshared', 'packoffset', 'cbuffer']) {
		if (!keywordNames.has(required)) {
			throw new Error(`Extracted keywords missing required token: ${required}`);
		}
	}
	for (const excluded of ['float4', 'Texture2D', 'SamplerState', 'texture']) {
		if (keywordNames.has(excluded)) {
			throw new Error(`Type keyword leaked into language keywords: ${excluded}`);
		}
	}

	const out = {
		version: 1,
		entries: buildEntries(keywordNames, docs)
	};
	writeValidatedBundle(out);
	process.stdout.write(`written ${outFile} entries=${out.entries.length}\n`);
}

main().catch((e) => {
	process.stderr.write(String(e && e.stack ? e.stack : e) + '\n');
	process.exit(1);
});
