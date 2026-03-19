#pragma once

#include "definition_location.hpp"
#include "json.hpp"

#include <string>
#include <unordered_map>
#include <vector>

struct IndexedStructMember {
  std::string name;
  std::string type;
};

struct IndexedStruct {
  std::string name;
  std::vector<IndexedStructMember> members;
};

struct IndexedDefinition {
  std::string name;
  std::string type;
  std::string uri;
  int line = 0;
  int start = 0;
  int end = 0;
  int kind = 0;
};

void workspaceIndexConfigure(const std::vector<std::string> &workspaceFolders,
                             const std::vector<std::string> &includePaths,
                             const std::vector<std::string> &shaderExtensions);

void workspaceIndexHandleFileChanges(const std::vector<std::string> &uris);

bool workspaceIndexFindDefinition(const std::string &symbol,
                                  DefinitionLocation &outLocation);

bool workspaceIndexFindStructDefinition(const std::string &symbol,
                                        DefinitionLocation &outLocation);

bool workspaceIndexFindDefinitions(const std::string &symbol,
                                   std::vector<IndexedDefinition> &outDefs,
                                   size_t limit);

bool workspaceIndexGetStructFields(const std::string &structName,
                                   std::vector<std::string> &outFields);

bool workspaceIndexGetStructMemberType(const std::string &structName,
                                       const std::string &memberName,
                                       std::string &outType);

bool workspaceIndexGetSymbolType(const std::string &symbol,
                                 std::string &outType);

bool workspaceIndexIsReady();
Json workspaceIndexGetIndexingState();
void workspaceIndexKickIndexing(const std::string &reason);
void workspaceIndexSetConcurrencyLimits(size_t workerCount,
                                        size_t queueCapacity);

void workspaceIndexCollectReverseIncludeClosure(
    const std::vector<std::string> &uris, std::vector<std::string> &outPaths,
    size_t limit);

void workspaceIndexShutdown();
