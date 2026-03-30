#include "symbol_query.hpp"

#include "server_request_handlers.hpp"
#include "workspace_summary_runtime.hpp"

#include <utility>

namespace {

bool resolveMacroGeneratedFunctionDefinition(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    ResolvedSymbolDefinitionTarget &out) {
  std::string docText;
  if (!ctx.readDocumentText(uri, docText))
    return false;

  std::vector<MacroGeneratedFunctionInfo> macroCandidates;
  if (!collectMacroGeneratedFunctions(uri, docText, ctx.workspaceFolders,
                                      ctx.includePaths, ctx.shaderExtensions,
                                      word, macroCandidates, 1) ||
      macroCandidates.empty()) {
    return false;
  }

  out.source = SymbolDefinitionTargetSource::MacroGenerated;
  out.location = macroCandidates.front().definition;
  out.found = true;
  out.hasMacroGeneratedFunction = true;
  out.macroGeneratedFunction = std::move(macroCandidates.front());
  return true;
}

bool resolveWorkspaceSymbolDefinition(const std::string &word,
                                      ResolvedSymbolDefinitionTarget &out) {
  DefinitionLocation indexed;
  if (!workspaceSummaryRuntimeFindDefinition(word, indexed))
    return false;

  out.source = SymbolDefinitionTargetSource::WorkspaceIndex;
  out.location = indexed;
  out.found = true;
  out.hasMacroGeneratedFunction = false;
  return true;
}

} // namespace

bool resolveSymbolDefinitionTarget(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    const SymbolDefinitionResolveOptions &options,
    ResolvedSymbolDefinitionTarget &out) {
  out = ResolvedSymbolDefinitionTarget{};

  switch (options.order) {
  case SymbolDefinitionSearchOrder::WorkspaceThenMacro:
    if (resolveWorkspaceSymbolDefinition(word, out))
      return true;
    if (resolveMacroGeneratedFunctionDefinition(uri, word, ctx, out))
      return true;
    return false;
  case SymbolDefinitionSearchOrder::MacroThenWorkspace:
    if (resolveMacroGeneratedFunctionDefinition(uri, word, ctx, out))
      return true;
    if (resolveWorkspaceSymbolDefinition(word, out))
      return true;
    return false;
  }
  return false;
}
