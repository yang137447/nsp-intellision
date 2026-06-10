const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..', '..');
const resourcesRoot = path.join(repoRoot, 'server_cpp', 'resources');
const builtinsRoot = path.join(resourcesRoot, 'builtins', 'intrinsics');
const methodsRoot = path.join(resourcesRoot, 'methods', 'object_methods');
const keywordsRoot = path.join(resourcesRoot, 'language', 'keywords');
const directivesRoot = path.join(resourcesRoot, 'language', 'directives');
const semanticsRoot = path.join(resourcesRoot, 'language', 'semantics');
const preprocessorMacrosRoot = path.join(resourcesRoot, 'language', 'preprocessor_macros');
const objectTypesRoot = path.join(resourcesRoot, 'types', 'object_types');
const objectFamiliesRoot = path.join(resourcesRoot, 'types', 'object_families');
const typeOverridesRoot = path.join(resourcesRoot, 'types', 'type_overrides');
const scalarTypesRoot = path.join(resourcesRoot, 'types', 'scalar_types');

const MIN_SCALAR_TYPE_COUNT = 16;
const MIN_OBJECT_TYPE_COUNT = 26;
const MIN_KEYWORD_COUNT = 40;
const MIN_METHOD_COUNT = 12;

const REQUIRED_KEYWORDS = [
	'break',
	'case',
	'cbuffer',
	'const',
	'discard',
	'for',
	'groupshared',
	'if',
	'in',
	'inout',
	'out',
	'packoffset',
	'return',
	'struct',
	'typedef',
	'uniform',
	'while'
];

const FORBIDDEN_KEYWORD_TYPE_TOKENS = [
	'bool',
	'float',
	'float4',
	'float4x4',
	'uint',
	'uint4x4',
	'min16float',
	'min16float2',
	'Texture2D',
	'SamplerState',
	'texture',
	'sampler',
	'matrix',
	'vector',
	'void'
];

const REQUIRED_OBJECT_TYPES = [
	'Buffer',
	'RWBuffer',
	'Texture1D',
	'Texture1DArray',
	'Texture2D',
	'Texture2DArray',
	'Texture2DMS',
	'Texture2DMSArray',
	'Texture3D',
	'TextureCube',
	'TextureCubeArray',
	'texture',
	'RWTexture1D',
	'RWTexture1DArray',
	'RWTexture2D',
	'RWTexture2DArray',
	'RWTexture3D',
	'StructuredBuffer',
	'RWStructuredBuffer',
	'ByteAddressBuffer',
	'RWByteAddressBuffer',
	'AppendStructuredBuffer',
	'ConsumeStructuredBuffer',
	'SamplerState',
	'sampler',
	'SamplerComparisonState'
];

const REQUIRED_SCALAR_TYPES = [
	'bool',
	'double',
	'dword',
	'float',
	'half',
	'int',
	'matrix',
	'min10float',
	'min12int',
	'min16float',
	'min16int',
	'min16uint',
	'string',
	'uint',
	'vector',
	'void'
];

const REQUIRED_GENERATED_SCALAR_TYPE_NAMES = [
	'float',
	'float1',
	'float4',
	'float4x4',
	'uint4x4',
	'min16float2',
	'min16uint4x4',
	'bool4',
	'bool4x4'
];

const REQUIRED_FAMILIES = [
	'Texture',
	'RWTexture',
	'Buffer',
	'RWBuffer',
	'StructuredBuffer',
	'RWStructuredBuffer',
	'ByteAddressBuffer',
	'RWByteAddressBuffer',
	'Sampler',
	'SamplerComparisonState'
];

const REQUIRED_METHODS = [
	'Sample',
	'SampleLevel',
	'SampleBias',
	'SampleGrad',
	'SampleCmp',
	'SampleCmpLevelZero',
	'Gather',
	'GatherRed',
	'GatherGreen',
	'GatherBlue',
	'GatherAlpha',
	'Load',
	'GetDimensions'
];

function parseCliArgs(argv) {
	const parsed = {};
	for (let i = 0; i < argv.length; i++) {
		const token = argv[i];
		if (token === '--builtins-manifest' && i + 1 < argv.length) {
			parsed.builtinsManifest = path.resolve(argv[i + 1]);
			i++;
		} else if (token === '--keywords-base' && i + 1 < argv.length) {
			parsed.keywordsBase = path.resolve(argv[i + 1]);
			i++;
		} else if (token === '--object-types-base' && i + 1 < argv.length) {
			parsed.objectTypesBase = path.resolve(argv[i + 1]);
			i++;
		} else if (token === '--scalar-types-base' && i + 1 < argv.length) {
			parsed.scalarTypesBase = path.resolve(argv[i + 1]);
			i++;
		}
	}
	return parsed;
}

