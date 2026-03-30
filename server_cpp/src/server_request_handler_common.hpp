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

inline constexpr bool kEnableOverloadResolver = false;
inline constexpr bool kEnableOverloadResolverShadowCompare = false;

void emitSignatureHelpIndeterminateTrace(const std::string &functionName,
                                         const TypeEvalResult &typeEval);

void emitSignatureHelpResolverShadowMismatch(
    const std::string &uri, const std::string &functionName,
    const std::string &resolverLabel, const std::string &legacyLabel,
    const std::string &resolverStatus);

void recordOverloadResolverResult(const ResolveCallResult &result);
