#pragma once

#include "diagnostics.hpp"
#include "diagnostics_prerequisites.hpp"
#include "preprocessor_view.hpp"

#include <string>
#include <unordered_map>
#include <vector>

// Preprocessor context plus diagnostics semantic-prerequisite metadata.
// If an included target cannot be proven to belong to the active NSF unit's
// include closure, callers still receive a local preprocessor view for syntax
// checks but semantic rules must treat preprocessor/include context as
// unreliable and avoid high-confidence diagnostics.
struct DiagnosticsPreprocessorBuildResult {
  PreprocessorView view;
  DiagnosticsPrerequisiteState prerequisites;
};

DiagnosticsPreprocessorBuildResult buildDiagnosticsPreprocessorContext(
    const std::string &uri, const std::string &text,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const DiagnosticsBuildOptions &options);

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
