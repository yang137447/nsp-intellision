import {
	registerWorkspaceSummarySchedulingTests,
	registerWorkspaceSummaryDefinitionFallbackTests,
	registerWorkspaceSummaryTypeFallbackTests
} from './client.integration.groups';

registerWorkspaceSummaryTypeFallbackTests();
registerWorkspaceSummaryDefinitionFallbackTests();
registerWorkspaceSummarySchedulingTests();
