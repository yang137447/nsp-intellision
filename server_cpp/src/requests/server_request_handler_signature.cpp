#include "server_request_handler_signature.hpp"

#include "call_query.hpp"
#include "callsite_parser.hpp"
#include "hover_markdown.hpp"
#include "indeterminate_reasons.hpp"
#include "interactive_semantic_runtime.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "server_request_handler_common.hpp"
#include "text_utils.hpp"

#include <algorithm>
#include <string>
#include <vector>

bool request_signature_handlers::handleSignatureHelpRequest(const std::string &method,
                                       const Json &id, const Json *params,
                                       ServerRequestContext &ctx,
                                       const std::vector<std::string> &,
                                       const std::vector<std::string> &) {
  if (method != "textDocument/signatureHelp" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  if (!textDocument || !position) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *lineValue = getObjectValue(*position, "line");
  const Json *charValue = getObjectValue(*position, "character");
  if (!uriValue || !lineValue || !charValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  int line = static_cast<int>(getNumberValue(*lineValue));
  int character = static_cast<int>(getNumberValue(*charValue));
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }

  size_t cursorOffset = positionToOffsetUtf16(doc->text, line, character);
  std::string functionName;
  int activeParameter = 0;
  if (!parseCallSiteAtOffset(doc->text, cursorOffset, functionName,
                             activeParameter)) {
    writeResponse(id, makeNull());
    return true;
  }

  std::string base;
  std::string member;
  int memberActiveParameter = 0;
  if (parseMemberCallAtOffset(doc->text, cursorOffset, base, member,
                              memberActiveParameter)) {
    bool isParam = false;
    TypeEvalResult baseEval = resolveHoverTypeAtDeclaration(
        uri, *doc, base, cursorOffset, ctx, isParam);
    if (!baseEval.type.empty()) {
      std::vector<HlslBuiltinSignature> methodSigs;
      if (lookupHlslBuiltinMethodSignatures(member, baseEval.type,
                                            methodSigs) &&
          !methodSigs.empty()) {
        Json signatures = makeArray();
        for (const auto &sig : methodSigs) {
          Json parameters = makeArray();
          for (const auto &p : sig.parameters) {
            Json parameter = makeObject();
            parameter.o["label"] = makeString(p);
            parameters.a.push_back(std::move(parameter));
          }
          Json signature = makeObject();
          signature.o["label"] = makeString(sig.label);
          signature.o["parameters"] = parameters;
          if (!sig.documentation.empty())
            signature.o["documentation"] = makeMarkup(sig.documentation);
          signatures.a.push_back(std::move(signature));
        }

        int clampedActive = 0;
        const auto &params = methodSigs.front().parameters;
        if (!params.empty()) {
          clampedActive =
              std::max(0, std::min(memberActiveParameter,
                                   static_cast<int>(params.size()) - 1));
        }

        Json result = makeObject();
        result.o["signatures"] = signatures;
        result.o["activeSignature"] = makeNumber(0);
        result.o["activeParameter"] = makeNumber(clampedActive);
        writeResponse(id, result);
        return true;
      }
    }
  }

  std::vector<SemanticSnapshotFunctionOverloadInfo> interactiveOverloads;
  if (interactiveResolveFunctionOverloads(uri, *doc, functionName, ctx,
                                          interactiveOverloads) &&
      !interactiveOverloads.empty()) {
    Json signatures = makeArray();
    const size_t signatureLimit =
        std::min<size_t>(50, interactiveOverloads.size());
    int activeSignature = 0;
    bool foundMatchingSignature = false;
    for (size_t i = 0; i < signatureLimit; i++) {
      const auto &overload = interactiveOverloads[i];
      Json parameters = makeArray();
      for (const auto &parameterLabel : overload.parameters) {
        Json parameter = makeObject();
        parameter.o["label"] = makeString(parameterLabel);
        parameters.a.push_back(std::move(parameter));
      }
      Json signature = makeObject();
      signature.o["label"] = makeString(overload.label);
      signature.o["parameters"] = parameters;
      signatures.a.push_back(std::move(signature));
      if (!foundMatchingSignature &&
          activeParameter < static_cast<int>(overload.parameters.size())) {
        activeSignature = static_cast<int>(i);
        foundMatchingSignature = true;
      }
    }

    int clampedActive = 0;
    const auto &activeParams =
        interactiveOverloads[static_cast<size_t>(activeSignature)].parameters;
    if (!activeParams.empty()) {
      clampedActive =
          std::max(0, std::min(activeParameter,
                               static_cast<int>(activeParams.size()) - 1));
    }

    Json result = makeObject();
    result.o["signatures"] = signatures;
    result.o["activeSignature"] = makeNumber(activeSignature);
    result.o["activeParameter"] = makeNumber(clampedActive);
    writeResponse(id, result);
    return true;
  }

  SignatureHelpTargetResult target =
      resolveSignatureHelpTarget(uri, functionName, ctx);
  if (target.builtinSigs && !target.builtinSigs->empty()) {
    Json signatures = makeArray();
    const size_t signatureLimit =
        std::min<size_t>(50, target.builtinSigs->size());
    for (size_t i = 0; i < signatureLimit; i++) {
      const auto &sig = (*target.builtinSigs)[i];
      Json parameters = makeArray();
      for (const auto &p : sig.parameters) {
        Json parameter = makeObject();
        parameter.o["label"] = makeString(p);
        parameters.a.push_back(std::move(parameter));
      }
      Json signature = makeObject();
      signature.o["label"] = makeString(sig.label);
      signature.o["parameters"] = parameters;
      if (!sig.documentation.empty())
        signature.o["documentation"] = makeMarkup(sig.documentation);
      signatures.a.push_back(std::move(signature));
    }

    int activeSignature = 0;
    for (size_t i = 0; i < signatureLimit; i++) {
      const auto &sig = (*target.builtinSigs)[i];
      if (activeParameter < static_cast<int>(sig.parameters.size())) {
        activeSignature = static_cast<int>(i);
        break;
      }
    }

    int clampedActive = 0;
    const auto &params = (*target.builtinSigs)[static_cast<size_t>(activeSignature)].parameters;
    if (!params.empty()) {
      clampedActive =
          std::max(0, std::min(activeParameter,
                               static_cast<int>(params.size()) - 1));
    }

    Json result = makeObject();
    result.o["signatures"] = signatures;
    result.o["activeSignature"] = makeNumber(activeSignature);
    result.o["activeParameter"] = makeNumber(clampedActive);
    writeResponse(id, result);
    return true;
  }

  if (!target.hasDefinition) {
    emitSignatureHelpIndeterminateTrace(functionName, target.typeEval);
    writeResponse(id, makeNull());
    return true;
  }
  if (kEnableOverloadResolver) {
    std::vector<CandidateSignature> candidates;
    if (collectFunctionOverloadCandidates(uri, functionName, target, ctx,
                                          candidates)) {
      std::vector<TypeDesc> argumentTypes;
      inferCallArgumentTypesAtCursor(uri, doc->text, cursorOffset,
                                     doc->epoch, ctx.workspaceFolders,
                                     ctx.includePaths, ctx.shaderExtensions,
                                     ctx.preprocessorDefines,
                                     argumentTypes);
      ResolveCallContext resolveContext;
      resolveContext.defines = ctx.preprocessorDefines;
      resolveContext.allowNarrowing = false;
      resolveContext.enableVisibilityFiltering = true;
      resolveContext.allowPartialArity = true;
      ResolveCallResult resolveResult =
          resolveCallCandidates(candidates, argumentTypes, resolveContext);
      recordOverloadResolverResult(resolveResult);
      if (!resolveResult.rankedCandidates.empty() &&
          resolveResult.status != ResolveCallStatus::NoViable) {
        Json signatures = makeArray();
        size_t signatureLimit =
            std::min<size_t>(50, resolveResult.rankedCandidates.size());
        for (size_t i = 0; i < signatureLimit; i++) {
          const CandidateScore &score = resolveResult.rankedCandidates[i];
          if (score.candidateIndex < 0 ||
              static_cast<size_t>(score.candidateIndex) >= candidates.size()) {
            continue;
          }
          const CandidateSignature &candidate =
              candidates[static_cast<size_t>(score.candidateIndex)];
          Json signature = makeObject();
          signature.o["label"] = makeString(candidate.displayLabel.empty()
                                                ? (functionName + "(...)")
                                                : candidate.displayLabel);
          Json parameters = makeArray();
          for (const auto &param : candidate.displayParams) {
            Json parameter = makeObject();
            parameter.o["label"] = makeString(param);
            parameters.a.push_back(std::move(parameter));
          }
          signature.o["parameters"] = parameters;
          signatures.a.push_back(std::move(signature));
        }
        int clampedActive = 0;
        const CandidateScore &bestScore =
            resolveResult.rankedCandidates.front();
        std::string resolverLabel = "";
        if (bestScore.candidateIndex >= 0 &&
            static_cast<size_t>(bestScore.candidateIndex) < candidates.size()) {
          const CandidateSignature &bestCandidate =
              candidates[static_cast<size_t>(bestScore.candidateIndex)];
          resolverLabel = bestCandidate.displayLabel;
          if (!bestCandidate.params.empty()) {
            clampedActive = std::max(
                0, std::min(activeParameter,
                            static_cast<int>(bestCandidate.params.size()) - 1));
          }
        }
        if (kEnableOverloadResolverShadowCompare) {
          std::string legacyLabel;
          std::vector<std::string> legacyParams;
          if (resolveFunctionParametersFromTarget(functionName, target, ctx,
                                                  legacyLabel, legacyParams)) {
            if (!legacyLabel.empty() && !resolverLabel.empty() &&
                legacyLabel != resolverLabel) {
              emitSignatureHelpResolverShadowMismatch(
                  uri, functionName, resolverLabel, legacyLabel,
                  resolveCallStatusToString(resolveResult.status));
            }
          }
        }
        Json result = makeObject();
        result.o["signatures"] = signatures;
        result.o["activeSignature"] = makeNumber(0);
        result.o["activeParameter"] = makeNumber(clampedActive);
        writeResponse(id, result);
        return true;
      }
    }
  }
  std::string label;
  std::vector<std::string> paramsOut;
  if (!resolveFunctionParametersFromTarget(functionName, target, ctx, label,
                                           paramsOut)) {
    TypeEvalResult typeEval;
    typeEval.confidence = TypeEvalConfidence::L3;
    typeEval.reasonCode =
        IndeterminateReason::SignatureHelpSignatureExtractFailed;
    emitSignatureHelpIndeterminateTrace(functionName, typeEval);
    writeResponse(id, makeNull());
    return true;
  }

  int clampedActive = 0;
  if (!paramsOut.empty()) {
    clampedActive = std::max(
        0, std::min(activeParameter, static_cast<int>(paramsOut.size()) - 1));
  }

  Json parameters = makeArray();
  for (const auto &p : paramsOut) {
    Json parameter = makeObject();
    parameter.o["label"] = makeString(p);
    parameters.a.push_back(std::move(parameter));
  }
  Json signature = makeObject();
  signature.o["label"] = makeString(label);
  signature.o["parameters"] = parameters;
  Json signatures = makeArray();
  signatures.a.push_back(std::move(signature));

  Json result = makeObject();
  result.o["signatures"] = signatures;
  result.o["activeSignature"] = makeNumber(0);
  result.o["activeParameter"] = makeNumber(clampedActive);
  writeResponse(id, result);
  return true;
}

