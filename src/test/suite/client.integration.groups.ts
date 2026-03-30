export { registerInteractiveRuntimeCoreTests } from './integration/interactive-core';

export { registerDiagnosticsTests } from './integration/diagnostics';

export {
	registerDeferredDocActiveUnitAnalysisContextTests,
	registerDeferredDocDefinesAnalysisContextTests,
	registerDeferredDocIncludeClosureAnalysisContextTests,
	registerDeferredDocSharedKeyAnalysisContextTests,
	registerDeferredDocWorkspaceSummaryAnalysisContextTests
} from './integration/analysis-context';

export {
	registerDeferredDocDocumentSymbolTests,
	registerDeferredDocInlayTests,
	registerDeferredDocSemanticTokenTests
} from './integration/deferred-doc';

export {
	registerRuntimeConfigLanguageOwnershipTests,
	registerRuntimeExternalFileConfigTests,
	registerRuntimeFileWatchTests,
	registerRuntimeIncludePathConfigTests,
	registerRuntimeIndexingTests
} from './integration/runtime-config';

export {
	registerWorkspaceSummaryDefinitionFallbackTests,
	registerWorkspaceSummarySchedulingTests,
	registerWorkspaceSummaryTypeFallbackTests
} from './integration/workspace-summary';

export { registerInteractiveRuntimeSignatureTests } from './integration/interactive-signature';

export {
	registerDefinitionProviderTests,
	registerInteractiveStructMemberCompletionTests
} from './integration/interactive-support';

export { registerReferencesRenameTests } from './integration/references-rename';

export { registerInteractiveUiMetadataTests } from './integration/ui-metadata';

