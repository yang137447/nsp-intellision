#include "call_query.hpp"

#include "active_unit.hpp"
#include "interactive_semantic_runtime.hpp"
#include "callsite_parser.hpp"
#include "indeterminate_reasons.hpp"
#include "macro_generated_functions.hpp"
#include "nsf_lexer.hpp"
#include "semantic_snapshot.hpp"
#include "server_request_handlers.hpp"
#include "server_parse.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"
#include "workspace_summary_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>
#include <utility>

namespace {

std::string trimCopyCallQuery(const std::string &value) {
  return trimRightCopy(trimLeftCopy(value));
}

bool pathHasNsfExtensionCallQuery(const std::string &path) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return ext == ".nsf";
}

std::string normalizeComparablePathCallQuery(std::string value) {
  value = std::filesystem::path(value).lexically_normal().string();
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   char c = static_cast<char>(std::tolower(ch));
                   return c == '\\' ? '/' : c;
                 });
  while (value.size() > 1 && value.back() == '/')
    value.pop_back();
  return value;
}

std::string resolveCurrentUnitPathForCallQuery(const std::string &uri) {
  const std::string activeUnitPath = getActiveUnitPath();
  const std::string currentPath = uriToPath(uri);
  if (!currentPath.empty() && pathHasNsfExtensionCallQuery(currentPath))
    return currentPath;
  if (!activeUnitPath.empty())
    return activeUnitPath;
  if (currentPath.empty())
    return std::string();

  std::vector<std::string> candidateUnits;
  workspaceSummaryRuntimeCollectIncludingUnits({uri}, candidateUnits, 2);
  if (candidateUnits.size() == 1)
    return candidateUnits.front();
  return std::string();
}

void collectCurrentUnitClosureUrisForCallQuery(
    const std::string &uri, std::vector<std::string> &outUris) {
  outUris.clear();
  const std::string currentUnitPath = resolveCurrentUnitPathForCallQuery(uri);
  if (currentUnitPath.empty())
    return;

  std::unordered_set<std::string> seen;
  auto appendPath = [&](const std::string &path) {
    if (path.empty())
      return;
    const std::string normalizedPath =
        std::filesystem::path(path).lexically_normal().string();
    const std::string key = normalizeComparablePathCallQuery(normalizedPath);
    if (key.empty() || !seen.insert(key).second)
      return;
    outUris.push_back(pathToUri(normalizedPath));
  };

  appendPath(currentUnitPath);
  std::vector<std::string> includeClosurePaths;
  workspaceSummaryRuntimeCollectIncludeClosureForUnit(currentUnitPath,
                                                      includeClosurePaths, 1024);
  for (const auto &path : includeClosurePaths)
    appendPath(path);
}

std::string extractReturnTypeFromLabelCallQuery(const std::string &label,
                                                const std::string &name) {
  size_t namePos = label.rfind(name);
  if (namePos == std::string::npos)
    return std::string();
  return trimRightCopy(label.substr(0, namePos));
}

bool inferBuiltinFunctionReturnType(const std::string &functionName,
                                    std::string &returnTypeOut) {
  returnTypeOut.clear();
  const auto *signatures = lookupHlslBuiltinSignatures(functionName);
  if (!signatures || signatures->empty())
    return false;

  std::string canonical;
  for (const auto &signature : *signatures) {
    std::string returnType =
        extractReturnTypeFromLabelCallQuery(signature.label, functionName);
    if (returnType.empty())
      return false;
    const std::string nextCanonical =
        typeDescToCanonicalString(parseTypeDesc(returnType));
    if (nextCanonical.empty())
      return false;
    if (canonical.empty()) {
      canonical = nextCanonical;
      returnTypeOut = returnType;
      continue;
    }
    if (canonical != nextCanonical)
      return false;
  }
  return !returnTypeOut.empty();
}

bool inferSemanticSnapshotFunctionReturnType(
    const std::string &uri, const std::string &docText, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &functionName, std::string &returnTypeOut) {
  returnTypeOut.clear();
  std::vector<SemanticSnapshotFunctionOverloadInfo> overloads;
  if (!querySemanticSnapshotFunctionOverloads(
          uri, docText, epoch, workspaceFolders, includePaths, shaderExtensions,
          defines, functionName, overloads) ||
      overloads.empty()) {
    return false;
  }

  std::string canonical;
  for (const auto &overload : overloads) {
    if (overload.returnType.empty())
      return false;
    const std::string nextCanonical =
        typeDescToCanonicalString(parseTypeDesc(overload.returnType));
    if (nextCanonical.empty())
      return false;
    if (canonical.empty()) {
      canonical = nextCanonical;
      returnTypeOut = overload.returnType;
      continue;
    }
    if (canonical != nextCanonical)
      return false;
  }
  return !returnTypeOut.empty();
}

