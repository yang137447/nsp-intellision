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

std::string extractParameterName(const std::string &parameterDecl);

TypeDesc parseParamTypeDescFromDecl(const std::string &parameterDecl);

TypeEvalResult resolveHoverTypeAtDeclaration(
    const std::string &uri, const Document &doc, const std::string &symbol,
    size_t cursorOffset, const ServerRequestContext &ctx, bool &isParamOut);

bool signatureHelpTargetEquals(const SignatureHelpTargetResult &a,
                               const SignatureHelpTargetResult &b);

SignatureHelpTargetResult
resolveSignatureHelpTarget(const std::string &uri,
                           const std::string &functionName,
                           const ServerRequestContext &ctx);

SignatureHelpTargetResult
resolveSignatureHelpTargetLegacy(const std::string &uri,
                                 const std::string &functionName,
                                 const ServerRequestContext &ctx);

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
