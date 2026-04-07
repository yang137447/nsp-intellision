import {
	registerDefinitionProviderTests,
	registerInteractiveRuntimeCoreTests,
	registerInteractiveRuntimeSignatureTests,
	registerInteractiveStructMemberCompletionTests
} from './client.integration.groups';

registerInteractiveRuntimeCoreTests();
registerInteractiveRuntimeSignatureTests();
registerInteractiveStructMemberCompletionTests();
registerDefinitionProviderTests();
