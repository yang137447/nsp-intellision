import * as path from 'path';

function dedupe(values: string[]): string[] {
	const seen = new Set<string>();
	const result: string[] = [];
	for (const value of values) {
		const key = value.trim();
		if (key.length === 0 || seen.has(key)) {
			continue;
		}
		seen.add(key);
		result.push(key);
	}
	return result;
}

function isAbsolutePath(value: string): boolean {
	return path.isAbsolute(value) || value.startsWith('\\\\');
}

export function inferWorkspaceRootsFromFile(filePath: string): string[] {
	const normalized = path.normalize(filePath);
	const parts = normalized.split(path.sep).filter(Boolean);
	const roots: string[] = [];

	const fileDir = path.dirname(normalized);
	roots.push(fileDir);
	roots.push(path.dirname(fileDir));

	const shaderSourceIndex = parts.lastIndexOf('shader-source');
	if (shaderSourceIndex >= 0) {
		const shaderSourceRoot = parts.slice(0, shaderSourceIndex + 1).join(path.sep);
		roots.push(shaderSourceRoot);
		roots.push(path.dirname(shaderSourceRoot));
	}

	return dedupe(
		roots
			.map((candidate) => candidate.trim())
			.filter((candidate) => candidate.length > 0)
			.map((candidate) => path.resolve(candidate))
	);
}

export function computeSingleFileIncludePaths(filePath: string, configuredIncludePaths: string[]): string[] {
	const roots = inferWorkspaceRootsFromFile(filePath);
	const resolved: string[] = [];
	for (const candidate of configuredIncludePaths) {
		const trimmed = candidate.trim();
		if (!trimmed) {
			continue;
		}
		if (isAbsolutePath(trimmed)) {
			resolved.push(path.normalize(trimmed));
			continue;
		}
		for (const root of roots) {
			resolved.push(path.join(root, trimmed));
		}
	}
	resolved.push(...roots);
	return dedupe(resolved.map((value) => path.resolve(value)));
}