const cliArgs = parseCliArgs(process.argv.slice(2));

const schemaChecks = [
	{
		label: 'builtins base',
		dataPath: cliArgs.builtinsManifest || path.join(builtinsRoot, 'base.json'),
		schemaPath: path.join(builtinsRoot, 'schema.json')
	},
	{
		label: 'builtins override',
		dataPath: path.join(builtinsRoot, 'override.json'),
		schemaPath: path.join(builtinsRoot, 'schema.json')
	},
	{
		label: 'object types',
		dataPath: cliArgs.objectTypesBase || path.join(objectTypesRoot, 'base.json'),
		schemaPath: path.join(objectTypesRoot, 'schema.json')
	},
	{
		label: 'object type overrides',
		dataPath: path.join(objectTypesRoot, 'override.json'),
		schemaPath: path.join(objectTypesRoot, 'schema.json')
	},
	{
		label: 'scalar types',
		dataPath: cliArgs.scalarTypesBase || path.join(scalarTypesRoot, 'base.json'),
		schemaPath: path.join(scalarTypesRoot, 'schema.json')
	},
	{
		label: 'scalar type overrides',
		dataPath: path.join(scalarTypesRoot, 'override.json'),
		schemaPath: path.join(scalarTypesRoot, 'schema.json')
	},
	{
		label: 'object families',
		dataPath: path.join(objectFamiliesRoot, 'base.json'),
		schemaPath: path.join(objectFamiliesRoot, 'schema.json')
	},
	{
		label: 'object family overrides',
		dataPath: path.join(objectFamiliesRoot, 'override.json'),
		schemaPath: path.join(objectFamiliesRoot, 'schema.json')
	},
	{
		label: 'type overrides base',
		dataPath: path.join(typeOverridesRoot, 'base.json'),
		schemaPath: path.join(typeOverridesRoot, 'schema.json')
	},
	{
		label: 'type overrides',
		dataPath: path.join(typeOverridesRoot, 'override.json'),
		schemaPath: path.join(typeOverridesRoot, 'schema.json')
	},
	{
		label: 'object methods',
		dataPath: path.join(methodsRoot, 'base.json'),
		schemaPath: path.join(methodsRoot, 'schema.json')
	},
	{
		label: 'object method overrides',
		dataPath: path.join(methodsRoot, 'override.json'),
		schemaPath: path.join(methodsRoot, 'schema.json')
	},
	{
		label: 'keyword base',
		dataPath: cliArgs.keywordsBase || path.join(keywordsRoot, 'base.json'),
		schemaPath: path.join(keywordsRoot, 'schema.json')
	},
	{
		label: 'keyword override',
		dataPath: path.join(keywordsRoot, 'override.json'),
		schemaPath: path.join(keywordsRoot, 'schema.json')
	},
	{
		label: 'directive base',
		dataPath: path.join(directivesRoot, 'base.json'),
		schemaPath: path.join(directivesRoot, 'schema.json')
	},
	{
		label: 'directive override',
		dataPath: path.join(directivesRoot, 'override.json'),
		schemaPath: path.join(directivesRoot, 'schema.json')
	},
	{
		label: 'semantic base',
		dataPath: path.join(semanticsRoot, 'base.json'),
		schemaPath: path.join(semanticsRoot, 'schema.json')
	},
	{
		label: 'semantic override',
		dataPath: path.join(semanticsRoot, 'override.json'),
		schemaPath: path.join(semanticsRoot, 'schema.json')
	},
	{
		label: 'preprocessor macro base',
		dataPath: path.join(preprocessorMacrosRoot, 'base.json'),
		schemaPath: path.join(preprocessorMacrosRoot, 'schema.json')
	},
	{
		label: 'preprocessor macro override',
		dataPath: path.join(preprocessorMacrosRoot, 'override.json'),
		schemaPath: path.join(preprocessorMacrosRoot, 'schema.json')
	}
];

function readJson(filePath) {
	const text = fs.readFileSync(filePath, 'utf8');
	return JSON.parse(text);
}

