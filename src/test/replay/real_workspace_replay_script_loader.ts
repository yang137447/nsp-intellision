import * as fs from 'fs';
import * as path from 'path';

import type { ReplayScript } from './real_workspace_replay_types';

export function loadReplayScripts(): ReplayScript[] {
	const repoRoot = path.resolve(__dirname, '..', '..', '..');
	const scriptDir = path.join(repoRoot, 'src', 'test', 'replay', 'scripts');
	if (!fs.existsSync(scriptDir)) {
		return [];
	}
	return fs
		.readdirSync(scriptDir)
		.filter((name) => name.endsWith('.json'))
		.sort()
		.map((name) => JSON.parse(fs.readFileSync(path.join(scriptDir, name), 'utf8')) as ReplayScript);
}
