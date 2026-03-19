#pragma once

#include "definition_location.hpp"

#include <string>
#include <vector>

bool findDefinitionByWorkspaceScan(const std::string &symbol,
                                   const std::vector<std::string> &roots,
                                   const std::vector<std::string> &extensions,
                                   DefinitionLocation &outLocation);

bool findDefinitionByWorkspaceScan(const std::string &symbol,
                                   const std::vector<std::string> &roots,
                                   const std::vector<std::string> &extensions,
                                   DefinitionLocation &outLocation,
                                   bool consultIndex);

bool findStructDefinitionByWorkspaceScan(
    const std::string &symbol, const std::vector<std::string> &roots,
    const std::vector<std::string> &extensions,
    DefinitionLocation &outLocation);
