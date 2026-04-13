import * as path from 'path';
import * as vscode from 'vscode';

import type { ReplayAnchor } from './real_workspace_replay_types';

export async function resolveReplayAnchor(
	anchor: ReplayAnchor
): Promise<{ uri: vscode.Uri; position: vscode.Position }> {
	const normalizedSuffix = anchor.workspaceFolderSuffix.toLowerCase();
	const workspaceFolders = vscode.workspace.workspaceFolders ?? [];
	let folder = workspaceFolders.find((item) =>
		item.uri.fsPath.replace(/\\/g, '/').toLowerCase().endsWith(normalizedSuffix)
	);
	if (!folder) {
		folder = workspaceFolders.find((item) =>
			item.uri.fsPath.replace(/\\/g, '/').toLowerCase().includes(normalizedSuffix)
		);
	}
	if (!folder) {
		throw new Error(`Unable to find workspace folder ending with '${anchor.workspaceFolderSuffix}'.`);
	}

	const uri = vscode.Uri.file(path.join(folder.uri.fsPath, anchor.relativePath));
	const document = await vscode.workspace.openTextDocument(uri);
	const occurrence = Math.max(1, anchor.occurrence ?? 1);
	let searchFrom = 0;
	let foundAt = -1;
	for (let index = 0; index < occurrence; index++) {
		foundAt = document.getText().indexOf(anchor.anchorText, searchFrom);
		if (foundAt < 0) {
			throw new Error(`Unable to resolve anchor '${anchor.anchorText}' in ${anchor.relativePath}.`);
		}
		searchFrom = foundAt + anchor.anchorText.length;
	}

	return {
		uri,
		position: document.positionAt(foundAt + (anchor.characterOffset ?? 0))
	};
}
