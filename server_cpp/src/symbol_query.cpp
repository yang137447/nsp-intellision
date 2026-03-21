#include "symbol_query.hpp"

#include "server_request_handlers.hpp"
#include "workspace_index.hpp"
#include "workspace_scan_plan.hpp"

#include <unordered_set>
#include <utility>
#include <vector>

bool findDefinitionInIncludeGraph(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    std::unordered_set<std::string> &visited, DefinitionLocation &outLocation);
bool findDefinitionByWorkspaceScan(const std::string &symbol,
                                   const std::vector<std::string> &roots,
                                   const std::vector<std::string> &extensions,
                                   DefinitionLocation &outLocation,
                                   bool exactWordBoundary = true);

namespace {

bool resolveMacroGeneratedFunctionDefinition(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    ResolvedSymbolDefinitionTarget &out) {
  std::string docText;
  if (!ctx.readDocumentText(uri, docText)) {
    return false;
  }
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

bool resolveWorkspaceSymbolDefinition(const std::string &uri,
                                      const std::string &word,
                                      ServerRequestContext &ctx,
                                      const SymbolDefinitionResolveOptions &options,
                                      ResolvedSymbolDefinitionTarget &out) {
  DefinitionLocation indexed;
  if (workspaceIndexFindDefinition(word, indexed)) {
    out.source = SymbolDefinitionTargetSource::WorkspaceIndex;
    out.location = indexed;
    out.found = true;
    out.hasMacroGeneratedFunction = false;
    return true;
  }

  if (!options.allowWorkspaceScan) {
    return false;
  }

  WorkspaceScanPlanOptions scanPlanOptions;
  scanPlanOptions.includeDocumentDirectory = options.includeDocumentDirectoryInScan;
  scanPlanOptions.requiredExtensions = {".nsf", ".hlsli", ".h"};
  if (options.excludeUsfUshInScan) {
    scanPlanOptions.excludedExtensions = {".usf", ".ush"};
  }
  WorkspaceScanPlan scanPlan =
      buildWorkspaceScanPlan(ctx, uri, scanPlanOptions);
  if (options.useWorkspaceScanCache) {
    resetWorkspaceScanCachesIfPlanChanged(ctx, scanPlan);
    const std::string scanLookupKey = uri + "|" + word;
    auto cached = ctx.scanDefinitionCache.find(scanLookupKey);
    if (cached != ctx.scanDefinitionCache.end()) {
      out.source = SymbolDefinitionTargetSource::WorkspaceScan;
      out.location = cached->second;
      out.found = true;
      out.hasMacroGeneratedFunction = false;
      return true;
    }
    const bool hadMiss = ctx.scanDefinitionMisses.find(scanLookupKey) !=
                         ctx.scanDefinitionMisses.end();
    if (!hadMiss) {
      DefinitionLocation scanned;
      if (findDefinitionByWorkspaceScan(word, scanPlan.roots, scanPlan.extensions,
                                        scanned, true)) {
        ctx.scanDefinitionCache.emplace(scanLookupKey, scanned);
        ctx.scanDefinitionMisses.erase(scanLookupKey);
        out.source = SymbolDefinitionTargetSource::WorkspaceScan;
        out.location = scanned;
        out.found = true;
        out.hasMacroGeneratedFunction = false;
        return true;
      }
      ctx.scanDefinitionMisses.insert(scanLookupKey);
      return false;
    }
    if (workspaceIndexIsReady() && workspaceIndexFindDefinition(word, indexed)) {
      ctx.scanDefinitionCache.emplace(scanLookupKey, indexed);
      ctx.scanDefinitionMisses.erase(scanLookupKey);
      out.source = SymbolDefinitionTargetSource::WorkspaceIndex;
      out.location = indexed;
      out.found = true;
      out.hasMacroGeneratedFunction = false;
      return true;
    }
    return false;
  }

  DefinitionLocation scanned;
  if (findDefinitionByWorkspaceScan(word, scanPlan.roots, scanPlan.extensions,
                                    scanned, true)) {
    out.source = SymbolDefinitionTargetSource::WorkspaceScan;
    out.location = scanned;
    out.found = true;
    out.hasMacroGeneratedFunction = false;
    return true;
  }
  return false;
}

bool resolveIncludeGraphSymbolDefinition(const std::string &uri,
                                         const std::string &word,
                                         ServerRequestContext &ctx,
                                         ResolvedSymbolDefinitionTarget &out) {
  std::unordered_set<std::string> visited;
  DefinitionLocation location;
  if (!findDefinitionInIncludeGraph(uri, word, ctx.documentSnapshot(),
                                    ctx.workspaceFolders, ctx.includePaths,
                                    ctx.shaderExtensions,
                                    ctx.preprocessorDefines, visited,
                                    location)) {
    return false;
  }
  out.source = SymbolDefinitionTargetSource::IncludeGraph;
  out.location = location;
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
  case SymbolDefinitionSearchOrder::WorkspaceThenGraphThenMacro:
    if (resolveWorkspaceSymbolDefinition(uri, word, ctx, options, out)) {
      return true;
    }
    if (resolveIncludeGraphSymbolDefinition(uri, word, ctx, out)) {
      return true;
    }
    if (resolveMacroGeneratedFunctionDefinition(uri, word, ctx, out)) {
      return true;
    }
    return false;
  case SymbolDefinitionSearchOrder::MacroThenWorkspaceThenScan:
    if (resolveMacroGeneratedFunctionDefinition(uri, word, ctx, out)) {
      return true;
    }
    if (resolveWorkspaceSymbolDefinition(uri, word, ctx, options, out)) {
      return true;
    }
    return false;
  }
  return false;
}
