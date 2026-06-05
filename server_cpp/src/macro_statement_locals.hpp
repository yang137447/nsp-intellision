#pragma once

// Statement-like macro local declaration extraction.
//
// Responsibilities:
// - identify active object-like macro invocations that occupy a statement line
// - parse declaration statements from the macro replacement text
// - return source and invocation locations that semantic snapshots and
//   diagnostics can consume through their existing lexical-scope models
//
// Non-goals:
// - this is not a general macro expansion API
// - it does not evaluate function-like macros or macro arguments
// - it does not synthesize locals for expressions that are not standalone macro
//   statements

#include "preprocessor_view.hpp"

#include <cstddef>
#include <string>
#include <vector>

struct MacroStatementLocalDeclaration {
  std::string name;
  std::string type;
  std::string macroName;
  int invocationLine = -1;
  int invocationStart = 0;
  int invocationEnd = 0;
  size_t invocationOffset = 0;
  std::string sourceUri;
  int sourceLine = -1;
  int sourceStart = 0;
  int sourceEnd = 0;
};

std::vector<MacroStatementLocalDeclaration>
collectStatementLikeMacroLocalDeclarations(const PreprocessorView &view,
                                           int lineIndex,
                                           const std::string &lineText,
                                           size_t lineStartOffset = 0);
