import {
	registerDefinitionProviderTests,
	registerInteractiveRuntimeCoreTests,
	registerInteractiveRuntimeSignatureTests,
	registerInteractiveStructMemberCompletionTests,
	registerInteractiveUiMetadataTests
} from './client.integration.groups';

registerInteractiveRuntimeCoreTests();
registerInteractiveRuntimeSignatureTests();
registerInteractiveStructMemberCompletionTests();
registerDefinitionProviderTests();
registerInteractiveUiMetadataTests();
