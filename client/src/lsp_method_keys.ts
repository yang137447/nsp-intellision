export const LSP_METHOD_KEYS = {
	initialize: 'initialize',
	shutdown: 'shutdown',
	hover: 'textDocument/hover',
	definition: 'textDocument/definition',
	references: 'textDocument/references',
	documentHighlight: 'textDocument/documentHighlight',
	completion: 'textDocument/completion',
	signatureHelp: 'textDocument/signatureHelp',
	documentSymbol: 'textDocument/documentSymbol',
	workspaceSymbol: 'workspace/symbol',
	publishDiagnostics: 'textDocument/publishDiagnostics',
	inlayHint: 'textDocument/inlayHint',
	didChangeConfiguration: 'workspace/didChangeConfiguration',
	setActiveUnit: 'nsf/setActiveUnit'
} as const;
