const fs = require('fs');
const path = require('path');
const https = require('https');
const childProcess = require('child_process');

const ORIGIN = 'https://learn.microsoft.com';
const INDEX_URLS = [
	`${ORIGIN}/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-intrinsic-functions`,
	`${ORIGIN}/en-us/windows/win32/direct3dhlsl/d3d11-graphics-reference-sm5-intrinsics`
];

const repoRoot = path.resolve(__dirname, '..', '..');
const outDir = path.join(repoRoot, 'server_cpp', 'resources', 'builtins', 'intrinsics');
const outFile = path.join(outDir, 'base.json');
const validateScript = path.join(repoRoot, 'scripts', 'json', 'validate_resources.js');
const INDEX_ONLY_DETAIL_PAGE_ALLOWLIST = new Set(['object']);

function sleep(ms) {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

function httpGet(url, attempt = 0) {
	return new Promise((resolve, reject) => {
		const req = https.get(
			url,
			{
				headers: {
					'user-agent': 'nsf-lsp builtin fetcher',
					accept: 'text/html,application/xhtml+xml'
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
					const retryable =
						res.statusCode === 429 || res.statusCode === 503 || res.statusCode === 504;
					const chunks = [];
					res.on('data', (d) => chunks.push(d));
					res.on('end', async () => {
						if (retryable && attempt < 4) {
							const waitMs = 500 * Math.pow(2, attempt);
							await sleep(waitMs);
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

function stripHtml(html) {
	return html
		.replace(/<script[\s\S]*?<\/script>/gi, '\n')
		.replace(/<style[\s\S]*?<\/style>/gi, '\n')
		.replace(/<[^>]+>/g, '\n')
		.replace(/&nbsp;/g, ' ')
		.replace(/\u00a0/g, ' ')
		.replace(/&amp;/g, '&')
		.replace(/&lt;/g, '<')
		.replace(/&gt;/g, '>')
		.replace(/&quot;/g, '"')
		.replace(/&#39;/g, "'")
		.replace(/\r/g, '')
		.split('\n')
		.map((line) => line.trim())
		.filter((line) => line.length > 0);
}

function decodeHtmlEntities(value) {
	return value
		.replace(/&nbsp;/g, ' ')
		.replace(/&amp;/g, '&')
		.replace(/&lt;/g, '<')
		.replace(/&gt;/g, '>')
		.replace(/&quot;/g, '"')
		.replace(/&#39;/g, "'");
}

function htmlToText(html) {
	return decodeHtmlEntities((html || '').replace(/<[^>]+>/g, ' '))
		.replace(/\s+/g, ' ')
		.trim();
}

function isFunctionLikeIndexName(name) {
	return /^[A-Za-z_][A-Za-z0-9_]*$/.test(name || '');
}

function makeAbsoluteLearnUrl(href, baseUrl) {
	if (!href) return '';
	try {
		const absolute = new URL(href, baseUrl);
		if (absolute.hostname !== 'learn.microsoft.com') return '';
		absolute.hash = '';
		absolute.search = '';
		return absolute.toString();
	} catch {
		return '';
	}
}

function parseShaderModelCell(text) {
	const models = new Set();
	for (const match of String(text || '').matchAll(/\b(\d+)\b/g)) {
		models.add(match[1]);
	}
	return Array.from(models).sort();
}

function extractHrefFromCellHtml(cellHtml, baseUrl) {
	const m = String(cellHtml || '').match(/<a\s+[^>]*href="([^"]+)"/i);
	return m ? makeAbsoluteLearnUrl(decodeHtmlEntities(m[1]), baseUrl) : '';
}

function addIndexEntry(map, entry) {
	const name = String(entry.name || '').trim();
	if (!isFunctionLikeIndexName(name)) return;
	if (!map.has(name)) {
		map.set(name, {
			name,
			urls: new Set(),
			summary: '',
			shaderModels: new Set()
		});
	}
	const current = map.get(name);
	for (const url of entry.urls || []) {
		if (url) current.urls.add(url);
	}
	if (entry.summary && !current.summary) current.summary = entry.summary;
	for (const sm of entry.shaderModels || []) {
		if (sm) current.shaderModels.add(sm);
	}
}

function extractFunctionEntriesFromIndexHtml(html, indexUrl) {
	const entries = new Map();
	const re = /<tr[^>]*>\s*<td[^>]*>([\s\S]*?)<\/td>/gi;
	let match;
	while ((match = re.exec(html))) {
		const cellHtml = match[1] || '';
		const cellText = htmlToText(cellHtml);
		const raw = cellText;
		if (!raw) continue;
		if (raw.toLowerCase() === 'function') continue;
		let name = raw;
		const paren = name.indexOf('(');
		if (paren >= 0) name = name.substring(0, paren);
		name = name.trim();
		if (!name) continue;

		const rowEnd = html.indexOf('</tr>', re.lastIndex);
		const rowHtml = rowEnd >= 0 ? html.slice(match.index, rowEnd) : '';
		const tdCells = [];
		const tdRe = /<td[^>]*>([\s\S]*?)<\/td>/gi;
		let tdMatch;
		while ((tdMatch = tdRe.exec(rowHtml))) {
			tdCells.push(tdMatch[1] || '');
		}
		addIndexEntry(entries, {
			name,
			urls: [extractHrefFromCellHtml(cellHtml, indexUrl)],
			summary: tdCells.length > 1 ? htmlToText(tdCells[1]) : '',
			shaderModels: tdCells.length > 2 ? parseShaderModelCell(htmlToText(tdCells[2])) : []
		});
	}

	const mainStart = html.search(/<main\b[^>]*\bid="main"[^>]*>/i);
	const mainEnd = mainStart >= 0 ? html.indexOf('</main>', mainStart) : -1;
	const articleHtml =
		mainStart >= 0 && mainEnd > mainStart ? html.slice(mainStart, mainEnd) : html;
	const linkRe = /<a\s+[^>]*href="([^"]+)"[^>]*>\s*(?:<strong>)?([^<]+)(?:<\/strong>)?\s*<\/a>/gi;
	while ((match = linkRe.exec(articleHtml))) {
		const name = htmlToText(match[2] || '');
		if (!isFunctionLikeIndexName(name)) continue;
		addIndexEntry(entries, {
			name,
			urls: [makeAbsoluteLearnUrl(decodeHtmlEntities(match[1] || ''), indexUrl)]
		});
	}

	return Array.from(entries.values())
		.map((entry) => ({
			name: entry.name,
			urls: Array.from(entry.urls).sort(),
			summary: entry.summary,
			shaderModels: Array.from(entry.shaderModels).sort()
		}))
		.sort((a, b) => a.name.localeCompare(b.name));
}

function slugDx(name) {
	return name
		.trim()
		.toLowerCase()
		.replace(/\s+/g, '')
		.replace(/_/g, '-')
		.replace(/[^a-z0-9-]/g, '');
}

function slugPlain(name) {
	return name
		.trim()
		.toLowerCase()
		.replace(/\s+/g, '')
		.replace(/[^a-z0-9_]/g, '');
}

async function fetchFirstWorkingUrl(name, hintedUrls = []) {
	const dx = slugDx(name);
	const plain = slugPlain(name);
	const candidates = [
		...hintedUrls,
		`${ORIGIN}/windows/win32/direct3dhlsl/dx-graphics-hlsl-${dx}`,
		`${ORIGIN}/windows/win32/direct3dhlsl/dx-graphics-hlsl-${plain}`,
		`${ORIGIN}/windows/win32/direct3dhlsl/${dx}`,
		`${ORIGIN}/windows/win32/direct3dhlsl/${plain}`
	].filter((url, index, all) => url && all.indexOf(url) === index);
	for (const url of candidates) {
		try {
			await httpGet(url);
			return url;
		} catch {
			continue;
		}
	}
	return '';
}

function makeIndexOnlyItem(entry) {
	return {
		name: entry.name,
		docUrl: '',
		shaderModels: entry.shaderModels || [],
		signatures: [],
		summary: entry.summary || ''
	};
}

function parseSignatures(textLines, expectedName) {
	const signatures = [];
	const seen = new Set();

	const expectedLower = expectedName ? expectedName.toLowerCase() : '';

	const normalizeParamLabel = (paramDecl) => {
		let text = (paramDecl || '').replace(/\s+/g, ' ').trim();
		if (!text) return '';
		if (text.toLowerCase() === 'void') return '';
		const parts = text.split(' ').filter(Boolean);
		let name = parts.length > 0 ? parts[parts.length - 1] : '';
		name = name.replace(/[;,]+$/g, '');
		name = name.replace(/^\*+/, '');
		return name;
	};

	const parseParams = (paramText) => {
		const text = (paramText || '').replace(/\s+/g, ' ').trim();
		if (!text) return [];
		if (text.toLowerCase() === 'void') return [];
		return text
			.split(',')
			.map((p) => normalizeParamLabel(p))
			.filter((p) => p.length > 0);
	};

	const addSig = (returnType, name, paramText) => {
		if (!name) return;
		if (expectedLower && name.toLowerCase() !== expectedLower) return;
		const params = parseParams(paramText);
		const display = `${name}(${params.join(', ')})`;
		const kind = returnType && String(returnType).trim().length > 0 ? String(returnType).trim() : 'ret';
		const key = `${kind}|${display}`;
		if (seen.has(key)) return;
		seen.add(key);
		signatures.push({ display, params, kind });
	};

	const readParamsUntilCloseParen = (startIndex, initialRemainder) => {
		let buf = '';
		const first = (initialRemainder || '').trim();
		if (first) buf += ` ${first}`;
		let endIndex = startIndex;
		for (let i = startIndex; i < textLines.length; i++) {
			const line = String(textLines[i] || '').trim();
			const closePos = line.indexOf(')');
			if (closePos >= 0) {
				const before = line.slice(0, closePos).trim();
				if (before) buf += ` ${before}`;
				endIndex = i;
				return { paramText: buf.trim(), endIndex };
			}
			if (line) buf += ` ${line}`;
		}
		return { paramText: buf.trim(), endIndex: textLines.length - 1 };
	};

	for (let i = 0; i < textLines.length; i++) {
		const line = String(textLines[i] || '').trim();
		if (!line) continue;

		// Prototype in assignment form: "uint4 result = msad4(uint reference, ...);"
		let m = line.match(
			/^([A-Za-z_][A-Za-z0-9_<>]*)\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*;?$/
		);
		if (m) {
			addSig(m[1], m[2], m[3]);
			continue;
		}

		// Full prototype in one line: "uint countbits(in uint value);"
		m = line.match(/^([A-Za-z_][A-Za-z0-9_<>]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*;?$/);
		if (m) {
			addSig(m[1], m[2], m[3]);
			continue;
		}

		// "ret" / "void" alone on its own line.
		if ((line === 'ret' || line === 'void') && i + 1 < textLines.length) {
			const returnType = line;
			const next = String(textLines[i + 1] || '').trim();
			if (!next) continue;

			// Next line may already contain the complete prototype.
			m = next.match(/^([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*;?$/);
			if (m) {
				addSig(returnType, m[1], m[2]);
				i = i + 1;
				continue;
			}

			// Multiline: "ret" + "lerp(" + params + ")"
			m = next.match(/^([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)$/);
			if (m) {
				const name = m[1];
				let rest = m[2] || '';
				const closePos = rest.indexOf(')');
				if (closePos >= 0) {
					addSig(returnType, name, rest.slice(0, closePos));
					i = i + 1;
					continue;
				}
				const { paramText, endIndex } = readParamsUntilCloseParen(i + 2, rest);
				addSig(returnType, name, paramText);
				i = endIndex;
				continue;
			}
			continue;
		}

		// Prototype that starts on this line but doesn't close: "ret abs(" or "uint countbits("
		m = line.match(/^([A-Za-z_][A-Za-z0-9_<>]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)$/);
		if (m) {
			const returnType = m[1];
			const name = m[2];
			let rest = m[3] || '';
			const closePos = rest.indexOf(')');
			if (closePos >= 0) {
				addSig(returnType, name, rest.slice(0, closePos));
				continue;
			}
			const { paramText, endIndex } = readParamsUntilCloseParen(i + 1, rest);
			addSig(returnType, name, paramText);
			i = endIndex;
			continue;
		}

		// Bare prototype that starts on this line but doesn't close: "sincos("
		m = line.match(/^([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)$/);
		if (m) {
			const name = m[1];
			let rest = m[2] || '';
			const closePos = rest.indexOf(')');
			if (closePos >= 0) {
				addSig('ret', name, rest.slice(0, closePos));
				continue;
			}
			const { paramText, endIndex } = readParamsUntilCloseParen(i + 1, rest);
			addSig('ret', name, paramText);
			i = endIndex;
			continue;
		}

		// Bare prototype without a return type: "abs(x)" or "abort();"
		m = line.match(/^([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*;?$/);
		if (m) {
			addSig('ret', m[1], m[2]);
		}
	}
	return signatures;
}

function parseShaderModels(textLines) {
	const models = new Set();
	for (const line of textLines) {
		const m = line.match(/\bShader Model\s+(\d+)/i);
		if (m) models.add(m[1]);
	}
	return Array.from(models).sort();
}

function parseSummary(textLines, nameLower) {
	const isNoise = (lower) => {
		if (!lower) return true;
		if (lower.includes('microsoft learn')) return true;
		if (lower.startsWith('skip to')) return true;
		if (lower.startsWith('this browser is no longer supported')) return true;
		if (lower.startsWith('upgrade to microsoft edge')) return true;
		if (lower.startsWith('download microsoft edge')) return true;
		if (lower.startsWith('more info about internet explorer')) return true;
		if (lower.startsWith('table of contents')) return true;
		if (lower.startsWith('ask learn')) return true;
		if (lower.startsWith('feedback')) return true;
		if (lower.startsWith('summarize this article for me')) return true;
		if (lower.startsWith('access to this page requires authorization')) return true;
		if (lower === 'parameters' || lower === 'syntax' || lower === 'see also') return true;
		if (lower.startsWith('intrinsic functions')) return true;
		return false;
	};

	const findFrom = (startIndex) => {
		for (let i = startIndex; i < textLines.length; i++) {
			const line = String(textLines[i] || '').trim();
			if (!line) continue;
			const lower = line.toLowerCase();
			if (isNoise(lower)) continue;
			if (nameLower && lower === nameLower) continue;
			if (nameLower && (lower.includes(`ret ${nameLower}(`) || lower.includes(`void ${nameLower}(`))) break;
			if (line.length < 6) continue;
			return line;
		}
		return '';
	};

	const idx = textLines.findIndex((l) => String(l || '').trim().toLowerCase() === 'in this article');
	if (idx >= 0) {
		const fromArticle = findFrom(idx + 1);
		if (fromArticle) return fromArticle;
	}

	return findFrom(0);
}

function parseReturnKind(textLines) {
	for (let i = 0; i < textLines.length; i++) {
		const lower = String(textLines[i] || '').trim().toLowerCase();
		if (lower !== 'return value' && lower !== 'return values') continue;

		for (let j = i + 1; j < textLines.length; j++) {
			const line = String(textLines[j] || '').trim();
			if (!line) continue;
			const l = line.toLowerCase();
			if (l === 'none.' || l === 'none') return 'void';
			if (l.startsWith('this function does not return')) return 'void';
			break;
		}
	}
	return '';
}

async function fetchAndParseFunctionPage(name, url) {
	const html = await httpGet(url);
	const lines = stripHtml(html);
	const derivedName = name;
	const signatures = parseSignatures(lines, derivedName);
	const returnKind = parseReturnKind(lines);
	if (returnKind === 'void') {
		for (const sig of signatures) {
			if (sig.kind === 'ret') sig.kind = 'void';
		}
	}
	const shaderModels = parseShaderModels(lines);
	const summary = derivedName ? parseSummary(lines, derivedName.toLowerCase()) : '';
	return {
		name: derivedName,
		docUrl: url,
		shaderModels,
		signatures,
		summary
	};
}

function mergeIntoManifest(manifest, item) {
	if (!item.name) return;
	const key = item.name;
	if (!manifest[key]) {
		manifest[key] = {
			name: key,
			signatures: [],
			docUrls: [],
			shaderModels: [],
			summary: ''
		};
	}
	const entry = manifest[key];
	if (item.summary && !entry.summary) entry.summary = item.summary;
	if (item.docUrl && !entry.docUrls.includes(item.docUrl)) entry.docUrls.push(item.docUrl);
	for (const sm of item.shaderModels ?? []) {
		if (!entry.shaderModels.includes(sm)) entry.shaderModels.push(sm);
	}
	for (const sig of item.signatures ?? []) {
		if (!entry.signatures.some((s) => s.display === sig.display)) {
			entry.signatures.push({
				display: sig.display,
				params: sig.params,
				kind: sig.kind
			});
		}
	}
	entry.docUrls.sort();
	entry.shaderModels.sort();
	entry.signatures.sort((a, b) => a.display.localeCompare(b.display));
}

async function runPool(items, concurrency, worker) {
	const queue = items.slice();
	let active = 0;
	let done = 0;
	const errors = [];
	return new Promise((resolve, reject) => {
		const next = () => {
			if (done === items.length && active === 0) {
				if (errors.length > 0) reject(errors[0]);
				else resolve();
				return;
			}
			while (active < concurrency && queue.length > 0) {
				const item = queue.shift();
				active++;
				Promise.resolve()
					.then(() => worker(item))
					.catch((e) => errors.push(e))
					.finally(() => {
						active--;
						done++;
						next();
					});
			}
		};
		next();
	});
}

async function main() {
	fs.mkdirSync(outDir, { recursive: true });

	const indexEntries = new Map();
	for (const url of INDEX_URLS) {
		const html = await httpGet(url);
		const entries = extractFunctionEntriesFromIndexHtml(html, url);
		if (entries.length === 0) {
			throw new Error(`No intrinsic names extracted from index ${url}`);
		}
		process.stdout.write(`index ${url} names=${entries.length}\n`);
		for (const entry of entries) addIndexEntry(indexEntries, entry);
	}

	const entriesFromIndexes = Array.from(indexEntries.values())
		.map((entry) => ({
			name: entry.name,
			urls: Array.from(entry.urls).sort(),
			summary: entry.summary,
			shaderModels: Array.from(entry.shaderModels).sort()
		}))
		.sort((a, b) => a.name.localeCompare(b.name));
	const manifest = {};
	const indexOnlyNames = [];
	await runPool(
		entriesFromIndexes,
		2,
		async (entry) => {
			const url = await fetchFirstWorkingUrl(entry.name, entry.urls);
			if (!url) {
				indexOnlyNames.push(entry.name);
				mergeIntoManifest(manifest, makeIndexOnlyItem(entry));
				return;
			}
			const item = await fetchAndParseFunctionPage(entry.name, url);
			if (!item.summary && entry.summary) item.summary = entry.summary;
			if ((item.shaderModels || []).length === 0) item.shaderModels = entry.shaderModels || [];
			mergeIntoManifest(manifest, item);
		}
	);

	const entries = Object.values(manifest).sort((a, b) => a.name.localeCompare(b.name));
	const generatedNames = new Set(entries.map((entry) => entry.name));
	const missingNames = entriesFromIndexes
		.map((entry) => entry.name)
		.filter((name) => !generatedNames.has(name));
	if (missingNames.length > 0) {
		throw new Error(`Generated manifest is missing index names: ${missingNames.join(', ')}`);
	}
	if (entries.length !== entriesFromIndexes.length) {
		throw new Error(
			`Generated manifest count mismatch: index=${entriesFromIndexes.length} entries=${entries.length}`
		);
	}
	if (indexOnlyNames.length > 0) {
		const unexpectedIndexOnly = indexOnlyNames.filter(
			(name) => !INDEX_ONLY_DETAIL_PAGE_ALLOWLIST.has(name)
		);
		if (unexpectedIndexOnly.length > 0) {
			throw new Error(
				`Missing detail pages for intrinsic names: ${unexpectedIndexOnly.sort().join(', ')}`
			);
		}
		process.stderr.write(
			`warning: index-only intrinsic entries without detail pages: ${indexOnlyNames.sort().join(', ')}\n`
		);
	}
	const out = {
		source: 'Microsoft Learn',
		sourceUrls: INDEX_URLS,
		generatedAtUtc: new Date().toISOString(),
		count: entries.length,
		entries
	};
	const tmpOutFile = `${outFile}.tmp`;
	fs.writeFileSync(tmpOutFile, JSON.stringify(out, null, 2) + '\n', 'utf8');
	const validate = childProcess.spawnSync(
		process.execPath,
		[validateScript, '--builtins-manifest', tmpOutFile],
		{
			cwd: repoRoot,
			stdio: 'inherit'
		}
	);
	if (validate.status !== 0) {
		try {
			if (fs.existsSync(tmpOutFile)) fs.unlinkSync(tmpOutFile);
		} catch {}
		throw new Error(`validate failed with exit code ${validate.status}`);
	}
	fs.renameSync(tmpOutFile, outFile);
	process.stdout.write(`written ${outFile} entries=${entries.length}\n`);
}

main().catch((e) => {
	process.stderr.write(String(e && e.stack ? e.stack : e) + '\n');
	process.exit(1);
});
