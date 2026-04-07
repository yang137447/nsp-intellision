import * as assert from 'assert';

import { repoDescribe } from './test_helpers';

type IncludeUnderlineRange = {
	line: number;
	startCharacter: number;
	endCharacter: number;
};

const {
	computeValidIncludeRangesForText
}: {
	computeValidIncludeRangesForText: (options: {
		documentFsPath: string;
		text: string;
		workspaceFolders: string[];
		includePaths: string[];
		shaderExtensions: string[];
		pathExists: (candidate: string) => Promise<boolean>;
	}) => Promise<IncludeUnderlineRange[]>;
} = require('../../../client/out/include_underline_support');

repoDescribe('NSF client integration: Include Underline Support', () => {
	it('resolves include ranges through configured roots and shader extensions', async () => {
		const text = [
			'#include "shared/common"',
			'#include "missing_file"',
			'float4 MainPS(float2 uv : TEXCOORD0) : SV_Target0 {',
			'    return float4(uv, 0.0, 1.0);',
			'}'
		].join('\n');

		const existingPaths = new Set(
			[
				'D:/project/shaders/shared/common.ush',
				'D:/project/local/include_target.hlsl'
			].map((value) => value.replace(/\\/g, '/').toLowerCase())
		);
		const ranges = await computeValidIncludeRangesForText({
			documentFsPath: 'D:/project/source/test_shader.nsf',
			text,
			workspaceFolders: ['D:/project'],
			includePaths: ['shaders'],
			shaderExtensions: ['.nsf', '.hlsl', '.ush'],
			pathExists: async (candidate: string) =>
				existingPaths.has(candidate.replace(/\\/g, '/').toLowerCase())
		});

		assert.deepStrictEqual(
			ranges,
			[
				{
					line: 0,
					startCharacter: 10,
					endCharacter: 23
				}
			] satisfies IncludeUnderlineRange[]
		);
	});

	it('prefers exact include paths when the include already has an extension', async () => {
		const text = '#include "include_target.hlsl"';
		const ranges = await computeValidIncludeRangesForText({
			documentFsPath: 'D:/project/source/test_shader.nsf',
			text,
			workspaceFolders: ['D:/project'],
			includePaths: ['shaders'],
			shaderExtensions: ['.nsf', '.hlsl', '.ush'],
			pathExists: async (candidate: string) =>
				candidate.replace(/\\/g, '/').toLowerCase() ===
				'd:/project/source/include_target.hlsl'
		});

		assert.deepStrictEqual(ranges, [
			{
				line: 0,
				startCharacter: 10,
				endCharacter: 29
			}
		] satisfies IncludeUnderlineRange[]);
	});
});