bool queryFunctionSignatureWithFallback(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &functionName, int lineIndex, int nameCharacter,
    const ServerRequestContext *ctx, std::string &labelOut,
    std::vector<std::string> &paramsOut) {
  if (ctx) {
    if (const Document *doc = ctx->findDocument(uri)) {
      if (interactiveResolveFunctionSignature(uri, *doc, functionName,
                                              lineIndex, nameCharacter, *ctx,
                                              labelOut, paramsOut)) {
        return true;
      }
    }
  }
  return querySemanticSnapshotFunctionSignature(
      uri, text, epoch, workspaceFolders, includePaths, shaderExtensions,
      defines, functionName, lineIndex, nameCharacter, labelOut, paramsOut);
}

static void appendOverloadCandidates(
    const std::string &candidateUri, const std::string &functionName,
    const std::vector<SemanticSnapshotFunctionOverloadInfo> &overloads,
    std::unordered_set<std::string> &seen,
    std::vector<CandidateSignature> &outCandidates) {
  for (const auto &overload : overloads) {
    std::string dedupKey = candidateUri + "|" + std::to_string(overload.line) +
                           "|" + std::to_string(overload.character);
    if (!seen.insert(dedupKey).second)
      continue;
    CandidateSignature candidate;
    candidate.name = functionName;
    candidate.displayLabel = overload.label;
    candidate.displayParams = overload.parameters;
    candidate.returnType = parseTypeDesc(overload.returnType);
    candidate.sourceUri = candidateUri;
    candidate.sourceLine = overload.line;
    candidate.visibilityCondition = "";
    candidate.params.reserve(overload.parameters.size());
    for (const auto &param : overload.parameters) {
      ParamDesc desc;
      desc.name = extractParameterName(param);
      desc.type = parseParamTypeDescFromDecl(param);
      candidate.params.push_back(std::move(desc));
    }
    outCandidates.push_back(std::move(candidate));
  }
}

static void appendSemanticSnapshotCandidatesForUri(
    const std::string &candidateUri, const std::string &functionName,
    const ServerRequestContext &ctx, std::unordered_set<std::string> &seen,
    std::vector<CandidateSignature> &outCandidates) {
  std::vector<SemanticSnapshotFunctionOverloadInfo> overloads;
  if (const Document *definitionDoc = ctx.findDocument(candidateUri)) {
    interactiveResolveFunctionOverloads(candidateUri, *definitionDoc,
                                        functionName, ctx, overloads);
  }
  if (overloads.empty()) {
    std::string defText;
    if (!ctx.readDocumentText(candidateUri, defText))
      return;
    uint64_t definitionEpoch = 0;
    if (const Document *definitionDoc = ctx.findDocument(candidateUri))
      definitionEpoch = definitionDoc->epoch;

    if (!querySemanticSnapshotFunctionOverloads(
            candidateUri, defText, definitionEpoch, ctx.workspaceFolders,
            ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
            functionName, overloads)) {
      return;
    }
  }

  appendOverloadCandidates(candidateUri, functionName, overloads, seen,
                           outCandidates);
}

bool resolveMacroGeneratedFunctionAtUri(
    const std::string &uri, const std::string &functionName,
    const ServerRequestContext &ctx, MacroGeneratedFunctionInfo &out) {
  std::string docText;
  if (!ctx.readDocumentText(uri, docText)) {
    return false;
  }
  std::vector<MacroGeneratedFunctionInfo> candidates;
  if (!collectMacroGeneratedFunctions(uri, docText, ctx.workspaceFolders,
                                      ctx.includePaths, ctx.shaderExtensions,
                                      functionName, candidates, 1) ||
      candidates.empty()) {
    return false;
  }
  out = std::move(candidates.front());
  return true;
}

bool locateCallOpenParenAtCursor(const std::string &text, size_t cursorOffset,
                                 size_t &openParenOut) {
  std::string functionName;
  int activeParameter = 0;
  return parseCallSiteAtOffset(text, cursorOffset, functionName,
                               activeParameter, &openParenOut);
}

