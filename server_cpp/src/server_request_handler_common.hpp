#pragma once

#include "server_request_handlers.hpp"
#include "overload_resolver.hpp"
#include "type_eval.hpp"

#include <string>
#include <vector>

// Private request-layer helper declarations shared across multiple extracted
// request handler translation units.
//
// Responsibilities:
// - expose local helper functions that remain part of the request-layer
//   implementation, but are shared by more than one request family
//
// Non-goals:
// - not a public feature boundary
// - does not introduce a new semantic truth source

bool pathHasNsfExtension(const std::string &path);

std::vector<std::string> collectIncludeContextUnitPaths(
    const std::string &uri, const ServerRequestContext &ctx);

bool collectIncludeContextDefinitionLocations(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    std::vector<DefinitionLocation> &outLocations);

bool pickBestWorkspaceDefinitionForCurrentContext(
    const std::string &uri, const std::string &word,
    DefinitionLocation &outLocation);

struct ActivePreprocessorMacroResolution {
  bool found = false;
  // Attribution only: true when the request reused the active-unit
  // PreprocessorView already published by global_context_runtime.*.
  bool usedCachedActiveUnitView = false;
  // True only for configured/profile inputs that have no source location.
  bool fromInitialState = false;
  bool fromSynthesizedZero = false;
  bool fromIfndefDefault = false;
  bool fromArtDefaultZero = false;
  bool fromArtCompanionConstant = false;
  bool fromCompilerPrivateConstant = false;
  bool fromCompilerMacroSnapshot = false;
  bool functionLike = false;
  std::string replacement;
  bool hasIntegerValue = false;
  int integerValue = 0;
  DefinitionLocation location;
};

bool resolveActivePreprocessorMacroAtLine(
    const std::string &uri, const std::string &text, int line,
    const std::string &word, ServerRequestContext &ctx,
    ActivePreprocessorMacroResolution &out);

inline constexpr bool kEnableOverloadResolver = false;

void emitSignatureHelpIndeterminateTrace(const std::string &functionName,
                                         const TypeEvalResult &typeEval);

void recordOverloadResolverResult(const ResolveCallResult &result);
