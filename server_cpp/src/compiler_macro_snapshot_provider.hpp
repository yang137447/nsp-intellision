#pragma once

#include "conditional_ast.hpp"

#include <string>
#include <vector>

// Server-owned compiler macro snapshot extraction.
//
// Responsibilities:
// - collect the subset of shadercompiler macro inputs that can be reproduced
//   quickly from the active unit include closure in C++
// - expose source-backed object-like single-token macro replacements that the
//   preprocessor view can seed before evaluating #if/#elif conditions
// - treat #undef as clearing the current candidate so a later stable #define
//   in the active closure can re-establish it; conflicting or final-undefined
//   candidates are still excluded
// - keep the extraction independent from diagnostics/hover rendering so those
//   consumers continue to share PreprocessorView as the single query surface
//
// Non-goals:
// - does not execute shadercompiler or call Python helpers
// - does not emulate backend shader generation, unused-function stripping, or
//   expression/function-like macro rewriting
// - does not guess values from files outside the active unit closure

enum class CompilerMacroSnapshotMacroKind {
  PrivateAlias,
  PublicDefault,
};

struct CompilerMacroSnapshotMacro {
  std::string name;
  std::string replacement;
  std::string sourceUri;
  int sourceLine = -1;
  int sourceStart = 0;
  int sourceEnd = 0;
  CompilerMacroSnapshotMacroKind kind =
      CompilerMacroSnapshotMacroKind::PrivateAlias;
};

struct CompilerMacroSnapshotSource {
  std::string uri;
  const ConditionalAst *ast = nullptr;
};

struct CompilerMacroSnapshot {
  std::vector<CompilerMacroSnapshotMacro> macros;
};

// Builds a deterministic macro snapshot from the active unit source list.
// The caller owns the ConditionalAst lifetimes for the duration of the call.
CompilerMacroSnapshot buildCompilerMacroSnapshotFromSources(
    const std::vector<CompilerMacroSnapshotSource> &sources);
