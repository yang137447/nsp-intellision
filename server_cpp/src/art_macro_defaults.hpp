#pragma once

#include <string>
#include <vector>

// Shared metadata for Neox `#art` BOOL/INT macro declarations that provide a
// shader-editing default of numeric zero, plus stable integer enum constants
// declared in the same parameter block.
//
// Responsibilities:
// - carry workspace-indexed `#art NAME "..." "BOOL"/"INT"` declarations
//   into preprocessor initialization
// - carry source-bound companion integer constants immediately associated with
//   an art macro so selector comparisons can be evaluated without promoting
//   material-family-specific constants into the global preset
// - preserve source location so hover, definition, diagnostics, and audit
//   observe the same default-zero source
//
// Non-goals:
// - does not assign defaults for non-BOOL/INT art directives
// - does not override configured/profile/source macro definitions
// - does not treat companion constants as globally stable across materials
struct ArtCompanionConstant {
  std::string name;
  int value = 0;
  std::string uri;
  int line = 0;
  int start = 0;
  int end = 0;
};

struct ArtDefaultZeroMacro {
  std::string name;
  std::string artType;
  std::string uri;
  int line = 0;
  int start = 0;
  int end = 0;
  std::vector<ArtCompanionConstant> companionConstants;
};