function isObject(value) {
	return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function typeMatches(value, expectedType) {
	if (expectedType === 'object') return isObject(value);
	if (expectedType === 'array') return Array.isArray(value);
	if (expectedType === 'string') return typeof value === 'string';
	if (expectedType === 'number') return typeof value === 'number' && Number.isFinite(value);
	if (expectedType === 'boolean') return typeof value === 'boolean';
	if (expectedType === 'integer') return Number.isInteger(value);
	return false;
}

function validateBySchema(value, schema, currentPath, errors) {
	if (Array.isArray(schema.type)) {
		const ok = schema.type.some((t) => typeMatches(value, t));
		if (!ok) {
			errors.push(`${currentPath}: expected one of [${schema.type.join(', ')}]`);
			return;
		}
	} else if (schema.type && !typeMatches(value, schema.type)) {
		errors.push(`${currentPath}: expected ${schema.type}`);
		return;
	}

	if (schema.enum && !schema.enum.includes(value)) {
		errors.push(`${currentPath}: value not in enum`);
	}

	if (schema.const !== undefined && value !== schema.const) {
		errors.push(`${currentPath}: value must equal ${JSON.stringify(schema.const)}`);
	}

	if (typeof value === 'string') {
		if (schema.minLength !== undefined && value.length < schema.minLength) {
			errors.push(`${currentPath}: string length < ${schema.minLength}`);
		}
		if (schema.pattern !== undefined) {
			const re = new RegExp(schema.pattern);
			if (!re.test(value)) errors.push(`${currentPath}: string does not match pattern ${schema.pattern}`);
		}
	}

	if (Array.isArray(value)) {
		if (schema.minItems !== undefined && value.length < schema.minItems) {
			errors.push(`${currentPath}: array length < ${schema.minItems}`);
		}
		if (schema.uniqueItems) {
			const keys = new Set();
			for (const item of value) {
				const serialized = JSON.stringify(item);
				if (keys.has(serialized)) {
					errors.push(`${currentPath}: array contains duplicate item`);
					break;
				}
				keys.add(serialized);
			}
		}
		if (schema.items) {
			for (let i = 0; i < value.length; i++) {
				validateBySchema(value[i], schema.items, `${currentPath}[${i}]`, errors);
			}
		}
	}

	if (isObject(value)) {
		const properties = schema.properties || {};
		const required = schema.required || [];
		for (const key of required) {
			if (!(key in value)) errors.push(`${currentPath}: missing required property "${key}"`);
		}
		for (const [key, childSchema] of Object.entries(properties)) {
			if (key in value) validateBySchema(value[key], childSchema, `${currentPath}.${key}`, errors);
		}
		if (schema.additionalProperties === false) {
			for (const key of Object.keys(value)) {
				if (!(key in properties)) errors.push(`${currentPath}: unexpected property "${key}"`);
			}
		}
	}
}

function assertOrThrow(condition, message) {
	if (!condition) throw new Error(message);
}

function assertUniqueNames(entries, label, options = {}) {
	const caseInsensitive = options.caseInsensitive !== false;
	const names = new Set();
	const originalNames = new Set();
	for (const entry of entries) {
		assertOrThrow(typeof entry.name === 'string' && entry.name.length > 0, `${label} entry name must be non-empty`);
		originalNames.add(entry.name);
		const key = caseInsensitive ? entry.name.toLowerCase() : entry.name;
		assertOrThrow(!names.has(key), `duplicate ${label} entry: ${entry.name}`);
		names.add(key);
	}
	return originalNames;
}

function assertUniqueStrings(values, label) {
	const seen = new Set();
	for (const value of values || []) {
		assertOrThrow(!seen.has(value), `duplicate ${label}: ${value}`);
		seen.add(value);
	}
}

function validateBuiltinsCoverage(baseBundle) {
	assertOrThrow(Array.isArray(baseBundle.entries), 'builtins base entries must be array');
	assertOrThrow(baseBundle.entries.length > 0, 'builtins base must contain entries');
	if (typeof baseBundle.count === 'number') {
		assertOrThrow(baseBundle.count === baseBundle.entries.length, 'builtins base count must equal entries length');
	}
	assertUniqueNames(baseBundle.entries, 'builtin');
}

function assertDimensionList(values, label) {
	assertOrThrow(Array.isArray(values), `${label} must be array`);
	assertOrThrow(values.length > 0, `${label} must not be empty`);
	assertUniqueStrings(values, label);
	for (const value of values) {
		assertOrThrow(Number.isInteger(value) && value >= 1 && value <= 4, `${label} must contain dimensions 1..4`);
	}
}

function generatedScalarTypeNames(scalarTypes) {
	const names = new Set();
	for (const entry of scalarTypes.entries) {
		names.add(entry.name);
		if (entry.generateVector) {
			for (const dim of entry.vectorDimensions || []) {
				names.add(`${entry.name}${dim}`);
			}
		}
		if (entry.generateMatrix) {
			for (const rows of entry.matrixRows || []) {
				for (const cols of entry.matrixColumns || []) {
					names.add(`${entry.name}${rows}x${cols}`);
				}
			}
		}
	}
	return names;
}

function validateScalarTypeCoverage(scalarTypes) {
	assertOrThrow(Array.isArray(scalarTypes.entries), 'scalar type entries must be array');
	assertOrThrow(scalarTypes.entries.length >= MIN_SCALAR_TYPE_COUNT, `scalar type entries < ${MIN_SCALAR_TYPE_COUNT}`);
	const scalarNames = assertUniqueNames(scalarTypes.entries, 'scalar type');
	for (const requiredType of REQUIRED_SCALAR_TYPES) {
		assertOrThrow(scalarNames.has(requiredType), `missing required scalar type: ${requiredType}`);
	}
	for (const entry of scalarTypes.entries) {
		if (entry.generateVector) {
			assertDimensionList(entry.vectorDimensions, `vectorDimensions for ${entry.name}`);
		}
		if (entry.generateMatrix) {
			assertDimensionList(entry.matrixRows, `matrixRows for ${entry.name}`);
			assertDimensionList(entry.matrixColumns, `matrixColumns for ${entry.name}`);
		}
		if (!entry.generateVector) {
			assertOrThrow(!entry.vectorDimensions, `non-vector scalar type ${entry.name} must not define vectorDimensions`);
		}
		if (!entry.generateMatrix) {
			assertOrThrow(!entry.matrixRows && !entry.matrixColumns, `non-matrix scalar type ${entry.name} must not define matrix dimensions`);
		}
	}
	const generatedNames = generatedScalarTypeNames(scalarTypes);
	for (const requiredName of REQUIRED_GENERATED_SCALAR_TYPE_NAMES) {
		assertOrThrow(generatedNames.has(requiredName), `missing generated scalar/vector/matrix type: ${requiredName}`);
	}
}

function validateTypeCoverage(objectTypes, families) {
	assertOrThrow(objectTypes.entries.length >= MIN_OBJECT_TYPE_COUNT, `object type entries < ${MIN_OBJECT_TYPE_COUNT}`);
	assertUniqueNames(objectTypes.entries, 'object type');
	const typeNames = new Set(objectTypes.entries.map((item) => item.name));
	const entriesByName = new Map(objectTypes.entries.map((item) => [item.name, item]));
	for (const requiredType of REQUIRED_OBJECT_TYPES) {
		assertOrThrow(typeNames.has(requiredType), `missing required object type: ${requiredType}`);
	}
	const familyMap = new Map();
	for (const family of families.families) {
		assertOrThrow(!familyMap.has(family.name), `duplicate object family: ${family.name}`);
		assertUniqueStrings(family.members || [], `member in family ${family.name}`);
		assertUniqueStrings(family.compatibleWith || [], `compatibleWith in family ${family.name}`);
		familyMap.set(family.name, family);
	}
	for (const familyName of REQUIRED_FAMILIES) {
		assertOrThrow(familyMap.has(familyName), `missing required family: ${familyName}`);
		assertOrThrow(familyMap.get(familyName).members.length > 0, `family ${familyName} must have members`);
	}
	for (const family of families.families) {
		for (const compatibleFamily of family.compatibleWith || []) {
			assertOrThrow(familyMap.has(compatibleFamily), `family ${family.name} references unknown compatible family: ${compatibleFamily}`);
		}
		for (const member of family.members) {
			assertOrThrow(typeNames.has(member), `family ${family.name} references unknown type: ${member}`);
			const memberEntry = entriesByName.get(member);
			assertOrThrow(memberEntry.family === family.name, `family ${family.name} includes ${member}, but object type declares family ${memberEntry.family}`);
		}
	}
	for (const entry of objectTypes.entries) {
		const family = familyMap.get(entry.family);
		assertOrThrow(family, `object type ${entry.name} references unknown family: ${entry.family}`);
		assertOrThrow(family.members.includes(entry.name), `object type ${entry.name} is missing from family ${entry.family}`);
	}
}

function validateMethodCoverage(methods) {
	assertOrThrow(methods.entries.length >= MIN_METHOD_COUNT, `object method entries < ${MIN_METHOD_COUNT}`);
	const methodNames = assertUniqueNames(methods.entries, 'object method');
	for (const requiredMethod of REQUIRED_METHODS) {
		assertOrThrow(methodNames.has(requiredMethod), `missing required method: ${requiredMethod}`);
	}
}

function validateKeywordCoverage(keywords) {
	assertOrThrow(Array.isArray(keywords.entries), 'keyword base entries must be array');
	assertOrThrow(keywords.entries.length >= MIN_KEYWORD_COUNT, `keyword base entries < ${MIN_KEYWORD_COUNT}`);
	assertUniqueNames(keywords.entries, 'keyword');
	const keywordNames = new Set(keywords.entries.map((item) => item.name));
	for (const requiredKeyword of REQUIRED_KEYWORDS) {
		assertOrThrow(keywordNames.has(requiredKeyword), `language keywords base must include ${requiredKeyword}`);
	}
	for (const forbiddenTypeToken of FORBIDDEN_KEYWORD_TYPE_TOKENS) {
		assertOrThrow(!keywordNames.has(forbiddenTypeToken), `type token leaked into language keywords: ${forbiddenTypeToken}`);
	}
}

function validateDirectiveCoverage(directives) {
	assertOrThrow(Array.isArray(directives.entries), 'directive base entries must be array');
	assertUniqueNames(directives.entries, 'directive');
	const directiveNames = new Set(directives.entries.map((item) => item.name));
	for (const requiredDirective of ['include', 'define', 'undef', 'if', 'ifdef', 'ifndef', 'elif', 'else', 'endif', 'pragma']) {
		assertOrThrow(directiveNames.has(requiredDirective), `language directives base must include ${requiredDirective}`);
	}
}

function validateSemanticCoverage(semantics) {
	assertOrThrow(Array.isArray(semantics.entries), 'semantic base entries must be array');
	assertUniqueNames(semantics.entries, 'semantic', { caseInsensitive: false });
	const semanticNames = new Set(semantics.entries.map((item) => item.name));
	for (const requiredSemantic of ['SV_Position', 'SV_Target0', 'SV_Depth']) {
		assertOrThrow(semanticNames.has(requiredSemantic), `language semantics base must include ${requiredSemantic}`);
	}
}

function validatePreprocessorMacroCoverage(macros) {
	assertOrThrow(Array.isArray(macros.entries), 'preprocessor macro base entries must be array');
	assertUniqueNames(macros.entries, 'preprocessor macro');
	const macroNames = new Set(macros.entries.map((item) => item.name));
	for (const requiredMacro of [
		'SHADER_QUALITY',
		'QUALITY_SUPPORT_MIDDLE',
		'QUALITY_SUPPORT_HIGH',
		'PLAYERS_SELF',
		'API_MOBILE_HIGH_QUALITY',
		'API_PC_HIGH_QUALITY',
		'API_SUPPORT_SV_INSTANCE_ID',
		'API_SUPPORT_TEXFETCH',
		'API_SUPPORT_SV_VERTEX_ID',
		'SYSTEM_SUPPORT_DEPTH_BUFFER_AS_TEXTURE'
	]) {
		assertOrThrow(macroNames.has(requiredMacro), `language preprocessor macros base must include ${requiredMacro}`);
	}
}

function main() {
	const loaded = {};
	for (const check of schemaChecks) {
		if (!fs.existsSync(check.dataPath)) throw new Error(`${check.label}: missing file ${check.dataPath}`);
		if (!fs.existsSync(check.schemaPath)) throw new Error(`${check.label}: missing schema ${check.schemaPath}`);
		const data = readJson(check.dataPath);
		const schema = readJson(check.schemaPath);
		const errors = [];
		validateBySchema(data, schema, '$', errors);
		if (errors.length > 0) {
			throw new Error(`${check.label}: schema validation failed\n${errors.join('\n')}`);
		}
		loaded[check.label] = data;
	}

	validateBuiltinsCoverage(loaded['builtins base']);
	validateScalarTypeCoverage(loaded['scalar types']);
	validateTypeCoverage(loaded['object types'], loaded['object families']);
	validateMethodCoverage(loaded['object methods']);
	validateKeywordCoverage(loaded['keyword base']);
	validateDirectiveCoverage(loaded['directive base']);
	validateSemanticCoverage(loaded['semantic base']);
	validatePreprocessorMacroCoverage(loaded['preprocessor macro base']);
	process.stdout.write('json:validate passed\n');
}

main();
