#pragma once

#include "conditional_ast.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct PreprocessorConditionDiagnostic {
  int line = 0;
  int start = 0;
  int end = 0;
  int severity = 1;
  std::string message;
};

using PreprocBranchSig = std::vector<std::pair<int, int>>;

struct PreprocessorView {
  std::vector<char> lineActive;
  std::vector<PreprocBranchSig> branchSigs;
  std::vector<PreprocessorConditionDiagnostic> conditionDiagnostics;
  std::vector<std::string> activeIncludeUris;
};

struct PreprocessorIncludeContext {
  std::string currentUri;
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  std::function<bool(const std::string &, std::string &)> loadText;
  int maxDepth = 32;
  bool collectIncludeConditionDiagnostics = false;
};

PreprocessorView
buildPreprocessorView(const ConditionalAst &ast,
                      const std::unordered_map<std::string, int> &defines);

PreprocessorView
buildPreprocessorView(const std::string &text,
                      const std::unordered_map<std::string, int> &defines);

PreprocessorView
buildPreprocessorView(const std::string &text,
                      const std::unordered_map<std::string, int> &defines,
                      const PreprocessorIncludeContext &includeContext);

// Interprets one active unit and captures the active preprocessor view that the
// unit supplies to one included target document. Returns false when the target
// document is not reached through the active include chain.
bool buildIncludedDocumentPreprocessorView(
    const std::string &rootText,
    const std::unordered_map<std::string, int> &defines,
    const PreprocessorIncludeContext &includeContext,
    const std::string &targetUri, PreprocessorView &resultOut);
