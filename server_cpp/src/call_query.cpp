#include "call_query.hpp"

#include "callsite_parser.hpp"
#include "fast_ast.hpp"
#include "indeterminate_reasons.hpp"
#include "macro_generated_functions.hpp"
#include "nsf_lexer.hpp"
#include "server_request_handlers.hpp"
#include "signature_help.hpp"
#include "text_utils.hpp"
#include "workspace_index.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <utility>

bool findDefinitionInIncludeGraph(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited, DefinitionLocation &outLocation);
bool findDefinitionInIncludeGraphLegacy(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited, DefinitionLocation &outLocation);
bool findTypeOfIdentifierInTextUpTo(const std::string &text,
                                    const std::string &identifier,
                                    size_t maxOffset, std::string &typeNameOut);
bool findParameterTypeInTextUpTo(const std::string &text,
                                 const std::string &identifier,
                                 size_t maxOffset, std::string &typeNameOut);
bool findTypeOfIdentifierInIncludeGraph(
    const std::string &uri, const std::string &identifier,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited, std::string &typeNameOut);

namespace {

bool definitionLocationEqualsLocal(const DefinitionLocation &a,
                                   const DefinitionLocation &b) {
  return a.uri == b.uri && a.line == b.line && a.start == b.start &&
         a.end == b.end;
}

std::string trimCopyCallQuery(const std::string &value) {
  return trimRightCopy(trimLeftCopy(value));
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
      TypeDesc ctorType = parseTypeDesc(tokens[0].text);
      if (ctorType.kind != TypeDescKind::Unknown) {
        return ctorType;
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
    if (findTypeOfIdentifierInTextUpTo(docText, symbol, maxOffset, typeName)) {
      return parseTypeDesc(typeName);
    }
    if (findParameterTypeInTextUpTo(docText, symbol, maxOffset, typeName)) {
      return parseTypeDesc(typeName);
    }
    workspaceIndexGetSymbolType(symbol, typeName);
    if (!typeName.empty()) {
      return parseTypeDesc(typeName);
    }
  }

  return TypeDesc{};
}

} // namespace

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
  TypeEvalResult result;
  isParamOut = false;
  std::string typeName;
  if (queryFastAstLocalType(uri, doc.text, doc.epoch, symbol, cursorOffset,
                            typeName) ||
      findTypeOfIdentifierInTextUpTo(doc.text, symbol, cursorOffset,
                                     typeName)) {
    result.type = typeName;
    result.confidence = TypeEvalConfidence::L2;
    return result;
  }
  if (findParameterTypeInTextUpTo(doc.text, symbol, cursorOffset, typeName)) {
    result.type = typeName;
    result.confidence = TypeEvalConfidence::L2;
    isParamOut = true;
    return result;
  }
  workspaceIndexGetSymbolType(symbol, typeName);
  if (!typeName.empty()) {
    result.type = typeName;
    result.confidence = TypeEvalConfidence::L1;
    return result;
  }
  std::unordered_set<std::string> visited;
  if (findTypeOfIdentifierInIncludeGraph(
          uri, symbol, ctx.documentSnapshot(), ctx.workspaceFolders,
          ctx.includePaths, ctx.shaderExtensions, visited, typeName) &&
      !typeName.empty()) {
    result.type = typeName;
    result.confidence = TypeEvalConfidence::L1;
    return result;
  }
  result.confidence = TypeEvalConfidence::L3;
  result.reasonCode = "unknown_symbol";
  return result;
}

