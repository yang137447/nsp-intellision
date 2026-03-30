import {
	registerDeferredDocDocumentSymbolTests,
	registerDeferredDocInlayTests,
	registerDeferredDocSemanticTokenTests
} from './client.integration.groups';

registerDeferredDocSemanticTokenTests();
registerDeferredDocInlayTests();
registerDeferredDocDocumentSymbolTests();