TypeDesc inferArgumentTypeDescAtCursor(const std::string &uri,
                                       const std::string &docText,
                                       size_t maxOffset,
                                       uint64_t epoch,
                                       const std::vector<std::string> &workspaceFolders,
                                       const std::vector<std::string> &includePaths,
                                       const std::vector<std::string> &shaderExtensions,
                                       const std::unordered_map<std::string, int>
                                           &defines,
                                       const std::string &expression) {
  std::string value = trimCopyCallQuery(expression);
  if (value.empty()) {
    return TypeDesc{};
  }
  std::string lower = value;
  for (char &ch : lower) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  if (lower == "true" || lower == "false") {
    return parseTypeDesc("bool");
  }

  auto tokens = lexLineTokens(value);
  if (!tokens.empty() && tokens[0].kind == LexToken::Kind::Identifier) {
    if (tokens.size() >= 2 && tokens[1].kind == LexToken::Kind::Punct &&
        tokens[1].text == "(") {
      const std::string &calleeName = tokens[0].text;
      TypeDesc ctorType = parseTypeDesc(calleeName);
      if (ctorType.kind != TypeDescKind::Unknown) {
        return ctorType;
      }
      std::string returnType;
      if (inferBuiltinFunctionReturnType(calleeName, returnType) ||
          inferSemanticSnapshotFunctionReturnType(
              uri, docText, epoch, workspaceFolders, includePaths,
              shaderExtensions, defines, calleeName, returnType)) {
        return parseTypeDesc(returnType);
      }
    }
  }

  bool isNumericPrefix =
      !value.empty() &&
      (std::isdigit(static_cast<unsigned char>(value[0])) ||
       ((value[0] == '+' || value[0] == '-') && value.size() > 1 &&
        std::isdigit(static_cast<unsigned char>(value[1]))));
  if (isNumericPrefix) {
    if (lower.find('.') != std::string::npos ||
        lower.find('e') != std::string::npos) {
      if (!lower.empty() && lower.back() == 'h') {
        return parseTypeDesc("half");
      }
      return parseTypeDesc("float");
    }
    if (!lower.empty() && lower.back() == 'u') {
      return parseTypeDesc("uint");
    }
    return parseTypeDesc("int");
  }

  if (tokens.size() == 1 && tokens[0].kind == LexToken::Kind::Identifier) {
    std::string symbol = tokens[0].text;
    std::string typeName;
    if (querySemanticSnapshotLocalTypeAtOffset(
            uri, docText, epoch, workspaceFolders, includePaths,
            shaderExtensions, defines, symbol, maxOffset, typeName)) {
      return parseTypeDesc(typeName);
    }
    if (querySemanticSnapshotParameterTypeAtOffset(
            uri, docText, epoch, workspaceFolders, includePaths,
            shaderExtensions, defines, symbol, maxOffset, typeName)) {
      return parseTypeDesc(typeName);
    }
    if (querySemanticSnapshotGlobalType(uri, docText, epoch, workspaceFolders,
                                        includePaths, shaderExtensions, defines,
                                        symbol, typeName)) {
      return parseTypeDesc(typeName);
    }
    workspaceSummaryRuntimeGetSymbolType(symbol, typeName);
    if (!typeName.empty()) {
      return parseTypeDesc(typeName);
    }
  }

  return TypeDesc{};
}

} // namespace

bool resolveCurrentUnitFunctionTarget(const std::string &uri,
                                      const std::string &functionName,
                                      const ServerRequestContext &ctx,
                                      CurrentUnitFunctionTarget &outTarget) {
  outTarget = CurrentUnitFunctionTarget{};
  std::vector<CurrentUnitFunctionTarget> targets;
  if (!collectCurrentUnitFunctionTargets(uri, functionName, ctx, targets) ||
      targets.size() != 1) {
    return false;
  }
  outTarget = targets.front();
  return outTarget.found;
}

