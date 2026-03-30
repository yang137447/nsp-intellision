#include "workspace_summary_runtime.hpp"

#include <atomic>

namespace {

std::atomic<uint64_t> gWorkspaceSummaryRuntimeVersion{1};

void bumpWorkspaceSummaryVersion() {
  gWorkspaceSummaryRuntimeVersion.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

void workspaceSummaryRuntimeConfigure(
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions) {
  workspaceIndexConfigure(workspaceFolders, includePaths, shaderExtensions);
  bumpWorkspaceSummaryVersion();
}

void workspaceSummaryRuntimeHandleFileChanges(
    const std::vector<std::string> &uris) {
  workspaceIndexHandleFileChanges(uris);
  if (!uris.empty())
    bumpWorkspaceSummaryVersion();
}

bool workspaceSummaryRuntimeFindDefinition(const std::string &symbol,
                                           DefinitionLocation &outLocation) {
  return workspaceIndexFindDefinition(symbol, outLocation);
}

bool workspaceSummaryRuntimeFindStructDefinition(
    const std::string &symbol, DefinitionLocation &outLocation) {
  return workspaceIndexFindStructDefinition(symbol, outLocation);
}

bool workspaceSummaryRuntimeFindDefinitions(
    const std::string &symbol, std::vector<IndexedDefinition> &outDefs,
    size_t limit) {
  return workspaceIndexFindDefinitions(symbol, outDefs, limit);
}

bool workspaceSummaryRuntimeQuerySymbols(
    const std::string &query, std::vector<IndexedDefinition> &outDefs,
    size_t limit) {
  return workspaceIndexQuerySymbols(query, outDefs, limit);
}

bool workspaceSummaryRuntimeGetStructFields(const std::string &structName,
                                            std::vector<std::string> &outFields) {
  return workspaceIndexGetStructFields(structName, outFields);
}

bool workspaceSummaryRuntimeGetStructMemberType(const std::string &structName,
                                                const std::string &memberName,
                                                std::string &outType) {
  return workspaceIndexGetStructMemberType(structName, memberName, outType);
}

bool workspaceSummaryRuntimeGetSymbolType(const std::string &symbol,
                                          std::string &outType) {
  return workspaceIndexGetSymbolType(symbol, outType);
}

bool workspaceSummaryRuntimeIsReady() { return workspaceIndexIsReady(); }

Json workspaceSummaryRuntimeGetIndexingState() {
  return workspaceIndexGetIndexingState();
}

void workspaceSummaryRuntimeKickIndexing(const std::string &reason) {
  workspaceIndexKickIndexing(reason);
}

void workspaceSummaryRuntimeRebuild(const std::string &reason,
                                    bool clearDiskCache) {
  workspaceIndexRebuild(reason, clearDiskCache);
  bumpWorkspaceSummaryVersion();
}

void workspaceSummaryRuntimeSetConcurrencyLimits(size_t workerCount,
                                                 size_t queueCapacity) {
  workspaceIndexSetConcurrencyLimits(workerCount, queueCapacity);
}

void workspaceSummaryRuntimeCollectReverseIncludeClosure(
    const std::vector<std::string> &uris, std::vector<std::string> &outPaths,
    size_t limit) {
  workspaceIndexCollectReverseIncludeClosure(uris, outPaths, limit);
}

void workspaceSummaryRuntimeCollectIncludingUnits(
    const std::vector<std::string> &uris, std::vector<std::string> &outPaths,
    size_t limit) {
  workspaceIndexCollectIncludingUnits(uris, outPaths, limit);
}

void workspaceSummaryRuntimeCollectIncludeClosureForUnit(
    const std::string &unitPathOrUri, std::vector<std::string> &outPaths,
    size_t limit) {
  workspaceIndexCollectIncludeClosureForUnit(unitPathOrUri, outPaths, limit);
}

uint64_t workspaceSummaryRuntimeGetVersion() {
  return gWorkspaceSummaryRuntimeVersion.load(std::memory_order_relaxed);
}

void workspaceSummaryRuntimeShutdown() { workspaceIndexShutdown(); }
