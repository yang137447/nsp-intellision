#pragma once

#include "definition_location.hpp"
#include "hlsl_builtin_docs.hpp"
#include "overload_resolver.hpp"
#include "server_documents.hpp"
#include "type_eval.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct ServerRequestContext;

struct SignatureHelpTargetResult {
  TypeEvalResult typeEval;
  const std::vector<HlslBuiltinSignature> *builtinSigs = nullptr;
  DefinitionLocation definition;
  bool hasDefinition = false;
};

// Consumer-ready function target resolved from the current `.nsf` unit and its
// indexed include closure.
//
// This is narrower than workspace-wide summary lookup: it only serves direct
// call-like requests that should prefer the active/candidate unit context, and
// it intentionally returns false when that unit context is ambiguous.
struct CurrentUnitFunctionTarget {
  DefinitionLocation definition;
  std::string label;
  std::vector<std::string> parameters;
  std::string returnType;
  bool found = false;
};

std::string extractParameterName(const std::string &parameterDecl);

TypeDesc parseParamTypeDescFromDecl(const std::string &parameterDecl);

TypeEvalResult resolveHoverTypeAtDeclaration(
    const std::string &uri, const Document &doc, const std::string &symbol,
    size_t cursorOffset, const ServerRequestContext &ctx, bool &isParamOut);

SignatureHelpTargetResult
resolveSignatureHelpTarget(const std::string &uri,
                           const std::string &functionName,
                           const ServerRequestContext &ctx);

// Resolves a function target from the current `.nsf` unit or, when no active
// unit is set, from the single candidate unit that includes `uri`.
//
// Returns false for ambiguous candidate units and does not scan the whole
// workspace as a replacement for `workspace_summary_runtime.*`.
bool resolveCurrentUnitFunctionTarget(const std::string &uri,
                                      const std::string &functionName,
                                      const ServerRequestContext &ctx,
                                      CurrentUnitFunctionTarget &outTarget);

// Collects distinct function targets from the current `.nsf` unit and its
// indexed include closure so request handlers can surface ambiguity instead of
// silently picking one helper definition.
bool collectCurrentUnitFunctionTargets(
    const std::string &uri, const std::string &functionName,
    const ServerRequestContext &ctx,
    std::vector<CurrentUnitFunctionTarget> &outTargets);

bool resolveFunctionParameters(const std::string &uri,
                               const std::string &functionName,
                               const ServerRequestContext &ctx,
                               std::vector<std::string> &outParams);

bool resolveFunctionParametersFromTarget(
    const std::string &functionName, const SignatureHelpTargetResult &target,
    const ServerRequestContext &ctx, std::string &labelOut,
    std::vector<std::string> &paramsOut);

void inferCallArgumentTypesAtCursor(const std::string &uri,
                                    const std::string &docText,
                                    size_t cursorOffset,
                                    uint64_t epoch,
                                    const std::vector<std::string> &workspaceFolders,
                                    const std::vector<std::string> &includePaths,
                                    const std::vector<std::string> &shaderExtensions,
                                    const std::unordered_map<std::string, int>
                                        &defines,
                                    std::vector<TypeDesc> &outTypes);

bool collectFunctionOverloadCandidates(
    const std::string &uri, const std::string &functionName,
    const SignatureHelpTargetResult &target, const ServerRequestContext &ctx,
    std::vector<CandidateSignature> &outCandidates);
