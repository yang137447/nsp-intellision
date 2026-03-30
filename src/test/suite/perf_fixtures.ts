export type PerfFixtureSpec = {
	id: string;
	label: string;
	primaryDocument: string;
	notes: string;
	semanticTokensDocument?: string;
	inlayDocument?: string;
	diagnosticsDocument?: string;
	documentSymbolsDocument?: string;
};

export const PERF_FIXTURES: Record<string, PerfFixtureSpec> = {
	pfx1SmallCurrentDoc: {
		id: 'PFX-1',
		label: 'SmallCurrentDoc',
		primaryDocument: 'module_completion_current_doc.nsf',
		notes: 'Small current-doc interactive fixture for completion, hover, and short-path edit latency.'
	},
	pfx2MediumEditPath: {
		id: 'PFX-2',
		label: 'MediumEditPath',
		primaryDocument: 'module_suite.nsf',
		notes: 'Medium interactive fixture with common edit-path requests and cross-feature reuse.',
		semanticTokensDocument: 'module_semantic_tokens.nsf',
		inlayDocument: 'module_inlay_hints.nsf',
		diagnosticsDocument: 'module_diagnostics_missing_semicolon.nsf',
		documentSymbolsDocument: 'module_suite.nsf'
	},
	pfx3LargeCurrentDoc: {
		id: 'PFX-3',
		label: 'LargeCurrentDoc',
		primaryDocument: 'module_perf_large_current_doc.nsf',
		notes: 'Large generated current-doc fixture for deferred runtime and long-document baselines.',
		semanticTokensDocument: 'module_perf_large_current_doc.nsf',
		inlayDocument: 'module_perf_large_current_doc.nsf',
		diagnosticsDocument: 'module_perf_large_current_doc.nsf',
		documentSymbolsDocument: 'module_perf_large_current_doc.nsf'
	},
	pfx4ActiveUnitAmbiguous: {
		id: 'PFX-4',
		label: 'ActiveUnitAmbiguous',
		primaryDocument: 'include_context/shared/multi_context_symbol_common.hlsl',
		notes: 'Multi-root include-context fixture for active unit and ambiguous candidate behavior.'
	},
	pfx5ReverseIncludeImpact: {
		id: 'PFX-5',
		label: 'ReverseIncludeImpact',
		primaryDocument: 'watch_consumer.nsf',
		notes: 'Provider/consumer fixture for file-watch, reverse-include, and cross-file diagnostic refresh.'
	}
};
