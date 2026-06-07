#pragma once

#include "art_macro_defaults.hpp"
#include "conditional_ast.hpp"
#include "preprocessor_macro_settings.hpp"

#include <cstdint>
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
  std::string macroName;
  bool synthesizedZero = false;
  bool inactiveBranch = false;
  int branchId = 0;
  int branchIndex = 0;
};

using PreprocBranchSig = std::vector<std::pair<int, int>>;

// Active macro metadata captured by preprocessor interpretation.
//
// `replacement` is the replacement text reconstructed from tokens. Source
// fields are optional and identify the real macro mutation when it came from
// source/include text rather than configured inputs. Function-like macros are
// expanded by the shared preprocessor expression evaluator when they appear in
// `#if` / `#elif`; downstream expression consumers should still treat this as
// preprocessor metadata rather than a general HLSL macro expansion API.
struct PreprocessorMacroReplacement {
  bool functionLike = false;
  std::string replacement;
  std::string sourceUri;
  int sourceLine = -1;
  int sourceStart = 0;
  int sourceEnd = 0;
  bool sourceSynthesizedZero = false;
  bool sourceIfndefDefault = false;
  bool sourceArtDefaultZero = false;
  bool sourceArtCompanionConstant = false;
  bool sourceCompilerPrivateConstant = false;
  bool sourceCompilerMacroSnapshot = false;
};

// Source-level macro mutation that becomes visible after `line`.
struct PreprocessorMacroEvent {
  int line = 0;
  std::string name;
  bool undefined = false;
  bool synthesizedZero = false;
  bool ifndefDefault = false;
  bool artDefaultZero = false;
  bool compilerPrivateConstant = false;
  bool compilerMacroSnapshot = false;
  PreprocessorMacroReplacement replacement;
  // Source location of the mutation that produced this macro state. `line`
  // remains the visibility boundary in the current view, while these fields
  // point at the real #define/#undef or first synthesized use, including when a
  // parent unit observes the mutation through an included file.
  std::string sourceUri;
  int sourceLine = -1;
  int sourceStart = 0;
  int sourceEnd = 0;
};

struct PreprocessorBranchMergeInfo {
  int line = 0;
  int branchId = 0;
  int activeBranchIndex = -1;
  int branchCount = 0;
};

struct PreprocessorMacroHealthMetrics {
  uint64_t initialConfiguredMacroCount = 0;
  uint64_t initialArtDefaultZeroMacroCount = 0;
  uint64_t initialCompilerPrivateConstantCount = 0;
  uint64_t initialCompilerMacroSnapshotCount = 0;
  uint64_t initialNumericDefineCount = 0;
  uint64_t initialMacroCount = 0;
  uint64_t sourceDefineEvents = 0;
  uint64_t ifndefDefaultDefineEvents = 0;
  uint64_t sourceUndefEvents = 0;
  uint64_t synthesizedZeroEvents = 0;
  uint64_t conditionDiagnosticCount = 0;
  uint64_t undefinedMacroDiagnosticCount = 0;
  uint64_t expansionWarningDiagnosticCount = 0;
  uint64_t inactiveBranchDiagnosticCount = 0;
  uint64_t branchMergeCount = 0;
  uint64_t activeIncludeCount = 0;
};

struct PreprocessorView {
  // Public active-line mask for the selected preprocessor path. Inactive branch
  // lines remain false even when inactive-branch probes collect metadata.
  std::vector<char> lineActive;
  // Per-line branch identity. Active path lines are written by the main
  // interpreter; inactive branch declaration/use lines may be populated from
  // isolated probes so all-branch consumers such as references/rename can group
  // conditional symbol families without treating those lines as active.
  std::vector<PreprocBranchSig> branchSigs;
  // Diagnostics and macro events emitted by the shared preprocessor state
  // machine. Object-like macro chains, including workspace `#art` BOOL/INT
  // default-zero inputs and configured preprocessor macro expressions, are
  // recursively evaluated through the same live state.
  // Undefined numeric macro use emits one diagnostic at the missing leaf's
  // first use in the current live state, then records a synthesized `0`
  // definition so subsequent `defined(...)` and numeric uses observe a
  // consistent state until a real `#define` or `#undef` changes it. Inactive
  // preprocessor branches are probed with isolated macro state: diagnostics are
  // retained with branch metadata, while the public active line mask and
  // post-merge macro state keep following the active branch.
  std::vector<PreprocessorConditionDiagnostic> conditionDiagnostics;
  std::vector<std::string> activeIncludeUris;
  std::unordered_map<std::string, PreprocessorMacroReplacement>
      initialMacroReplacements;
  std::vector<PreprocessorMacroEvent> macroEvents;
  std::vector<PreprocessorBranchMergeInfo> branchMerges;
  // Aggregated health metrics derived from the same macro state machine used
  // by diagnostics, hover, and definition. Audit tooling may aggregate these
  // counts, but feature logic should continue to consume the structured view
  // above rather than reinterpreting report JSON.
  PreprocessorMacroHealthMetrics macroHealth;
};

struct PreprocessorIncludeContext {
  std::string currentUri;
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  // Lowest-priority initial macros collected from workspace summary. `#art`
  // BOOL/INT declarations provide default zero globally; companion enum
  // constants attached to those declarations are consumed only when
  // buildPreprocessorView can prove the owning parameter file is in the current
  // active include closure and the companion value is not conflicting.
  // Configured macros, active-unit compiler private constants, numeric defines,
  // and source #define/#undef remain authoritative when they mention the same
  // name.
  std::vector<ArtDefaultZeroMacro> artDefaultZeroMacros;
  // Optional caller-owned cache scope for active-unit compiler macro analysis
  // inputs. Empty keeps the public interactive/editor paths uncached; audit
  // tooling may provide a stable per-unit scope to avoid rescanning the same
  // include closure for each file in that unit.
  std::string compilerPrivateConstantCacheScope;
  std::function<bool(const std::string &, std::string &)> loadText;
  int maxDepth = 32;
  bool collectIncludeConditionDiagnostics = false;
};

// Updates the process-wide effective `nsf.preprocessorMacros` table consumed
// by preprocessor view construction. The caller owns configuration parsing and
// must refresh document analysis context after changing this map.
void setConfiguredPreprocessorMacros(
    const ConfiguredPreprocessorMacros &macros);

ConfiguredPreprocessorMacros getConfiguredPreprocessorMacros();

std::string getConfiguredPreprocessorMacrosFingerprint();

// Looks up the macro replacement active before `line`.
//
// Inputs: a PreprocessorView produced for the same document/context, a 0-based
// line number, and a macro name.
// Output: true plus replacement metadata when the macro is defined at that
// point; false when it is not defined.
// Non-goals: this API does not expand function-like macro arguments for callers,
// evaluate replacement types, or provide fallback semantic analysis.
bool lookupActivePreprocessorMacroReplacement(
    const PreprocessorView &view, int line, const std::string &name,
    PreprocessorMacroReplacement &replacementOut);

// Evaluates an active object-like macro through the same expression evaluator
// used for #if/#elif. The caller supplies a PreprocessorView built for the same
// document/context and a 0-based line boundary. Returns false when the macro is
// not defined at that point. Function-like macro bodies are not invoked here
// because callers do not provide invocation arguments.
bool evaluateActivePreprocessorMacroInteger(const PreprocessorView &view,
                                            int line,
                                            const std::string &name,
                                            int &valueOut);

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
