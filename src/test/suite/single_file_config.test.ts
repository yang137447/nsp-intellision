import * as assert from 'assert';

import { computeSingleFileIncludePaths } from '../../../client/out/single_file_config';

describe('Single file config', () => {
	it('infers shader-source roots and expands relative include paths', () => {
		const filePath =
			'C:\\Software\\WorkTemp\\G66ShaderDevelop\\shader-source\\shaderlib\\lightmap_meadow_uniforms.hlsl';
		const includePaths = computeSingleFileIncludePaths(filePath, ['shaderlib', 'shader-source', 'C:\\Abs\\Inc']);
		const joined = includePaths.join('|');
		assert.ok(joined.includes('C:\\Software\\WorkTemp\\G66ShaderDevelop\\shader-source'));
		assert.ok(joined.includes('C:\\Software\\WorkTemp\\G66ShaderDevelop'));
		assert.ok(joined.includes('C:\\Abs\\Inc'));
	});
});