bool collectCurrentUnitFunctionTargets(
    const std::string &uri, const std::string &functionName,
    const ServerRequestContext &ctx,
    std::vector<CurrentUnitFunctionTarget> &outTargets) {
  outTargets.clear();
  if (functionName.empty())
    return false;

  std::vector<std::string> candidateUris;
  collectCurrentUnitClosureUrisForCallQuery(uri, candidateUris);
  if (candidateUris.empty())
    return false;

  std::unordered_set<std::string> seenTargets;
  for (const auto &candidateUri : candidateUris) {
    std::string candidateText;
    if (!ctx.readDocumentText(candidateUri, candidateText))
      continue;

    if (normalizeComparablePathCallQuery(uriToPath(candidateUri)) ==
        normalizeComparablePathCallQuery(uriToPath(uri))) {
      uint64_t candidateEpoch = 0;
      if (const Document *candidateDoc = ctx.findDocument(candidateUri))
        candidateEpoch = candidateDoc->epoch;

      std::vector<SemanticSnapshotFunctionOverloadInfo> overloads;
      if (querySemanticSnapshotFunctionOverloads(
              candidateUri, candidateText, candidateEpoch,
              ctx.workspaceFolders, ctx.includePaths, ctx.shaderExtensions,
              ctx.preprocessorDefines, functionName, overloads)) {
        for (const auto &overload : overloads) {
          CurrentUnitFunctionTarget target;
          target.definition.uri = candidateUri;
          target.definition.line = overload.line;
          target.definition.start = overload.character;
          target.definition.end =
              overload.character + static_cast<int>(functionName.size());
          target.label = overload.label.empty() ? (functionName + "(...)")
                                                : overload.label;
          target.parameters = overload.parameters;
          target.returnType =
              extractReturnTypeFromLabelCallQuery(target.label, functionName);
          target.found = true;

          const std::string key = target.definition.uri + "|" +
                                  std::to_string(target.definition.line) + "|" +
                                  std::to_string(target.definition.start) + "|" +
                                  std::to_string(target.definition.end);
          if (seenTargets.insert(key).second)
            outTargets.push_back(std::move(target));
        }
        if (!outTargets.empty())
          continue;
      }
    }

    size_t searchFrom = 0;
    while (searchFrom < candidateText.size()) {
      const size_t found = candidateText.find(functionName, searchFrom);
      if (found == std::string::npos)
        break;
      searchFrom = found + functionName.size();

      size_t currentLineStart = found;
      while (currentLineStart > 0 && candidateText[currentLineStart - 1] != '\n')
        currentLineStart--;
      int lineIndex = 0;
      for (size_t i = 0; i < currentLineStart; i++) {
        if (candidateText[i] == '\n')
          lineIndex++;
      }
      const std::string lineText = getLineAt(candidateText, lineIndex);
      const int nameCharacter = byteOffsetInLineToUtf16(
          lineText, static_cast<int>(found - currentLineStart));

      ParsedFunctionSignatureTextInfo signatureInfo;
      if (!extractFunctionSignatureFromTextShared(candidateText, lineIndex,
                                                  nameCharacter, functionName,
                                                  signatureInfo) ||
          !signatureInfo.hasBody) {
        continue;
      }

      CurrentUnitFunctionTarget target;
      target.definition.uri = candidateUri;
      target.definition.line = lineIndex;
      target.definition.start = nameCharacter;
      target.definition.end = nameCharacter + static_cast<int>(functionName.size());
      target.label =
          signatureInfo.label.empty() ? (functionName + "(...)") : signatureInfo.label;
      target.parameters = signatureInfo.parameters;
      target.returnType =
          extractReturnTypeFromLabelCallQuery(target.label, functionName);
      target.found = true;

      const std::string key = target.definition.uri + "|" +
                              std::to_string(target.definition.line) + "|" +
                              std::to_string(target.definition.start) + "|" +
                              std::to_string(target.definition.end);
      if (seenTargets.insert(key).second)
        outTargets.push_back(std::move(target));
      break;
    }
  }

  std::sort(outTargets.begin(), outTargets.end(),
            [](const auto &lhs, const auto &rhs) {
              if (lhs.definition.uri != rhs.definition.uri)
                return lhs.definition.uri < rhs.definition.uri;
              if (lhs.definition.line != rhs.definition.line)
                return lhs.definition.line < rhs.definition.line;
              return lhs.definition.start < rhs.definition.start;
            });
  return !outTargets.empty();
}

