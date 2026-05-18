#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// Shared active-unit macro-profile lookup.
//
// Responsibilities:
// - discover shadercompiler-exported local variant files under the configured
//   workspace/include roots
// - map one active unit path to its shader key
// - extract only explicit per-unit numeric macros that remain stable across all
//   local variants for that shader key
//
// Non-goals:
// - does not guess selector/profile values when the export contains multiple
//   conflicting values
// - does not replace user `nsf.defines` overrides or source-level
//   `#define/#undef`
struct UnitMacroProfileSnapshot {
  bool foundShaderEntry = false;
  std::string shaderKey;
  std::string sourcePath;
  std::unordered_map<std::string, int> defines;
};

// Resolves explicit per-unit numeric macros for the active unit.
//
// Returns true when a matching shader entry was found in a discovered
// local-variants file, even if no unanimous explicit macros could be extracted.
// Callers should inject only `defines`.
bool resolveUnitMacroProfileSnapshot(
    const std::string &activeUnitPath,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    UnitMacroProfileSnapshot &snapshotOut);

// Returns whether the changed path/URI is one of the currently discovered
// local-variants files and therefore should refresh active-unit analysis
// context.
bool unitMacroProfileProviderOwnsPath(
    const std::string &pathOrUri,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths);
