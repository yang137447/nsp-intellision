#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// Shared active-unit macro-profile lookup.
//
// Responsibilities:
// - discover shadercompiler-exported local variant sources under the configured
//   workspace/include roots and optional `nsf.shaderCompilerPath`
// - map one active unit path to its shader key
// - extract only explicit per-unit numeric macros that remain stable across all
//   local variants / used-variant rows for that shader key
// - report variant-axis macros that exist in the profile source but cannot be
//   resolved to one value under the current active-unit context
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
  std::string sourceKind;
  // Row-level profile resolution metadata for debug/audit only.
  int profileTotalRowCount = 0;
  int profileSelectedRowCount = 0;
  std::string profileSelectedRowSignature;
  std::string profileSelectionHintSourcePath;
  // Macro names that are present in the matched profile source but still vary
  // across variant rows. They are diagnostic/debug metadata only and must not
  // be injected as effective defines.
  std::vector<std::string> unresolvedMacroNames;
  std::unordered_map<std::string, int> defines;
};

// Resolves explicit per-unit numeric macros for the active unit.
//
// Returns true when a matching shader entry was found in a discovered
// profile source, even if no unanimous explicit macros could be extracted.
// `selectionHints` should only contain explicit numeric context inputs
// (for example workspace defines/macros). The provider may use those hints to
// filter profile rows when the matched source already contains that macro.
// Callers should inject only `defines`.
bool resolveUnitMacroProfileSnapshot(
    const std::string &activeUnitPath,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::string &shaderCompilerPath,
    const std::unordered_map<std::string, int> &selectionHints,
    UnitMacroProfileSnapshot &snapshotOut);

// Returns whether the changed path/URI is one of the currently discovered
// profile-source files and therefore should refresh active-unit analysis
// context.
bool unitMacroProfileProviderOwnsPath(
    const std::string &pathOrUri,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::string &shaderCompilerPath);
