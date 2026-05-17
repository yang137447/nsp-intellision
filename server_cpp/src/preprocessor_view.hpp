#pragma once

#include "conditional_ast.hpp"
#include "preprocessor_macro_settings.hpp"

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

// Active macro metadata captured by preprocessor interpretation.
//
// `replacement` is the object-like replacement text reconstructed from tokens.
// Function-like macros are reported but are not safe for expression consumers
// to expand without modeling parameters.
struct PreprocessorMacroReplacement {
  bool functionLike = false;
  std::string replacement;
};

// Source-level macro mutation that becomes visible after `line`.
struct PreprocessorMacroEvent {
  int line = 0;
  std::string name;
  bool undefined = false;
  PreprocessorMacroReplacement replacement;
};

struct PreprocessorView {
  std::vector<char> lineActive;
  std::vector<PreprocBranchSig> branchSigs;
  std::vector<PreprocessorConditionDiagnostic> conditionDiagnostics;
  std::vector<std::string> activeIncludeUris;
  std::unordered_map<std::string, PreprocessorMacroReplacement>
      initialMacroReplacements;
  std::vector<PreprocessorMacroEvent> macroEvents;
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

// Updates the process-wide effective `nsf.preprocessorMacros` table consumed
// by preprocessor view construction. The caller owns configuration parsing and
// must refresh document analysis context after changing this map.
void setConfiguredPreprocessorMacros(
    const ConfiguredPreprocessorMacros &macros);

std::string getConfiguredPreprocessorMacrosFingerprint();

// Looks up the macro replacement active before `line`.
//
// Inputs: a PreprocessorView produced for the same document/context, a 0-based
// line number, and a macro name.
// Output: true plus replacement metadata when the macro is defined at that
// point; false when it is not defined.
// Non-goals: this API does not expand function-like macro arguments, evaluate
// replacement types, or provide fallback semantic analysis.
bool lookupActivePreprocessorMacroReplacement(
    const PreprocessorView &view, int line, const std::string &name,
    PreprocessorMacroReplacement &replacementOut);

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
