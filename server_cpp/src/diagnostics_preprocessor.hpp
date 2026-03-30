#pragma once

#include "diagnostics.hpp"
#include "preprocessor_view.hpp"

#include <string>
#include <unordered_map>
#include <vector>

PreprocessorView buildDiagnosticsPreprocessorView(
    const std::string &uri, const std::string &text,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const DiagnosticsBuildOptions &options);

bool findIncludePathSpan(const std::string &lineText, size_t includePos,
                         int &startOut, int &endOut);

size_t findIncludeDirectiveOutsideComments(const std::string &lineText,
                                           bool &inBlockComment);

bool hasUnterminatedBlockComment(const std::string &text, int &lineOut,
                                 int &charOut);