std::string extractParameterName(const std::string &parameterDecl) {
  std::string value = trimCopyCallQuery(parameterDecl);
  if (value.empty() || value == "void") {
    return "";
  }
  size_t eq = value.find('=');
  if (eq != std::string::npos) {
    value = trimCopyCallQuery(value.substr(0, eq));
  }
  size_t colon = value.find(':');
  if (colon != std::string::npos) {
    value = trimCopyCallQuery(value.substr(0, colon));
  }
  if (value.empty()) {
    return "";
  }

  int bracketDepth = 0;
  for (size_t i = value.size(); i > 0; i--) {
    char ch = value[i - 1];
    if (ch == ']') {
      bracketDepth++;
      continue;
    }
    if (ch == '[') {
      if (bracketDepth > 0) {
        bracketDepth--;
      }
      continue;
    }
    if (bracketDepth > 0) {
      continue;
    }
    if (isIdentifierChar(ch)) {
      size_t end = i;
      size_t start = i - 1;
      while (start > 0 && isIdentifierChar(value[start - 1])) {
        start--;
      }
      return value.substr(start, end - start);
    }
  }
  return "";
}

TypeDesc parseParamTypeDescFromDecl(const std::string &parameterDecl) {
  auto tokens = lexLineTokens(parameterDecl);
  for (const auto &token : tokens) {
    if (token.kind != LexToken::Kind::Identifier) {
      continue;
    }
    if (isQualifierToken(token.text)) {
      continue;
    }
    if (token.text == "in" || token.text == "out" || token.text == "inout") {
      continue;
    }
    return parseTypeDesc(token.text);
  }
  return TypeDesc{};
}

TypeEvalResult resolveHoverTypeAtDeclaration(
    const std::string &uri, const Document &doc, const std::string &symbol,
    size_t cursorOffset, const ServerRequestContext &ctx, bool &isParamOut) {
  return interactiveResolveHoverTypeAtDeclaration(uri, doc, symbol, cursorOffset,
                                                  ctx, isParamOut);
}

SignatureHelpTargetResult
resolveSignatureHelpTarget(const std::string &uri,
                           const std::string &functionName,
                           const ServerRequestContext &ctx) {
  SignatureHelpTargetResult result;
  HlslBuiltinRuleView builtinRule;
  const bool isBuiltin = queryHlslBuiltinRuleView(functionName, builtinRule);
  const std::vector<HlslBuiltinSignature> *builtinSigs =
      lookupHlslBuiltinSignatures(functionName);
  if (builtinSigs && !builtinSigs->empty()) {
    result.builtinSigs = builtinSigs;
    result.typeEval.type = "builtin_function";
    result.typeEval.confidence = TypeEvalConfidence::L1;
    return result;
  }
  if (isBuiltin) {
    result.typeEval.type = "builtin_function";
    result.typeEval.confidence = TypeEvalConfidence::L3;
    result.typeEval.reasonCode =
        isHlslBuiltinRegistryAvailable()
            ? IndeterminateReason::SignatureHelpBuiltinUnmodeled
            : IndeterminateReason::SignatureHelpBuiltinRegistryUnavailable;
    return result;
  }

  CurrentUnitFunctionTarget currentUnitTarget;
  if (resolveCurrentUnitFunctionTarget(uri, functionName, ctx,
                                       currentUnitTarget)) {
    result.definition = currentUnitTarget.definition;
    result.hasDefinition = true;
    result.typeEval.type = "user_function";
    result.typeEval.confidence = TypeEvalConfidence::L1;
    return result;
  }

  if (workspaceSummaryRuntimeFindDefinition(functionName, result.definition)) {
    result.hasDefinition = true;
    result.typeEval.type = "user_function";
    result.typeEval.confidence = TypeEvalConfidence::L1;
    return result;
  }

  MacroGeneratedFunctionInfo macroCandidate;
  if (resolveMacroGeneratedFunctionAtUri(uri, functionName, ctx,
                                         macroCandidate)) {
    result.definition = macroCandidate.definition;
    result.hasDefinition = true;
    result.typeEval.type = "user_function";
    result.typeEval.confidence = TypeEvalConfidence::L1;
    return result;
  }

  result.typeEval.confidence = TypeEvalConfidence::L3;
  result.typeEval.reasonCode =
      isHlslBuiltinRegistryAvailable()
          ? IndeterminateReason::SignatureHelpCallTargetUnknown
          : IndeterminateReason::SignatureHelpBuiltinRegistryUnavailable;
  return result;
}

