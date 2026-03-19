const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..', '..');
const resourcesRoot = path.join(repoRoot, 'server_cpp', 'resources');
const builtinsRoot = path.join(resourcesRoot, 'builtins', 'intrinsics');
const methodsRoot = path.join(resourcesRoot, 'methods', 'object_methods');
const keywordsRoot = path.join(resourcesRoot, 'language', 'keywords');
const directivesRoot = path.join(resourcesRoot, 'language', 'directives');
const semanticsRoot = path.join(resourcesRoot, 'language', 'semantics');
const objectTypesRoot = path.join(resourcesRoot, 'types', 'object_types');
const objectFamiliesRoot = path.join(resourcesRoot, 'types', 'object_families');
const typeOverridesRoot = path.join(resourcesRoot, 'types', 'type_overrides');

const MIN_OBJECT_TYPE_COUNT = 24;
const MIN_METHOD_COUNT = 12;

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
	'SamplerComparisonState'
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
		dataPath: path.join(objectTypesRoot, 'base.json'),
		schemaPath: path.join(objectTypesRoot, 'schema.json')
	},
	{
		label: 'object type overrides',
		dataPath: path.join(objectTypesRoot, 'override.json'),
		schemaPath: path.join(objectTypesRoot, 'schema.json')
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
		dataPath: path.join(keywordsRoot, 'base.json'),
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

function validateBuiltinsCoverage(baseBundle) {
	assertOrThrow(Array.isArray(baseBundle.entries), 'builtins base entries must be array');
	assertOrThrow(baseBundle.entries.length > 0, 'builtins base must contain entries');
	if (typeof baseBundle.count === 'number') {
		assertOrThrow(baseBundle.count === baseBundle.entries.length, 'builtins base count must equal entries length');
	}
	const names = new Set();
	for (const entry of baseBundle.entries) {
		assertOrThrow(typeof entry.name === 'string' && entry.name.length > 0, 'builtin entry name must be non-empty');
		assertOrThrow(!names.has(entry.name.toLowerCase()), `duplicate builtin entry: ${entry.name}`);
		names.add(entry.name.toLowerCase());
	}
}

function validateTypeCoverage(objectTypes, families) {
	assertOrThrow(objectTypes.entries.length >= MIN_OBJECT_TYPE_COUNT, `object type entries < ${MIN_OBJECT_TYPE_COUNT}`);
	const typeNames = new Set(objectTypes.entries.map((item) => item.name));
	for (const requiredType of REQUIRED_OBJECT_TYPES) {
		assertOrThrow(typeNames.has(requiredType), `missing required object type: ${requiredType}`);
	}
	const familyMap = new Map();
	for (const family of families.families) {
		familyMap.set(family.name, family);
	}
	for (const familyName of REQUIRED_FAMILIES) {
		assertOrThrow(familyMap.has(familyName), `missing required family: ${familyName}`);
		assertOrThrow(familyMap.get(familyName).members.length > 0, `family ${familyName} must have members`);
	}
	for (const family of families.families) {
		for (const member of family.members) {
			assertOrThrow(typeNames.has(member), `family ${family.name} references unknown type: ${member}`);
		}
	}
}

function validateMethodCoverage(methods) {
	assertOrThrow(methods.entries.length >= MIN_METHOD_COUNT, `object method entries < ${MIN_METHOD_COUNT}`);
	const methodNames = new Set(methods.entries.map((item) => item.name));
	for (const requiredMethod of REQUIRED_METHODS) {
		assertOrThrow(methodNames.has(requiredMethod), `missing required method: ${requiredMethod}`);
	}
}

function validateKeywordCoverage(keywords) {
	assertOrThrow(Array.isArray(keywords.entries), 'keyword base entries must be array');
	const keywordNames = new Set(keywords.entries.map((item) => item.name));
	assertOrThrow(keywordNames.has('discard'), 'language keywords base must include discard');
}

function validateDirectiveCoverage(directives) {
	assertOrThrow(Array.isArray(directives.entries), 'directive base entries must be array');
	const directiveNames = new Set(directives.entries.map((item) => item.name));
	for (const requiredDirective of ['include', 'define', 'undef', 'if', 'ifdef', 'ifndef', 'elif', 'else', 'endif', 'pragma']) {
		assertOrThrow(directiveNames.has(requiredDirective), `language directives base must include ${requiredDirective}`);
	}
}

function validateSemanticCoverage(semantics) {
	assertOrThrow(Array.isArray(semantics.entries), 'semantic base entries must be array');
	const semanticNames = new Set(semantics.entries.map((item) => item.name));
	for (const requiredSemantic of ['SV_Position', 'SV_Target0', 'SV_Depth']) {
		assertOrThrow(semanticNames.has(requiredSemantic), `language semantics base must include ${requiredSemantic}`);
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
	validateTypeCoverage(loaded['object types'], loaded['object families']);
	validateMethodCoverage(loaded['object methods']);
	validateKeywordCoverage(loaded['keyword base']);
	validateDirectiveCoverage(loaded['directive base']);
	validateSemanticCoverage(loaded['semantic base']);
	process.stdout.write('json:validate passed\n');
}

main();
