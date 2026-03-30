#pragma once

#include "definition_location.hpp"
#include "macro_generated_functions.hpp"

#include <string>

struct ServerRequestContext;

enum class SymbolDefinitionTargetSource {
  None,
  WorkspaceIndex,
  MacroGenerated
};

enum class SymbolDefinitionSearchOrder {
  WorkspaceThenMacro,
  MacroThenWorkspace
};

struct SymbolDefinitionResolveOptions {
  SymbolDefinitionSearchOrder order =
      SymbolDefinitionSearchOrder::WorkspaceThenMacro;
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