bool resolveFunctionParameters(const std::string &uri,
                               const std::string &functionName,
                               const ServerRequestContext &ctx,
                               std::vector<std::string> &outParams) {
  outParams.clear();
  if (const Document *doc = ctx.findDocument(uri)) {
    std::vector<SemanticSnapshotFunctionOverloadInfo> overloads;
    if (interactiveResolveFunctionOverloads(uri, *doc, functionName, ctx,
                                            overloads) &&
        !overloads.empty()) {
      outParams = overloads.front().parameters;
      return true;
    }
  }
  SignatureHelpTargetResult target =
      resolveSignatureHelpTarget(uri, functionName, ctx);
  if (target.builtinSigs && !target.builtinSigs->empty()) {
    outParams = target.builtinSigs->front().parameters;
    return true;
  }
  if (!target.hasDefinition) {
    return false;
  }

  std::string defText;
  if (!ctx.readDocumentText(target.definition.uri, defText)) {
    return false;
  }

  std::string label;
  uint64_t epoch = 0;
  const Document *targetDoc = ctx.findDocument(target.definition.uri);
  if (targetDoc) {
    epoch = targetDoc->epoch;
  }
  if (queryFunctionSignatureWithFallback(
          target.definition.uri, defText, epoch, ctx.workspaceFolders,
          ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
          functionName, target.definition.line, target.definition.start, &ctx,
          label, outParams)) {
    return true;
  }
  MacroGeneratedFunctionInfo macroCandidate;
  if (resolveMacroGeneratedFunctionAtUri(uri, functionName, ctx,
                                         macroCandidate)) {
    outParams = macroCandidate.parameterDecls;
    return true;
  }
  return false;
}

bool resolveFunctionParametersFromTarget(
    const std::string &functionName, const SignatureHelpTargetResult &target,
    const ServerRequestContext &ctx, std::string &labelOut,
    std::vector<std::string> &paramsOut) {
  labelOut.clear();
  paramsOut.clear();
  if (target.builtinSigs && !target.builtinSigs->empty()) {
    labelOut = target.builtinSigs->front().label;
    paramsOut = target.builtinSigs->front().parameters;
    return true;
  }
  if (!target.hasDefinition) {
    return false;
  }
  std::string defText;
  if (!ctx.readDocumentText(target.definition.uri, defText)) {
    return false;
  }
  uint64_t definitionEpoch = 0;
  const Document *definitionDoc = ctx.findDocument(target.definition.uri);
  if (definitionDoc) {
    definitionEpoch = definitionDoc->epoch;
  }
  const bool fastSig = queryFunctionSignatureWithFallback(
      target.definition.uri, defText, definitionEpoch, ctx.workspaceFolders,
      ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
      functionName, target.definition.line, target.definition.start, &ctx,
      labelOut, paramsOut);
  if (!fastSig || labelOut.empty()) {
    MacroGeneratedFunctionInfo macroCandidate;
    if (resolveMacroGeneratedFunctionAtUri(target.definition.uri, functionName,
                                           ctx, macroCandidate)) {
      labelOut = macroCandidate.label;
      paramsOut = macroCandidate.parameterDecls;
      return true;
    }
    labelOut.clear();
    paramsOut.clear();
    return false;
  }
  return true;
}

void inferCallArgumentTypesAtCursor(const std::string &uri,
                                    const std::string &docText,
                                    size_t cursorOffset,
                                    uint64_t epoch,
                                    const std::vector<std::string> &workspaceFolders,
                                    const std::vector<std::string> &includePaths,
                                    const std::vector<std::string> &shaderExtensions,
                                    const std::unordered_map<std::string, int>
                                        &defines,
                                    std::vector<TypeDesc> &outTypes) {
  outTypes.clear();
  size_t openParen = 0;
  if (!locateCallOpenParenAtCursor(docText, cursorOffset, openParen)) {
    return;
  }
  size_t start = openParen + 1;
  int parenDepth = 0;
  int angleDepth = 0;
  int bracketDepth = 0;
  for (size_t i = start; i < cursorOffset; i++) {
    char ch = docText[i];
    if (ch == '(') {
      parenDepth++;
    } else if (ch == ')' && parenDepth > 0) {
      parenDepth--;
    } else if (ch == '<') {
      angleDepth++;
    } else if (ch == '>' && angleDepth > 0) {
      angleDepth--;
    } else if (ch == '[') {
      bracketDepth++;
    } else if (ch == ']' && bracketDepth > 0) {
      bracketDepth--;
    }
    if (ch == ',' && parenDepth == 0 && angleDepth == 0 &&
        bracketDepth == 0) {
      std::string arg = docText.substr(start, i - start);
      outTypes.push_back(inferArgumentTypeDescAtCursor(
          uri, docText, openParen, epoch, workspaceFolders, includePaths,
          shaderExtensions, defines, arg));
      start = i + 1;
    }
  }
  std::string tail = docText.substr(start, cursorOffset - start);
  if (!trimCopyCallQuery(tail).empty()) {
    outTypes.push_back(inferArgumentTypeDescAtCursor(
        uri, docText, openParen, epoch, workspaceFolders, includePaths,
        shaderExtensions, defines, tail));
  }
}

