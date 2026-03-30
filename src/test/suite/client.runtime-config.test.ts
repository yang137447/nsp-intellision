import {
	registerRuntimeConfigLanguageOwnershipTests,
	registerRuntimeExternalFileConfigTests,
	registerRuntimeFileWatchTests,
	registerRuntimeIncludePathConfigTests,
	registerRuntimeIndexingTests
} from './client.integration.groups';

registerRuntimeConfigLanguageOwnershipTests();
registerRuntimeIndexingTests();
registerRuntimeExternalFileConfigTests();
registerRuntimeFileWatchTests();
registerRuntimeIncludePathConfigTests();
