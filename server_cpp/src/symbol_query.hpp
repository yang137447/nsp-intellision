#pragma once

#include "definition_location.hpp"
#include "macro_generated_functions.hpp"

#include <string>

struct ServerRequestContext;

enum class SymbolDefinitionTargetSource {
  None,
  WorkspaceIndex,
  WorkspaceScan,
  IncludeGraph,
  MacroGenerated
};

enum class SymbolDefinitionSearchOrder {
  WorkspaceThenGraphThenMacro,
  MacroThenWorkspaceThenScan
};

struct SymbolDefinitionResolveOptions {
  SymbolDefinitionSearchOrder order =
      SymbolDefinitionSearchOrder::WorkspaceThenGraphThenMacro;
  bool allowWorkspaceScan = false;
  bool useWorkspaceScanCache = false;
  bool includeDocumentDirectoryInScan = true;
  bool excludeUsfUshInScan = false;
};

struct ResolvedSymbolDefinitionTarget {
  SymbolDefinitionTargetSource source = SymbolDefinitionTargetSource::None;
  DefinitionLocation location;
  bool found = false;
  bool hasMacroGeneratedFunction = false;
  MacroGeneratedFunctionInfo macroGeneratedFunction;
};

bool resolveSymbolDefinitionTarget(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    const SymbolDefinitionResolveOptions &options,
    ResolvedSymbolDefinitionTarget &out);
