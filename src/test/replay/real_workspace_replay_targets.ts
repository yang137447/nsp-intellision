import * as path from 'path';
import * as vscode from 'vscode';

import type { ReplayAnchor } from './real_workspace_replay_types';

export async function resolveReplayAnchor(
	anchor: ReplayAnchor
): Promise<{ uri: vscode.Uri; position: vscode.Position }> {
	const normalizedSuffix = anchor.workspaceFolderSuffix.replace(/\\/g, '/').toLowerCase();
	const workspaceFolders = vscode.workspace.workspaceFolders ?? [];
	let folder: vscode.WorkspaceFolder | undefined;
	for (const item of workspaceFolders) {
		const normalizedPath = item.uri.fsPath.replace(/\\/g, '/').toLowerCase();
		const candidates = [normalizedPath];
		const worktreeIndex = normalizedPath.indexOf('/.worktrees/');
		if (worktreeIndex >= 0) {
			candidates.push(normalizedPath.slice(0, worktreeIndex));
		}
		for (const candidate of candidates) {
			if (candidate.endsWith(normalizedSuffix)) {
				folder = item;
				break;
			}
		}
		if (folder) {
			break;
		}
	}
	if (!folder) {
		throw new Error(`Unable to find workspace folder ending with '${anchor.workspaceFolderSuffix}'.`);
	}

	const relativePathCandidates = Array.from(
		new Set([anchor.relativePath, ...(anchor.relativePathAlternatives ?? [])])
	);
	const anchorTextCandidates = Array.from(
		new Set([anchor.anchorText, ...(anchor.anchorTextAlternatives ?? [])])
	);
	const occurrence = Math.max(1, anchor.occurrence ?? 1);
	const attemptErrors: string[] = [];
	for (const relativePath of relativePathCandidates) {
		const uri = vscode.Uri.file(path.join(folder.uri.fsPath, relativePath));
		let document: vscode.TextDocument;
		try {
			document = await vscode.workspace.openTextDocument(uri);
		} catch (error) {
			attemptErrors.push(`${relativePath}: ${String(error)}`);
			continue;
		}
		const documentText = document.getText();
		for (const anchorText of anchorTextCandidates) {
			let searchFrom = 0;
			let foundAt = -1;
			for (let index = 0; index < occurrence; index++) {
				foundAt = documentText.indexOf(anchorText, searchFrom);
				if (foundAt < 0) {
					break;
				}
				searchFrom = foundAt + anchorText.length;
			}
			if (foundAt >= 0) {
				return {
					uri,
					position: document.positionAt(foundAt + (anchor.characterOffset ?? 0))
				};
			}
		}
		attemptErrors.push(`${relativePath}: no matching anchor text`);
	}

	throw new Error(
		`Unable to resolve anchor '${anchor.anchorText}' in ${anchor.relativePath}. Attempts: ${attemptErrors.join('; ')}`
	);
}
