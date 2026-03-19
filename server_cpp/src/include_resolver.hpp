#pragma once

#include <string>
#include <vector>

std::vector<std::string>
resolveIncludeCandidates(const std::string &currentUri,
                         const std::string &includePath,
                         const std::vector<std::string> &workspaceFolders,
                         const std::vector<std::string> &includePaths,
                         const std::vector<std::string> &shaderExtensions);
