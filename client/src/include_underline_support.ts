import * as path from 'path';

export type IncludeUnderlineRange = {
	line: number;
	startCharacter: number;
	endCharacter: number;
};

const isAbsolutePath = (value: string): boolean => {
	if (path.isAbsolute(value)) {
		return true;
	}
	return /^[a-zA-Z]:[\\/]/.test(value);
};

export function resolveIncludeCandidatesForPath(options: {
	documentFsPath: string;
	includePath: string;
	workspaceFolders: string[];
	includePaths: string[];
	shaderExtensions: string[];
}): string[] {
	const { documentFsPath, includePath, workspaceFolders, includePaths, shaderExtensions } = options;
	const candidates: string[] = [];
	if (isAbsolutePath(includePath)) {
		candidates.push(includePath);
	} else {
		candidates.push(path.join(path.dirname(documentFsPath), includePath));
		for (const inc of includePaths) {
			const trimmed = inc.trim();
			if (!trimmed) {
				continue;
			}
			if (isAbsolutePath(trimmed)) {
				candidates.push(path.join(trimmed, includePath));
			}
		}
		for (const folder of workspaceFolders) {
			for (const inc of includePaths) {
				const trimmed = inc.trim();
				if (!trimmed) {
					continue;
				}
				const base = isAbsolutePath(trimmed) ? trimmed : path.join(folder, trimmed);
				candidates.push(path.join(base, includePath));
			}
		}
	}
	const hasExtension = path.extname(includePath).length > 0;
	const normalized: string[] = [];
	const seen = new Set<string>();
	for (const candidate of candidates) {
		if (hasExtension) {
			const key = path.normalize(candidate).toLowerCase();
			if (seen.has(key)) {
				continue;
			}
			seen.add(key);
			normalized.push(candidate);
			continue;
		}
		for (const ext of shaderExtensions) {
			const withExt = `${candidate}${ext}`;
			const key = path.normalize(withExt).toLowerCase();
			if (seen.has(key)) {
				continue;
			}
			seen.add(key);
			normalized.push(withExt);
		}
	}
	return normalized;
}

export async function computeValidIncludeRangesForText(options: {
	documentFsPath: string;
	text: string;
	workspaceFolders: string[];
	includePaths: string[];
	shaderExtensions: string[];
	pathExists: (candidate: string) => Promise<boolean>;
}): Promise<IncludeUnderlineRange[]> {
	const ranges: IncludeUnderlineRange[] = [];
	const lines = options.text.split(/\r?\n/);
	for (let lineIndex = 0; lineIndex < lines.length; lineIndex++) {
		const line = lines[lineIndex];
		const matched = line.match(/^\s*#\s*include\s*[<"]([^">]+)[>"]/);
		if (!matched) {
			continue;
		}
		const includePath = matched[1];
		const fullMatch = matched[0];
		const relativeStart = fullMatch.indexOf(includePath);
		if (relativeStart < 0) {
			continue;
		}
		const startCharacter = (matched.index ?? 0) + relativeStart;
		const endCharacter = startCharacter + includePath.length;
		const candidates = resolveIncludeCandidatesForPath({
			documentFsPath: options.documentFsPath,
			includePath,
			workspaceFolders: options.workspaceFolders,
			includePaths: options.includePaths,
			shaderExtensions: options.shaderExtensions
		});
		let found = false;
		for (const candidate of candidates) {
			if (await options.pathExists(candidate)) {
				found = true;
				break;
			}
		}
		if (!found) {
			continue;
		}
		ranges.push({ line: lineIndex, startCharacter, endCharacter });
	}
	return ranges;
}