bool collectFunctionOverloadCandidates(
    const std::string &uri, const std::string &functionName,
    const SignatureHelpTargetResult &target, const ServerRequestContext &ctx,
    std::vector<CandidateSignature> &outCandidates) {
  outCandidates.clear();
  std::vector<IndexedDefinition> defs;
  workspaceSummaryRuntimeFindDefinitions(functionName, defs, 128);
  if (target.hasDefinition) {
    IndexedDefinition preferred;
    preferred.uri = target.definition.uri;
    preferred.line = target.definition.line;
    preferred.start = target.definition.start;
    preferred.end = target.definition.end;
    preferred.kind = 12;
    defs.insert(defs.begin(), preferred);
  }

  std::unordered_set<std::string> seen;
  std::unordered_set<std::string> seenUris;
  if (target.hasDefinition && seenUris.insert(target.definition.uri).second) {
    appendSemanticSnapshotCandidatesForUri(target.definition.uri, functionName,
                                           ctx, seen, outCandidates);
  }
  for (const auto &d : defs) {
    if (d.kind != 12) {
      continue;
    }
    if (seenUris.insert(d.uri).second) {
      appendSemanticSnapshotCandidatesForUri(d.uri, functionName, ctx, seen,
                                             outCandidates);
    }
    std::string dedupKey =
        d.uri + "|" + std::to_string(d.line) + "|" + std::to_string(d.start);
    if (!seen.insert(dedupKey).second) {
      continue;
    }
    std::string defText;
    if (!ctx.readDocumentText(d.uri, defText)) {
      continue;
    }
    std::string label;
    std::vector<std::string> params;
    uint64_t definitionEpoch = 0;
    const Document *definitionDoc = ctx.findDocument(d.uri);
    if (definitionDoc) {
      definitionEpoch = definitionDoc->epoch;
    }
    const bool fastSig = queryFunctionSignatureWithFallback(
        d.uri, defText, definitionEpoch, ctx.workspaceFolders,
        ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
        functionName, d.line, d.start, &ctx, label, params);
    if (!fastSig || label.empty()) {
      continue;
    }
    CandidateSignature candidate;
    candidate.name = functionName;
    candidate.displayLabel = label;
    candidate.displayParams = params;
    candidate.returnType = parseTypeDesc(d.type);
    candidate.sourceUri = d.uri;
    candidate.sourceLine = d.line;
    candidate.visibilityCondition = "";
    candidate.params.reserve(params.size());
    for (const auto &param : params) {
      ParamDesc desc;
      desc.name = extractParameterName(param);
      desc.type = parseParamTypeDescFromDecl(param);
      candidate.params.push_back(std::move(desc));
    }
    outCandidates.push_back(std::move(candidate));
  }
  {
    MacroGeneratedFunctionInfo macroCandidate;
    if (resolveMacroGeneratedFunctionAtUri(uri, functionName, ctx,
                                           macroCandidate)) {
      CandidateSignature candidate;
      candidate.name = functionName;
      candidate.displayLabel = macroCandidate.label;
      candidate.displayParams = macroCandidate.parameterDecls;
      candidate.returnType = parseTypeDesc(macroCandidate.returnType);
      candidate.sourceUri = macroCandidate.definition.uri;
      candidate.sourceLine = macroCandidate.definition.line;
      candidate.visibilityCondition = "";
      for (const auto &param : macroCandidate.parameterDecls) {
        ParamDesc desc;
        desc.name = extractParameterName(param);
        desc.type = parseParamTypeDescFromDecl(param);
        candidate.params.push_back(std::move(desc));
      }
      outCandidates.push_back(std::move(candidate));
    }
  }
  if (!outCandidates.empty()) {
    return true;
  }

  return false;
}