bool signatureHelpTargetEquals(const SignatureHelpTargetResult &a,
                               const SignatureHelpTargetResult &b) {
  if ((a.builtinSigs != nullptr) != (b.builtinSigs != nullptr)) {
    return false;
  }
  if (a.builtinSigs != nullptr && b.builtinSigs != nullptr) {
    const std::string aLabel =
        a.builtinSigs->empty() ? std::string() : a.builtinSigs->front().label;
    const std::string bLabel =
        b.builtinSigs->empty() ? std::string() : b.builtinSigs->front().label;
    if (aLabel != bLabel) {
      return false;
    }
  }
  if (a.hasDefinition != b.hasDefinition) {
    return false;
  }
  if (a.hasDefinition &&
      !definitionLocationEqualsLocal(a.definition, b.definition)) {
    return false;
  }
  return a.typeEval.type == b.typeEval.type &&
         a.typeEval.reasonCode == b.typeEval.reasonCode;
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

  std::unordered_set<std::string> visited;
  if (findDefinitionInIncludeGraph(
          uri, functionName, ctx.documentSnapshot(), ctx.workspaceFolders,
          ctx.includePaths, ctx.shaderExtensions, visited, result.definition)) {
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

SignatureHelpTargetResult
resolveSignatureHelpTargetLegacy(const std::string &uri,
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
  std::unordered_set<std::string> visited;
  if (findDefinitionInIncludeGraphLegacy(
          uri, functionName, ctx.documentSnapshot(), ctx.workspaceFolders,
          ctx.includePaths, ctx.shaderExtensions, visited, result.definition)) {
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
  if (queryFastAstFunctionSignature(
          target.definition.uri, defText, epoch, functionName,
          target.definition.line, target.definition.start, label, outParams)) {
    return true;
  }
  if (extractFunctionSignatureAt(defText, target.definition.line,
                                 target.definition.start, functionName, label,
                                 outParams)) {
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
  const bool fastSig = queryFastAstFunctionSignature(
      target.definition.uri, defText, definitionEpoch, functionName,
      target.definition.line, target.definition.start, labelOut, paramsOut);
  if ((!fastSig && !extractFunctionSignatureAt(
                       defText, target.definition.line, target.definition.start,
                       functionName, labelOut, paramsOut)) ||
      labelOut.empty()) {
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
      outTypes.push_back(
          inferArgumentTypeDescAtCursor(uri, docText, openParen, arg));
      start = i + 1;
    }
  }
  std::string tail = docText.substr(start, cursorOffset - start);
  if (!trimCopyCallQuery(tail).empty()) {
    outTypes.push_back(
        inferArgumentTypeDescAtCursor(uri, docText, openParen, tail));
  }
}

bool collectFunctionOverloadCandidates(
    const std::string &uri, const std::string &functionName,
    const SignatureHelpTargetResult &target, const ServerRequestContext &ctx,
    std::vector<CandidateSignature> &outCandidates) {
  outCandidates.clear();
  std::vector<IndexedDefinition> defs;
  workspaceIndexFindDefinitions(functionName, defs, 128);
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
  for (const auto &d : defs) {
    if (d.kind != 12) {
      continue;
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
    const bool fastSig = queryFastAstFunctionSignature(
        d.uri, defText, definitionEpoch, functionName, d.line, d.start, label,
        params);
    if ((!fastSig &&
         !extractFunctionSignatureAt(defText, d.line, d.start, functionName,
                                     label, params)) ||
        label.empty()) {
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

  if (!target.hasDefinition) {
    return false;
  }
  std::string label;
  std::vector<std::string> params;
  if (!resolveFunctionParametersFromTarget(functionName, target, ctx, label,
                                           params)) {
    return false;
  }
  CandidateSignature fallback;
  fallback.name = functionName;
  fallback.displayLabel = label;
  fallback.displayParams = params;
  fallback.sourceUri = target.definition.uri;
  fallback.sourceLine = target.definition.line;
  fallback.visibilityCondition = "";
  for (const auto &param : params) {
    ParamDesc desc;
    desc.name = extractParameterName(param);
    desc.type = parseParamTypeDescFromDecl(param);
    fallback.params.push_back(std::move(desc));
  }
  outCandidates.push_back(std::move(fallback));
  return true;
}
