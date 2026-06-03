import * as assert from 'assert';

import { computePreprocessorMacroPresetCompletion } from '../../../client/out/client_config_sync';

function createPreset(count: number): Record<string, string | number | boolean> {
	const preset: Record<string, string | number | boolean> = {};
	for (let index = 0; index < count; index++) {
		preset[`PRESET_MACRO_${index}`] = '0';
	}
	return preset;
}

describe('Preprocessor macro preset completion', () => {
	it('adds missing default preset entries for an old complete preset', () => {
		const preset = {
			...createPreset(60),
			API_PC_HIGH_QUALITY: '0',
			API_SUPPORT_TEXFETCH: '0'
		};
		const configured: Record<string, unknown> = {
			...createPreset(60),
			USER_OVERRIDE: 'kept'
		};
		configured.PRESET_MACRO_1 = '9';

		const result = computePreprocessorMacroPresetCompletion(configured, preset);

		assert.strictEqual(result.eligible, true);
		assert.strictEqual(result.shouldUpdate, true);
		assert.deepStrictEqual(result.missingNames, ['API_PC_HIGH_QUALITY', 'API_SUPPORT_TEXFETCH']);
		assert.strictEqual(result.merged.PRESET_MACRO_1, '9');
		assert.strictEqual(result.merged.USER_OVERRIDE, 'kept');
		assert.strictEqual(result.merged.API_PC_HIGH_QUALITY, '0');
		assert.strictEqual(result.merged.API_SUPPORT_TEXFETCH, '0');
	});

	it('does not fill small custom macro tables', () => {
		const result = computePreprocessorMacroPresetCompletion(
			{
				API_PC_HIGH_QUALITY: '1',
				CUSTOM_SHADER_BRANCH: '1'
			},
			{
				...createPreset(60),
				API_PC_HIGH_QUALITY: '0',
				API_SUPPORT_TEXFETCH: '0'
			}
		);

		assert.strictEqual(result.eligible, false);
		assert.strictEqual(result.shouldUpdate, false);
		assert.strictEqual(result.reason, 'too-few-configured-macros');
		assert.strictEqual(result.merged.API_SUPPORT_TEXFETCH, undefined);
	});

	it('keeps explicitly present values instead of overwriting them', () => {
		const preset = {
			...createPreset(60),
			API_PC_HIGH_QUALITY: '0',
			API_SUPPORT_TEXFETCH: '0'
		};
		const configured: Record<string, unknown> = {
			...createPreset(60),
			API_PC_HIGH_QUALITY: '1'
		};

		const result = computePreprocessorMacroPresetCompletion(configured, preset);

		assert.strictEqual(result.eligible, true);
		assert.strictEqual(result.shouldUpdate, true);
		assert.deepStrictEqual(result.missingNames, ['API_SUPPORT_TEXFETCH']);
		assert.strictEqual(result.merged.API_PC_HIGH_QUALITY, '1');
		assert.strictEqual(result.merged.API_SUPPORT_TEXFETCH, '0');
	});
});
