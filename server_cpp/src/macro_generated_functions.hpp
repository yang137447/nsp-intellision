#pragma once

#include "definition_location.hpp"

#include <string>
#include <vector>

struct MacroGeneratedFunctionInfo {
  std::string name;
  std::string returnType;
  std::string label;
  std::vector<std::string> parameterDecls;
  std::vector<std::string> parameterTypes;
  DefinitionLocation definition;
};

bool collectMacroGeneratedFunctions(
    const std::string &entryUri, const std::string &entryText,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::string &functionNameFilter,
    std::vector<MacroGeneratedFunctionInfo> &outCandidates, size_t limit = 16);
