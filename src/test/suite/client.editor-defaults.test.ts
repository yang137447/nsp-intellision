import * as assert from 'assert';
import * as fs from 'fs';
import * as path from 'path';

import { getWorkspaceRoot, repoDescribe } from './test_helpers';

repoDescribe('NSF client integration: Editor Defaults', () => {
	it('declares quick suggestion and parameter hint defaults for both nsf and hlsl language modes', () => {
		const packageJsonPath = path.join(getWorkspaceRoot(), 'package.json');
		const packageJson = JSON.parse(fs.readFileSync(packageJsonPath, 'utf8'));
		const defaults = packageJson?.contributes?.configurationDefaults ?? {};

		for (const languageKey of ['[nsf]', '[hlsl]']) {
			const languageDefaults = defaults[languageKey];
			assert.ok(languageDefaults, `Expected configurationDefaults entry for ${languageKey}.`);
			assert.deepStrictEqual(languageDefaults['editor.quickSuggestions'], {
				other: true,
				comments: false,
				strings: false
			});
			assert.strictEqual(languageDefaults['editor.suggestOnTriggerCharacters'], true);
			assert.strictEqual(languageDefaults['editor.parameterHints.enabled'], true);
		}
	});
});
