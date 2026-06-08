#include "diagnostics_semantic.hpp"
#include "diagnostics_expression_type.hpp"
#include "diagnostics_semantic_common.hpp"
#include "diagnostics_symbol_type.hpp"

#include "callsite_parser.hpp"
#include "diagnostics_emit.hpp"
#include "diagnostics_indeterminate.hpp"
#include "diagnostics_io.hpp"
#include "diagnostics_preprocessor.hpp"
#include "diagnostics_syntax.hpp"
#include "fast_ast.hpp"
#include "full_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_markdown.hpp"
#include "include_resolver.hpp"
#include "indeterminate_reasons.hpp"
#include "language_registry.hpp"
#include "lsp_helpers.hpp"
#include "macro_generated_functions.hpp"
#include "macro_statement_locals.hpp"
#include "nsf_lexer.hpp"
#include "overload_resolver.hpp"
#include "preprocessor_view.hpp"
#include "semantic_cache.hpp"
#include "semantic_snapshot.hpp"
#include "server_parse.hpp"
#include "text_utils.hpp"
#include "type_desc.hpp"
#include "type_eval.hpp"
#include "type_model.hpp"
#include "type_relation.hpp"
#include "uri_utils.hpp"
#include "workspace_summary_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>


void collectReturnAndTypeDiagnostics(
    const std::string &uri, const std::string &text,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const PreprocessorView &preprocessorView,
    const DiagnosticsPrerequisiteState &basePrerequisites, Json &diags,
    int timeBudgetMs, size_t maxDiagnostics, bool &timedOut,
    bool indeterminateEnabled, int indeterminateSeverity,
    size_t indeterminateMaxItems, size_t &indeterminateCount,
    DiagnosticsPrerequisiteSkipStats &prerequisiteSkips,
    bool typeConversionRiskWarningsEnabled) {
  const auto diagnosticsStart = std::chrono::steady_clock::now();
  const auto diagnosticsBudget =
      std::chrono::milliseconds(std::max(30, timeBudgetMs));
  std::istringstream stream(text);
  const TrimmedCodeLineScanSharedResult lineScan =
      buildTrimmedCodeLineScanShared(text, &preprocessorView.lineActive);
  const std::vector<std::string> &trimmedCodeLines = lineScan.trimmedLines;
  std::string lineText;
  int lineIndex = 0;
  bool inBlockComment = false;
  bool inString = false;

  bool inFunction = false;
  std::string pendingReturnType;
  std::string pendingFunctionName;
  int pendingFunctionLine = -1;
  int pendingFunctionStart = -1;
  bool pendingSignature = false;
  std::unordered_map<std::string, std::string> pendingParams;
  std::vector<std::string> pendingParamTypesOrdered;

  std::string functionReturnType;
  std::string functionName;
  int functionNameLine = -1;
  int functionNameStart = -1;
  int functionNameEnd = -1;
  int functionBraceDepth = 0;
  bool sawReturn = false;
  bool sawTopLevelReturn = false;
  bool sawPotentialMissingReturn = false;
  bool sawPotentialUnreachable = false;
  enum class FlowBlockKind { Function, Branch, LoopOrSwitch, Other };
  struct LocalDeclEntry {
    std::string type;
    PreprocBranchSig sig;
  };
  struct LocalScopeFrame {
    int id = 0;
    int parentId = -1;
    int braceDepth = 0;
    bool forScope = false;
    int closeAfterReturnToDepth = -1;
    std::unordered_map<std::string, std::vector<LocalDeclEntry>> localsByName;
  };
  struct PendingForScope {
    int scopeId = -1;
    int parentDepth = 0;
    int createdLine = -1;
  };
  std::vector<LocalScopeFrame> localScopes;
  std::vector<int> localScopeStack;
  std::vector<PendingForScope> pendingForScopes;
  int nextLocalScopeId = 0;
  std::unordered_map<std::string, std::string> localsVisibleTypes;
  std::unordered_set<std::string> localsVisibleNames;
  std::string pendingMultilineLocalName;
  int pendingMultilineLocalDepth = -1;
  int pendingMultilineLocalLine = -1;
  int pendingMultilineLocalStart = -1;
  int pendingMultilineLocalEnd = -1;
  std::unordered_set<std::string> paramNames;
  std::unordered_set<std::string> globalSymbols;
  std::unordered_set<std::string> globalFunctionSignatures;
  std::unordered_map<std::string, bool> resolvedSymbolCache;
  std::unordered_map<int, int> pendingUnbracedBranchLineByDepth;
  std::unordered_map<int, bool> pendingUnbracedBranchIsElseByDepth;
  bool conditionalReturnSeen = false;
  bool unconditionalReturnSeen = false;
  bool inUiMetaBlock = false;
  std::string pendingUiVarName;
  int pendingUiVarLine = -1;
  int pendingUiVarStart = -1;
  int pendingUiVarEnd = -1;
  int typeBlockBraceDepth = 0;
  bool pendingTypeBlockOpen = false;
  auto emitIndeterminate =
      [&](int line, int startByte, int endByte, const std::string &code,
          const std::string &reasonCode, const std::string &message) {
        if (!indeterminateEnabled)
          return;
        if (indeterminateCount >= indeterminateMaxItems)
          return;
        if (diags.a.size() >= maxDiagnostics)
          return;
        diags.a.push_back(makeDiagnosticWithCodeAndReason(
            text, line, startByte, endByte, indeterminateSeverity, "nsf",
            message, code, reasonCode));
        indeterminateCount++;
      };
  auto parserRegionReliableForLine = [&](int line) {
    if (line < 0)
      return true;
    const size_t index = static_cast<size_t>(line);
    const bool insideOpenGroupingBefore =
        index < lineScan.parenDepthBeforeLine.size() &&
        lineScan.parenDepthBeforeLine[index] > 0;
    const bool insideOpenGroupingAfter =
        index < lineScan.parenDepthAfterLine.size() &&
        lineScan.parenDepthAfterLine[index] > 0;
    const bool insideOpenBracketBefore =
        index < lineScan.bracketDepthBeforeLine.size() &&
        lineScan.bracketDepthBeforeLine[index] > 0;
    const bool insideOpenBracketAfter =
        index < lineScan.bracketDepthAfterLine.size() &&
        lineScan.bracketDepthAfterLine[index] > 0;
    return !insideOpenGroupingBefore && !insideOpenGroupingAfter &&
           !insideOpenBracketBefore && !insideOpenBracketAfter;
  };
  auto prerequisitesForLine = [&](int line, bool expressionTypeAvailable) {
    DiagnosticsPrerequisiteState state = basePrerequisites;
    state.expressionTypeAvailable = expressionTypeAvailable;
    if (!parserRegionReliableForLine(line)) {
      state.parserRegionReliable = false;
      state.localScopeReliable = false;
    }
    return state;
  };
  auto canPublishRule = [&](DiagnosticsRuleKind rule, int line,
                            bool expressionTypeAvailable = true) {
    const DiagnosticsPrerequisiteState state =
        prerequisitesForLine(line, expressionTypeAvailable);
    DiagnosticsPrerequisiteKind missing = DiagnosticsPrerequisiteKind::None;
    if (diagnosticsRulePrerequisitesSatisfied(rule, state, missing))
      return true;
    recordDiagnosticsPrerequisiteSkip(prerequisiteSkips, missing);
    return false;
  };
  auto emitHighConfidenceDiagnostic =
      [&](DiagnosticsRuleKind rule, int line, int startByte, int endByte,
          int severity, const std::string &message,
          bool expressionTypeAvailable = true) {
        if (diags.a.size() >= maxDiagnostics)
          return;
        if (!canPublishRule(rule, line, expressionTypeAvailable))
          return;
        diags.a.push_back(
            makeDiagnostic(text, line, startByte, endByte, severity, "nsf",
                           message));
      };
  auto nextTrimmedCodeLine = [&](int currentLine) -> std::string {
    for (int nextLine = currentLine + 1;
         nextLine < static_cast<int>(trimmedCodeLines.size()); nextLine++) {
      if (nextLine < static_cast<int>(preprocessorView.lineActive.size()) &&
          !preprocessorView.lineActive[nextLine]) {
        continue;
      }
      if (!trimmedCodeLines[nextLine].empty())
        return trimmedCodeLines[nextLine];
    }
    return {};
  };
  auto previousTrimmedCodeLine = [&](int currentLine) -> std::string {
    for (int previousLine = currentLine - 1; previousLine >= 0;
         previousLine--) {
      if (previousLine < static_cast<int>(preprocessorView.lineActive.size()) &&
          !preprocessorView.lineActive[previousLine]) {
        continue;
      }
      if (!trimmedCodeLines[previousLine].empty())
        return trimmedCodeLines[previousLine];
    }
    return {};
  };
  auto shouldReportMissingSemicolonOnLine = [&](int line) {
    if (line < 0 || line >= static_cast<int>(trimmedCodeLines.size()))
      return true;
    const size_t index = static_cast<size_t>(line);
    const bool insideOpenGroupingBefore =
        (index < lineScan.parenDepthBeforeLine.size() &&
         lineScan.parenDepthBeforeLine[index] > 0) ||
        (index < lineScan.bracketDepthBeforeLine.size() &&
         lineScan.bracketDepthBeforeLine[index] > 0);
    const bool insideOpenGroupingAfter =
        (index < lineScan.parenDepthAfterLine.size() &&
         lineScan.parenDepthAfterLine[index] > 0) ||
        (index < lineScan.bracketDepthAfterLine.size() &&
         lineScan.bracketDepthAfterLine[index] > 0);
    return shouldReportMissingSemicolonShared(
        previousTrimmedCodeLine(line), trimmedCodeLines[index],
        nextTrimmedCodeLine(line),
        insideOpenGroupingBefore, insideOpenGroupingAfter);
  };
  auto shouldSkipIndeterminateOnIncompleteLine = [&](int line) {
    if (line < 0 || line >= static_cast<int>(trimmedCodeLines.size()))
      return false;
    const std::string &trimmed = trimmedCodeLines[static_cast<size_t>(line)];
    if (trimmed.find(';') != std::string::npos)
      return false;
    return !shouldReportMissingSemicolonOnLine(line);
  };
  const bool builtinRegistryAvailable = isHlslBuiltinRegistryAvailable();
  const std::string builtinRegistryError = getHlslBuiltinRegistryError();
  if (!builtinRegistryAvailable) {
    emitIndeterminate(
        0, 0, 0, "NSF_INDET_BUILTIN_REGISTRY_UNAVAILABLE",
        IndeterminateReason::DiagnosticsBuiltinRegistryUnavailable,
        "Indeterminate builtin analysis: builtin registry unavailable. " +
            builtinRegistryError);
  }

  auto isAbsolutePath = [](const std::string &path) {
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':')
      return true;
    return path.rfind("\\\\", 0) == 0 || path.rfind("/", 0) == 0;
  };
  auto joinPath = [](const std::string &base, const std::string &child) {
    if (base.empty())
      return child;
    char sep = '\\';
    if (base.back() == '/' || base.back() == '\\')
      return base + child;
    return base + sep + child;
  };
  auto addUnique = [](std::vector<std::string> &items,
                      const std::string &value) {
    for (const auto &item : items) {
      if (item == value)
        return;
    }
    items.push_back(value);
  };

  std::vector<std::string> scanRoots;
  std::string docPath = uriToPath(uri);
  if (!docPath.empty()) {
    size_t lastSlash = docPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
      scanRoots.push_back(docPath.substr(0, lastSlash));
    }
  }
  for (const auto &inc : includePaths) {
    if (inc.empty())
      continue;
    if (isAbsolutePath(inc)) {
      addUnique(scanRoots, inc);
      continue;
    }
    for (const auto &folder : workspaceFolders) {
      if (!folder.empty())
        addUnique(scanRoots, joinPath(folder, inc));
    }
  }
  for (const auto &folder : workspaceFolders) {
    if (!folder.empty())
      addUnique(scanRoots, folder);
  }
  std::vector<std::string> scanExtensions = shaderExtensions;
  StructTypeCache structCache;
  SymbolTypeCache symbolCache;
  enum class FunctionCandidateConfidence {
    AstIndexed,
    MacroDerived,
    TextFallback
  };
  struct UserFunctionCandidate {
    std::string label;
    std::vector<std::string> paramTypes;
    DefinitionLocation loc;
    FunctionCandidateConfidence confidence =
        FunctionCandidateConfidence::AstIndexed;
  };
  std::unordered_map<std::string, std::vector<UserFunctionCandidate>>
      userFunctionCache;
  std::unordered_map<std::string, std::string> fileTextCache;
  std::unordered_map<std::string, bool> fileExistsCache;
  std::vector<std::string> includeGraphFiles;

  auto formatLocationShort = [&](const DefinitionLocation &loc) {
    std::string path = uriToPath(loc.uri);
    if (path.empty())
      path = loc.uri;
    std::ostringstream oss;
    oss << path << ":" << (loc.line + 1);
    return oss.str();
  };

  auto parseParamTypeFromDecl = [&](const std::string &param) -> std::string {
    auto tokens = lexLineTokens(param);
    for (const auto &t : tokens) {
      if (t.kind != LexToken::Kind::Identifier)
        continue;
      if (isQualifierToken(t.text))
        continue;
      if (t.text == "in" || t.text == "out" || t.text == "inout")
        continue;
      return normalizeTypeToken(t.text);
    }
    return "";
  };

  auto normalizeTypeForLine = [&](const std::string &type, int line) {
    return normalizeTypeTokenWithPreprocessor(type, &preprocessorView, line);
  };

  auto relationOptionsForExpressionRange =
      [&](const std::vector<LexToken> &rangeTokens, size_t startIndex,
          size_t endIndex) {
        TypeRelationOptions options;
        endIndex = std::min(endIndex, rangeTokens.size());
        int parenDepth = 0;
        int bracketDepth = 0;
        for (size_t i = startIndex; i < endIndex; i++) {
          if (rangeTokens[i].kind != LexToken::Kind::Punct)
            continue;
          const std::string &p = rangeTokens[i].text;
          if (p == "(") {
            parenDepth++;
            continue;
          }
          if (p == ")") {
            if (parenDepth > 0)
              parenDepth--;
            continue;
          }
          if (p == "[") {
            bracketDepth++;
            continue;
          }
          if (p == "]") {
            if (bracketDepth > 0)
              bracketDepth--;
            continue;
          }
          if (parenDepth == 0 && bracketDepth == 0 &&
              (p == ";" || p == ",")) {
            endIndex = i;
            break;
          }
        }
        if (startIndex >= endIndex || startIndex >= rangeTokens.size())
          return options;
        size_t cursor = startIndex;
        NumericLiteralParseResult numeric =
            parseNumericLiteralFromTokens(rangeTokens, cursor);
        if (!numeric.matched || !numeric.valid || numeric.tokenSpan == 0)
          return options;
        cursor += numeric.tokenSpan;
        if (cursor != endIndex)
          return options;
        options.actualIsNumericLiteral = true;
        std::string literalText;
        for (size_t i = startIndex; i < endIndex && i < rangeTokens.size();
             i++) {
          literalText += rangeTokens[i].text;
        }
        options.actualLiteralText = literalText;
        return options;
      };

  auto relationForTypesWithOptions =
      [&](const std::string &expected, const std::string &actual,
          const TypeRelationOptions &options) {
        return evaluateTypeRelationWithOptions(parseTypeDesc(expected),
                                               parseTypeDesc(actual), options);
      };

  auto relationForTypes = [&](const std::string &expected,
                              const std::string &actual,
                              bool suppressWarnings = false) {
    TypeRelationOptions options;
    options.suppressWarnings = suppressWarnings;
    return relationForTypesWithOptions(expected, actual, options);
  };

  auto emitTypeRelationDiagnostic =
      [&](int line, int startByte, int endByte, const std::string &expected,
          const std::string &actual, const std::string &mismatchMessage,
          const TypeRelationOptions &options = TypeRelationOptions{}) {
        if (expected.empty() || actual.empty()) {
          canPublishRule(DiagnosticsRuleKind::ExpressionType, line, false);
          return;
        }
        if (!canPublishRule(DiagnosticsRuleKind::ExpressionType, line, true))
          return;
        TypeRelationResult relation =
            relationForTypesWithOptions(expected, actual, options);
        if (!relation.viable) {
          diags.a.push_back(makeDiagnostic(text, line, startByte, endByte, 1,
                                           "nsf", mismatchMessage));
          return;
        }
        std::string warning = typeRelationWarningMessage(relation);
        if (typeConversionRiskWarningsEnabled && !warning.empty()) {
          diags.a.push_back(
              makeDiagnostic(text, line, startByte, endByte, 2, "nsf", warning));
        }
      };

  auto emitRelationWarning = [&](int line, int startByte, int endByte,
                                  const TypeRelationResult &relation) {
    if (!canPublishRule(DiagnosticsRuleKind::ExpressionType, line, true))
      return;
    std::string warning = typeRelationWarningMessage(relation);
    if (typeConversionRiskWarningsEnabled && !warning.empty()) {
      diags.a.push_back(
          makeDiagnostic(text, line, startByte, endByte, 2, "nsf", warning));
    }
  };

  auto getUserFunctionCandidates = [&](const std::string &name)
      -> const std::vector<UserFunctionCandidate> & {
    auto it = userFunctionCache.find(name);
    if (it != userFunctionCache.end())
      return it->second;

    std::vector<UserFunctionCandidate> out;
    std::unordered_set<std::string> seenCandidateKeys;
    std::vector<IndexedDefinition> defs;
    if (workspaceSummaryRuntimeFindDefinitions(name, defs, 64)) {
      std::unordered_set<std::string> seenUris;
      for (const auto &d : defs) {
        if (d.kind != 12)
          continue;
        std::string path = uriToPath(d.uri);
        if (path.empty())
          continue;
        auto textIt = fileTextCache.find(path);
        if (textIt == fileTextCache.end()) {
          std::string loadedText;
          if (!diagnosticsReadFileToString(path, loadedText))
            continue;
          textIt = fileTextCache.emplace(path, std::move(loadedText)).first;
        }
        const std::string &defText = textIt->second;
        if (seenUris.insert(d.uri).second) {
          std::vector<SemanticSnapshotFunctionOverloadInfo> overloads;
          if (querySemanticSnapshotFunctionOverloads(
                  d.uri, defText, 0, workspaceFolders, includePaths,
                  shaderExtensions, defines, name, overloads)) {
            for (const auto &overload : overloads) {
              const std::string candidateKey =
                  d.uri + "|" + std::to_string(overload.line) + "|" +
                  std::to_string(overload.character);
              if (!seenCandidateKeys.insert(candidateKey).second)
                continue;
              std::vector<std::string> paramTypes;
              paramTypes.reserve(overload.parameters.size());
              for (const auto &p : overload.parameters)
                paramTypes.push_back(parseParamTypeFromDecl(p));
              out.push_back(UserFunctionCandidate{
                  overload.label, paramTypes,
                  DefinitionLocation{d.uri, overload.line, overload.character,
                                     overload.character +
                                         static_cast<int>(name.size())},
                  FunctionCandidateConfidence::AstIndexed});
              if (out.size() >= 16)
                break;
            }
          }
        }
        if (out.size() >= 16)
          break;
        std::string label;
        std::vector<std::string> params;
        if (!querySemanticSnapshotFunctionSignature(
                d.uri, defText, 0, workspaceFolders, includePaths,
                shaderExtensions, defines, name, d.line, d.start, label,
                params) ||
            label.empty()) {
          continue;
        }
        const std::string candidateKey =
            d.uri + "|" + std::to_string(d.line) + "|" +
            std::to_string(d.start);
        if (!seenCandidateKeys.insert(candidateKey).second)
          continue;
        std::vector<std::string> paramTypes;
        paramTypes.reserve(params.size());
        for (const auto &p : params)
          paramTypes.push_back(parseParamTypeFromDecl(p));
        out.push_back(UserFunctionCandidate{
            label, paramTypes,
            DefinitionLocation{d.uri, d.line, d.start, d.end},
            FunctionCandidateConfidence::AstIndexed});
        if (out.size() >= 16)
          break;
      }
    }
    if (out.empty()) {
      std::vector<MacroGeneratedFunctionInfo> macroCandidates;
      if (collectMacroGeneratedFunctions(uri, text, workspaceFolders,
                                         includePaths, scanExtensions, name,
                                         macroCandidates, 16)) {
        for (const auto &candidate : macroCandidates) {
          out.push_back(UserFunctionCandidate{
              candidate.label, candidate.parameterTypes, candidate.definition,
              FunctionCandidateConfidence::MacroDerived});
        }
      }
    }
    if (out.empty()) {
      std::vector<SemanticSnapshotFunctionOverloadInfo> overloads;
      if (querySemanticSnapshotFunctionOverloads(
              uri, text, 0, workspaceFolders, includePaths, shaderExtensions,
              defines, name, overloads)) {
        for (const auto &overload : overloads) {
          const std::string candidateKey =
              uri + "|" + std::to_string(overload.line) + "|" +
              std::to_string(overload.character);
          if (!seenCandidateKeys.insert(candidateKey).second)
            continue;
          std::vector<std::string> paramTypes;
          paramTypes.reserve(overload.parameters.size());
          for (const auto &p : overload.parameters)
            paramTypes.push_back(parseParamTypeFromDecl(p));
          out.push_back(UserFunctionCandidate{
              overload.label, paramTypes,
              DefinitionLocation{uri, overload.line, overload.character,
                                 overload.character +
                                     static_cast<int>(name.size())},
              FunctionCandidateConfidence::TextFallback});
          if (out.size() >= 16)
            break;
        }
      }
    }
    auto inserted = userFunctionCache.emplace(name, std::move(out));
    return inserted.first->second;
  };

  {
    auto startTime = std::chrono::steady_clock::now();
    const auto timeBudget = std::chrono::milliseconds(
        std::max(20, std::min(250, timeBudgetMs / 2)));
    const size_t fileBudget = 512;
    size_t loadedFiles = 0;

    std::unordered_set<std::string> visitedUris;
    std::vector<std::string> stackUris;
    stackUris.push_back(uri);
    visitedUris.insert(uri);

    while (!stackUris.empty()) {
      std::string currentUri = stackUris.back();
      stackUris.pop_back();
      auto elapsed = std::chrono::steady_clock::now() - startTime;
      if (elapsed > timeBudget || loadedFiles >= fileBudget)
        break;

      std::string currentText;
      if (currentUri == uri) {
        currentText = text;
      } else {
        std::string currentPath = uriToPath(currentUri);
        if (currentPath.empty())
          continue;
        auto textIt = fileTextCache.find(currentPath);
        if (textIt == fileTextCache.end()) {
          std::string loadedText;
          if (!diagnosticsReadFileToString(currentPath, loadedText))
            continue;
          textIt =
              fileTextCache.emplace(currentPath, std::move(loadedText)).first;
        }
        currentText = textIt->second;
      }
      loadedFiles++;

      std::vector<std::string> includePathList;
      if (!queryFullAstIncludes(currentUri, currentText, 0, includePathList)) {
        includePathList.clear();
      }
      for (const auto &includePath : includePathList) {
        auto candidates =
            resolveIncludeCandidates(currentUri, includePath, workspaceFolders,
                                     includePaths, scanExtensions);
        for (const auto &candidate : candidates) {
          auto existsIt = fileExistsCache.find(candidate);
          bool exists = false;
          if (existsIt == fileExistsCache.end()) {
            struct _stat statBuffer;
            exists = (_stat(candidate.c_str(), &statBuffer) == 0);
            fileExistsCache.emplace(candidate, exists);
          } else {
            exists = existsIt->second;
          }
          if (!exists)
            continue;
          addUnique(includeGraphFiles, candidate);
          std::string nextUri = pathToUri(candidate);
          if (!nextUri.empty() && visitedUris.insert(nextUri).second) {
            stackUris.push_back(nextUri);
          }
          break;
        }
      }
    }
  }

  auto collectMacroNames = [&](const std::string &sourceText,
                               std::unordered_set<std::string> &out) {
    std::istringstream stream(sourceText);
    std::string lineText;
    bool inBlockComment = false;
    while (std::getline(stream, lineText)) {
      bool maskBlock = inBlockComment;
      const auto mask = buildCodeMaskForLine(lineText, maskBlock);
      inBlockComment = maskBlock;
      if (!isPreprocessorDirectiveLine(lineText, mask))
        continue;
      const auto rawTokens = lexLineTokens(lineText);
      std::vector<LexToken> tokens;
      tokens.reserve(rawTokens.size());
      for (const auto &token : rawTokens) {
        if (token.start < mask.size() && mask[token.start])
          tokens.push_back(token);
      }
      if (tokens.size() < 3)
        continue;
      if (tokens[0].kind != LexToken::Kind::Punct || tokens[0].text != "#")
        continue;
      if (tokens[1].kind != LexToken::Kind::Identifier ||
          tokens[1].text != "define")
        continue;
      if (tokens[2].kind != LexToken::Kind::Identifier)
        continue;
      out.insert(tokens[2].text);
    }
  };

  std::unordered_set<std::string> macroNames;
  collectMacroNames(text, macroNames);
  for (const auto &path : includeGraphFiles) {
    auto textIt = fileTextCache.find(path);
    if (textIt == fileTextCache.end()) {
      std::string loadedText;
      if (!diagnosticsReadFileToString(path, loadedText))
        continue;
      textIt = fileTextCache.emplace(path, std::move(loadedText)).first;
    }
    collectMacroNames(textIt->second, macroNames);
  }
  auto isBuiltinOrKeyword = [&](const std::string &word) {
    if (word.empty())
      return true;
    if (isQualifierToken(word))
      return true;
    if (isHlslKeyword(word))
      return true;
    int dim = 0;
    if (isVectorType(word, dim))
      return true;
    if (isScalarType(word))
      return true;
    int rows = 0;
    int cols = 0;
    if (isMatrixType(word, rows, cols))
      return true;
    if (isHlslBuiltinFunction(word))
      return true;
    if (macroNames.find(word) != macroNames.end())
      return true;
    return false;
  };

  auto isKnownSymbol = [&](const std::string &word) {
    if (localsVisibleNames.find(word) != localsVisibleNames.end())
      return true;
    if (paramNames.find(word) != paramNames.end())
      return true;
    if (globalSymbols.find(word) != globalSymbols.end())
      return true;
    return false;
  };

  auto isExternallyResolvable = [&](const std::string &word) {
    auto it = resolvedSymbolCache.find(word);
    if (it != resolvedSymbolCache.end())
      return it->second;
    std::string indexedType;
    if (workspaceSummaryRuntimeGetSymbolType(word, indexedType) &&
        !indexedType.empty()) {
      resolvedSymbolCache.emplace(word, true);
      return true;
    }
    std::vector<IndexedDefinition> defs;
    const bool ok = workspaceSummaryRuntimeFindDefinitions(word, defs, 1) &&
                    !defs.empty();
    resolvedSymbolCache.emplace(word, ok);
    return ok;
  };

  auto findLocalScope = [&](int scopeId) -> LocalScopeFrame * {
    for (auto &scope : localScopes) {
      if (scope.id == scopeId)
        return &scope;
    }
    return nullptr;
  };

  auto currentLocalScope = [&]() -> LocalScopeFrame * {
    if (localScopeStack.empty())
      return nullptr;
    return findLocalScope(localScopeStack.back());
  };

  auto enterLocalScope = [&](int braceDepth, bool forScope = false,
                             int closeAfterReturnToDepth = -1) -> int {
    LocalScopeFrame frame;
    frame.id = nextLocalScopeId++;
    frame.parentId = localScopeStack.empty() ? -1 : localScopeStack.back();
    frame.braceDepth = braceDepth;
    frame.forScope = forScope;
    frame.closeAfterReturnToDepth = closeAfterReturnToDepth;
    localScopes.push_back(std::move(frame));
    localScopeStack.push_back(localScopes.back().id);
    return localScopeStack.back();
  };

  auto resetLocalScopes = [&]() {
    localScopes.clear();
    localScopeStack.clear();
    pendingForScopes.clear();
    nextLocalScopeId = 0;
    enterLocalScope(1);
  };

  auto exitTopLocalScope = [&]() {
    if (localScopeStack.size() <= 1)
      return;
    localScopeStack.pop_back();
  };

  auto closeForScopesReturnedToDepth = [&](int depth) {
    while (localScopeStack.size() > 1) {
      LocalScopeFrame *scope = findLocalScope(localScopeStack.back());
      if (!scope || !scope->forScope ||
          scope->closeAfterReturnToDepth != depth) {
        break;
      }
      localScopeStack.pop_back();
    }
  };

  auto erasePendingForScope = [&](int scopeId) {
    pendingForScopes.erase(
        std::remove_if(pendingForScopes.begin(), pendingForScopes.end(),
                       [&](const PendingForScope &pending) {
                         return pending.scopeId == scopeId;
                       }),
        pendingForScopes.end());
  };

  auto declareLocalInCurrentScope =
      [&](const std::string &name, const std::string &type,
          const PreprocBranchSig &sig) {
        LocalScopeFrame *scope = currentLocalScope();
        if (!scope)
          return;
        scope->localsByName[name].push_back(
            LocalDeclEntry{normalizeTypeToken(type), sig});
      };

  auto hasDuplicateInCurrentScope = [&](const std::string &name,
                                        const PreprocBranchSig &sig) {
    LocalScopeFrame *scope = currentLocalScope();
    if (!scope)
      return false;
    auto it = scope->localsByName.find(name);
    if (it == scope->localsByName.end())
      return false;
    for (const auto &decl : it->second) {
      if (preprocBranchSigsOverlap(decl.sig, sig))
        return true;
    }
    return false;
  };

  auto rebuildVisibleLocals = [&](const PreprocBranchSig &sig) {
    localsVisibleTypes.clear();
    localsVisibleNames.clear();
    std::unordered_set<std::string> resolvedNames;
    for (auto stackIt = localScopeStack.rbegin();
         stackIt != localScopeStack.rend(); ++stackIt) {
      LocalScopeFrame *scope = findLocalScope(*stackIt);
      if (!scope)
        continue;
      for (const auto &entry : scope->localsByName) {
        const std::string &name = entry.first;
        if (resolvedNames.find(name) != resolvedNames.end())
          continue;
        bool any = false;
        std::string soleType;
        bool typeSet = false;
        bool ambiguous = false;
        for (const auto &decl : entry.second) {
          if (!preprocBranchSigsOverlap(decl.sig, sig))
            continue;
          any = true;
          std::string t = normalizeTypeToken(decl.type);
          if (!typeSet) {
            soleType = t;
            typeSet = true;
          } else if (t != soleType) {
            ambiguous = true;
          }
        }
        if (!any)
          continue;
        resolvedNames.insert(name);
        localsVisibleNames.insert(name);
        if (typeSet && !ambiguous && !soleType.empty()) {
          localsVisibleTypes.emplace(name, soleType);
        }
      }
    }
  };

  std::unordered_map<int, int> activeBranchIndexById;
  for (const auto &merge : preprocessorView.branchMerges) {
    if (merge.activeBranchIndex >= 0)
      activeBranchIndexById[merge.branchId] = merge.activeBranchIndex;
  }

  auto isSemanticActiveLine = [&](int line, const PreprocBranchSig &sig) {
    if (line < static_cast<int>(preprocessorView.lineActive.size()) &&
        !preprocessorView.lineActive[line]) {
      return false;
    }
    for (const auto &entry : sig) {
      auto it = activeBranchIndexById.find(entry.first);
      if (it != activeBranchIndexById.end() && entry.second != it->second)
        return false;
    }
    return true;
  };

  bool unreachableActive = false;
  int unreachableDepth = 0;
  std::vector<FlowBlockKind> flowBlockKinds;
  std::vector<FlowBlockKind> nextOpenBraceKinds;
  size_t nextOpenBraceKindIndex = 0;
  FlowBlockKind pendingControlOpenBlockKind = FlowBlockKind::Other;

  auto consumeNextOpenBraceKind = [&]() {
    if (nextOpenBraceKindIndex < nextOpenBraceKinds.size())
      return nextOpenBraceKinds[nextOpenBraceKindIndex++];
    return FlowBlockKind::Other;
  };

  auto buildOpenBraceKindsForLine =
      [](const std::vector<LexToken> &lineTokens) {
        std::vector<FlowBlockKind> kinds;
        FlowBlockKind pendingKind = FlowBlockKind::Other;
        for (const auto &token : lineTokens) {
          if (token.kind == LexToken::Kind::Identifier) {
            if (token.text == "if" || token.text == "else") {
              pendingKind = FlowBlockKind::Branch;
            } else if (token.text == "for" || token.text == "while" ||
                       token.text == "switch" || token.text == "do") {
              pendingKind = FlowBlockKind::LoopOrSwitch;
            }
            continue;
          }
          if (token.kind != LexToken::Kind::Punct)
            continue;
          if (token.text == "{") {
            kinds.push_back(pendingKind);
            pendingKind = FlowBlockKind::Other;
          } else if (token.text == ";") {
            pendingKind = FlowBlockKind::Other;
          }
        }
        return kinds;
      };

  auto leadingControlKindForLine =
      [](const std::vector<LexToken> &lineTokens) {
        for (const auto &token : lineTokens) {
          if (token.kind != LexToken::Kind::Identifier)
            continue;
          if (token.text == "if" || token.text == "else")
            return FlowBlockKind::Branch;
          if (token.text == "for" || token.text == "while" ||
              token.text == "switch" || token.text == "do")
            return FlowBlockKind::LoopOrSwitch;
          break;
        }
        return FlowBlockKind::Other;
      };

  auto isUnbracedBranchControlHeaderLine =
      [](const std::vector<LexToken> &lineTokens) {
        bool sawLeadingBranchControl = false;
        for (const auto &token : lineTokens) {
          if (token.kind == LexToken::Kind::Identifier) {
            if (!sawLeadingBranchControl) {
              if (token.text != "if" && token.text != "else")
                return false;
              sawLeadingBranchControl = true;
              continue;
            }
            continue;
          }
          if (token.kind != LexToken::Kind::Punct)
            continue;
          if (token.text == "{" || token.text == ";")
            return false;
        }
        return sawLeadingBranchControl;
      };

  auto isUnbracedElseControlHeaderLine =
      [](const std::vector<LexToken> &lineTokens) {
        for (const auto &token : lineTokens) {
          if (token.kind != LexToken::Kind::Identifier)
            continue;
          return token.text == "else";
        }
        return false;
      };

  auto lineStartsWithOpenBrace = [](const std::vector<LexToken> &lineTokens) {
    for (const auto &token : lineTokens) {
      if (token.kind == LexToken::Kind::Punct && token.text == "{")
        return true;
      if (token.kind == LexToken::Kind::Identifier)
        return false;
      if (token.kind == LexToken::Kind::Punct && token.text != "}")
        return false;
    }
    return false;
  };

  auto scanForBraces = [&](const std::string &line) {
    for (size_t i = 0; i < line.size(); i++) {
      char ch = line[i];
      char next = (i + 1 < line.size()) ? line[i + 1] : '\0';
      if (inBlockComment) {
        if (ch == '*' && next == '/') {
          inBlockComment = false;
          i++;
        }
        continue;
      }
      if (inString) {
        if (ch == '"' && (i == 0 || line[i - 1] != '\\'))
          inString = false;
        continue;
      }
      if (ch == '"') {
        inString = true;
        continue;
      }
      if (ch == '/' && next == '/') {
        break;
      }
      if (ch == '/' && next == '*') {
        inBlockComment = true;
        i++;
        continue;
      }
      if (ch == '{') {
        if (inFunction) {
          const FlowBlockKind blockKind = consumeNextOpenBraceKind();
          functionBraceDepth++;
          flowBlockKinds.push_back(blockKind);
          for (size_t pendingIndex = 0; pendingIndex < pendingForScopes.size();
               pendingIndex++) {
            if (pendingForScopes[pendingIndex].parentDepth ==
                functionBraceDepth - 1) {
              const int pendingScopeId = pendingForScopes[pendingIndex].scopeId;
              LocalScopeFrame *scope = findLocalScope(pendingScopeId);
              if (scope)
                scope->closeAfterReturnToDepth = functionBraceDepth - 1;
              erasePendingForScope(pendingScopeId);
              break;
            }
          }
          enterLocalScope(functionBraceDepth);
        } else if (pendingSignature) {
          std::string signatureKey = pendingFunctionName + "(";
          for (size_t i = 0; i < pendingParamTypesOrdered.size(); i++) {
            if (i > 0)
              signatureKey.push_back(',');
            signatureKey += normalizeTypeToken(pendingParamTypesOrdered[i]);
          }
          signatureKey.push_back(')');
          if (!globalFunctionSignatures.insert(signatureKey).second) {
            emitHighConfidenceDiagnostic(
                DiagnosticsRuleKind::SemanticSource, pendingFunctionLine,
                pendingFunctionStart,
                pendingFunctionStart +
                    static_cast<int>(pendingFunctionName.size()),
                2,
                "Duplicate global declaration: " + pendingFunctionName + ".");
          }
          inFunction = true;
          functionReturnType = pendingReturnType;
          functionName = pendingFunctionName;
          functionNameLine = pendingFunctionLine;
          functionNameStart = pendingFunctionStart;
          functionNameEnd = pendingFunctionStart +
                            static_cast<int>(pendingFunctionName.size());
          functionBraceDepth = 1;
          sawReturn = false;
          sawTopLevelReturn = false;
          sawPotentialMissingReturn = false;
          sawPotentialUnreachable = false;
          pendingUnbracedBranchLineByDepth.clear();
          pendingUnbracedBranchIsElseByDepth.clear();
          conditionalReturnSeen = false;
          unconditionalReturnSeen = false;
          pendingControlOpenBlockKind = FlowBlockKind::Other;
          flowBlockKinds.clear();
          flowBlockKinds.push_back(FlowBlockKind::Function);
          resetLocalScopes();
          localsVisibleTypes.clear();
          localsVisibleNames.clear();
          paramNames.clear();
          for (const auto &entry : pendingParams) {
            paramNames.insert(entry.first);
            declareLocalInCurrentScope(entry.first, entry.second,
                                       PreprocBranchSig{});
          }
          rebuildVisibleLocals(PreprocBranchSig{});
          pendingParams.clear();
          pendingParamTypesOrdered.clear();
          pendingSignature = false;
          unreachableActive = false;
          unreachableDepth = 0;
        } else if (pendingTypeBlockOpen || typeBlockBraceDepth > 0) {
          typeBlockBraceDepth++;
          pendingTypeBlockOpen = false;
        }
        continue;
      }
      if (ch == '}') {
        if (inFunction) {
          const FlowBlockKind closingKind =
              flowBlockKinds.empty() ? FlowBlockKind::Other
                                     : flowBlockKinds.back();
          if ((closingKind == FlowBlockKind::Branch ||
               closingKind == FlowBlockKind::LoopOrSwitch) &&
              unreachableActive && unreachableDepth >= functionBraceDepth) {
            unreachableActive = false;
            unreachableDepth = 0;
          }
          if (functionBraceDepth == 1 && !pendingMultilineLocalName.empty() &&
              pendingMultilineLocalDepth == 1 &&
              pendingMultilineLocalLine >= 0 &&
              pendingMultilineLocalStart >= 0 &&
              pendingMultilineLocalEnd >= pendingMultilineLocalStart) {
            diags.a.push_back(makeDiagnostic(
                text, pendingMultilineLocalLine, pendingMultilineLocalStart,
                pendingMultilineLocalEnd, 1, "nsf", "Missing semicolon."));
          }
          if (functionBraceDepth > 1)
            exitTopLocalScope();
          functionBraceDepth =
              functionBraceDepth > 0 ? functionBraceDepth - 1 : 0;
          if (!flowBlockKinds.empty())
            flowBlockKinds.pop_back();
          closeForScopesReturnedToDepth(functionBraceDepth);
          if (functionBraceDepth == 0) {
            if (normalizeTypeToken(functionReturnType) != "void" &&
                !sawReturn) {
              emitHighConfidenceDiagnostic(DiagnosticsRuleKind::SemanticSource,
                                           functionNameLine, functionNameStart,
                                           functionNameEnd, 1,
                                           "Missing return statement.");
            }
            if (normalizeTypeToken(functionReturnType) != "void" && sawReturn &&
                sawPotentialMissingReturn) {
              emitHighConfidenceDiagnostic(
                  DiagnosticsRuleKind::SemanticSource, functionNameLine,
                  functionNameStart, functionNameEnd, 2,
                  "Potential missing return on some paths.");
            }
            inFunction = false;
            functionReturnType.clear();
            functionName.clear();
            localScopes.clear();
            localScopeStack.clear();
            pendingForScopes.clear();
            localsVisibleTypes.clear();
            localsVisibleNames.clear();
            paramNames.clear();
            pendingMultilineLocalName.clear();
            pendingMultilineLocalDepth = -1;
            pendingMultilineLocalLine = -1;
            pendingMultilineLocalStart = -1;
            pendingMultilineLocalEnd = -1;
            unreachableActive = false;
            unreachableDepth = 0;
            flowBlockKinds.clear();
            pendingControlOpenBlockKind = FlowBlockKind::Other;
            pendingUnbracedBranchLineByDepth.clear();
            pendingUnbracedBranchIsElseByDepth.clear();
          }
        } else if (typeBlockBraceDepth > 0) {
          typeBlockBraceDepth--;
          if (typeBlockBraceDepth == 0)
            pendingTypeBlockOpen = false;
        }
        continue;
      }
    }
  };

  const PreprocBranchSig emptySig;
  auto collectSignatureParamsFromTokens =
      [&](const std::vector<LexToken> &sigTokens, size_t startIndex,
          size_t endExclusive, int diagLine) {
        if (startIndex >= endExclusive || endExclusive > sigTokens.size())
          return;
        size_t segmentStart = startIndex;
        for (size_t i = startIndex; i <= endExclusive; i++) {
          bool atEnd = (i == endExclusive);
          bool atComma =
              (!atEnd && sigTokens[i].kind == LexToken::Kind::Punct &&
               sigTokens[i].text == ",");
          if (!atEnd && !atComma)
            continue;
          size_t segmentEnd = i;
          std::string paramType;
          std::string paramName;
          for (size_t j = segmentStart; j < segmentEnd; j++) {
            if (sigTokens[j].kind == LexToken::Kind::Punct &&
                (sigTokens[j].text == ":" || sigTokens[j].text == "="))
              break;
            if (sigTokens[j].kind != LexToken::Kind::Identifier)
              continue;
            if (isQualifierToken(sigTokens[j].text))
              continue;
            if (paramType.empty()) {
              paramType = sigTokens[j].text;
              continue;
            }
            if (paramName.empty())
              paramName = sigTokens[j].text;
          }
          if (!paramType.empty() && !paramName.empty()) {
            if (pendingParams.find(paramName) != pendingParams.end()) {
              int start = 0;
              int end = 0;
              if (!atEnd && i < sigTokens.size()) {
                start = static_cast<int>(sigTokens[i].start);
                end = static_cast<int>(sigTokens[i].end);
              } else if (segmentEnd > segmentStart &&
                         segmentEnd - 1 < sigTokens.size()) {
                start = static_cast<int>(sigTokens[segmentEnd - 1].start);
                end = static_cast<int>(sigTokens[segmentEnd - 1].end);
              }
              emitHighConfidenceDiagnostic(
                  DiagnosticsRuleKind::SemanticSource, diagLine, start, end, 2,
                  "Duplicate parameter declaration: " + paramName + ".");
            } else {
              const std::string normalizedParamType =
                  normalizeTypeForLine(paramType, diagLine);
              pendingParams.emplace(paramName, normalizedParamType);
              pendingParamTypesOrdered.push_back(normalizedParamType);
            }
          }
          segmentStart = i + 1;
        }
      };

  while (std::getline(stream, lineText)) {
    if (diags.a.size() >= maxDiagnostics)
      return;
    if (std::chrono::steady_clock::now() - diagnosticsStart >
        diagnosticsBudget) {
      timedOut = true;
      return;
    }
    bool maskBlock = inBlockComment;
    const auto mask = buildCodeMaskForLine(lineText, maskBlock);
    inBlockComment = maskBlock;
    const auto rawTokens = lexLineTokens(lineText);
    std::vector<LexToken> tokens;
    tokens.reserve(rawTokens.size());
    for (const auto &token : rawTokens) {
      if (token.start < mask.size() && mask[token.start])
        tokens.push_back(token);
    }
    nextOpenBraceKinds = buildOpenBraceKindsForLine(tokens);
    if (!nextOpenBraceKinds.empty() &&
        nextOpenBraceKinds.front() == FlowBlockKind::Other &&
        pendingControlOpenBlockKind != FlowBlockKind::Other) {
      nextOpenBraceKinds.front() = pendingControlOpenBlockKind;
    }
    nextOpenBraceKindIndex = 0;

    const PreprocBranchSig &currentSig =
        (lineIndex < static_cast<int>(preprocessorView.branchSigs.size())
             ? preprocessorView.branchSigs[lineIndex]
             : emptySig);
    if (!isSemanticActiveLine(lineIndex, currentSig)) {
      lineIndex++;
      continue;
    }

    if (isPreprocessorDirectiveLine(lineText, mask)) {
      scanForBraces(lineText);
      lineIndex++;
      continue;
    }

    bool lineReportedMissingSemicolon = false;
    bool pendingUiVarStartedThisLine = false;
    auto emitCurrentLineMissingSemicolon = [&](int startByte, int endByte) {
      diags.a.push_back(makeDiagnostic(text, lineIndex, startByte, endByte, 1,
                                       "nsf", "Missing semicolon."));
      lineReportedMissingSemicolon = true;
    };

    bool lineStartsTypeBlock = false;
    if (!inFunction && !tokens.empty() &&
        tokens[0].kind == LexToken::Kind::Identifier &&
        (tokens[0].text == "struct" || tokens[0].text == "cbuffer")) {
      bool hasSemicolon = false;
      for (const auto &token : tokens) {
        if (token.kind == LexToken::Kind::Punct && token.text == ";") {
          hasSemicolon = true;
          break;
        }
      }
      lineStartsTypeBlock = !hasSemicolon;
    }
    if (lineStartsTypeBlock)
      pendingTypeBlockOpen = true;

    if (!inFunction && typeBlockBraceDepth == 0 && !pendingTypeBlockOpen &&
        pendingUiVarName.empty()) {
      std::string uiMetaType;
      std::string uiMetaName;
      size_t uiMetaStart = 0;
      size_t uiMetaEnd = 0;
      if (findMetadataDeclarationHeaderPosShared(
              lineText, uiMetaType, uiMetaName, uiMetaStart, uiMetaEnd)) {
        pendingUiVarName = uiMetaName;
        pendingUiVarLine = lineIndex;
        pendingUiVarStart = static_cast<int>(uiMetaStart);
        pendingUiVarEnd = static_cast<int>(uiMetaEnd);
        pendingUiVarStartedThisLine = true;
      }
    }

    if (!pendingUiVarName.empty() && !inUiMetaBlock) {
      if (tokens.size() == 1 && tokens[0].kind == LexToken::Kind::Punct &&
          tokens[0].text == "<") {
        if (!globalSymbols.insert(pendingUiVarName).second) {
          emitHighConfidenceDiagnostic(
              DiagnosticsRuleKind::SemanticSource, pendingUiVarLine,
              pendingUiVarStart, pendingUiVarEnd, 2,
              "Duplicate global declaration: " + pendingUiVarName + ".");
        }
        inUiMetaBlock = true;
        lineIndex++;
        continue;
      }
      if (!tokens.empty() && !pendingUiVarStartedThisLine) {
        pendingUiVarName.clear();
        pendingUiVarLine = -1;
        pendingUiVarStart = -1;
        pendingUiVarEnd = -1;
      }
    }

    if (inUiMetaBlock) {
      if (!tokens.empty() && tokens[0].kind == LexToken::Kind::Punct &&
          tokens[0].text == ">") {
        inUiMetaBlock = false;
        pendingUiVarName.clear();
        pendingUiVarLine = -1;
        pendingUiVarStart = -1;
        pendingUiVarEnd = -1;
      } else {
        lineIndex++;
        continue;
      }
    }

    if (!inFunction && typeBlockBraceDepth == 0 && !pendingTypeBlockOpen &&
        tokens.size() >= 2) {
      size_t typeIndex = std::string::npos;
      for (size_t t = 0; t < tokens.size(); t++) {
        if (tokens[t].kind != LexToken::Kind::Identifier)
          continue;
        if (isQualifierToken(tokens[t].text))
          continue;
        if (tokens[t].text == "return" || tokens[t].text == "if" ||
            tokens[t].text == "for" || tokens[t].text == "while" ||
            tokens[t].text == "switch" || tokens[t].text == "struct" ||
            tokens[t].text == "cbuffer")
          break;
        typeIndex = t;
        break;
      }
      if (typeIndex != std::string::npos && typeIndex + 1 < tokens.size()) {
        if (tokens[typeIndex + 1].kind == LexToken::Kind::Identifier) {
          bool hasSemi = false;
          for (const auto &t : tokens) {
            if (t.kind == LexToken::Kind::Punct && t.text == ";") {
              hasSemi = true;
              break;
            }
          }
          if (hasSemi) {
            const std::string name = tokens[typeIndex + 1].text;
            if (name.rfind("SasUi", 0) == 0) {
              lineIndex++;
              continue;
            }
            if (!globalSymbols.insert(name).second) {
              emitHighConfidenceDiagnostic(
                  DiagnosticsRuleKind::SemanticSource, lineIndex,
                  static_cast<int>(tokens[typeIndex + 1].start),
                  static_cast<int>(tokens[typeIndex + 1].end), 2,
                  "Duplicate global declaration: " + name + ".");
            }
          }
        }
      }
    }

    bool signatureStartedThisLine = false;
    if (!inFunction && !pendingSignature) {
      size_t typeIndex = std::string::npos;
      for (size_t t = 0; t < tokens.size(); t++) {
        if (tokens[t].kind != LexToken::Kind::Identifier)
          continue;
        if (isQualifierToken(tokens[t].text))
          continue;
        if (tokens[t].text == "return" || tokens[t].text == "if" ||
            tokens[t].text == "for" || tokens[t].text == "while" ||
            tokens[t].text == "switch" || tokens[t].text == "struct" ||
            tokens[t].text == "cbuffer")
          break;
        typeIndex = t;
        break;
      }
      if (typeIndex != std::string::npos && typeIndex + 2 < tokens.size()) {
        if (tokens[typeIndex + 1].kind == LexToken::Kind::Identifier &&
            tokens[typeIndex + 2].kind == LexToken::Kind::Punct &&
            tokens[typeIndex + 2].text == "(") {
          pendingReturnType = normalizeTypeForLine(tokens[typeIndex].text,
                                                   lineIndex);
          pendingFunctionName = tokens[typeIndex + 1].text;
          pendingFunctionLine = lineIndex;
          pendingFunctionStart = static_cast<int>(tokens[typeIndex + 1].start);
          pendingSignature = true;
          signatureStartedThisLine = true;
          pendingParams.clear();
          pendingParamTypesOrdered.clear();

          int parenDepth = 0;
          size_t openIndex = typeIndex + 2;
          size_t closeIndex = std::string::npos;
          for (size_t i = openIndex; i < tokens.size(); i++) {
            if (tokens[i].kind != LexToken::Kind::Punct)
              continue;
            if (tokens[i].text == "(") {
              parenDepth++;
              continue;
            }
            if (tokens[i].text == ")") {
              parenDepth--;
              if (parenDepth == 0) {
                closeIndex = i;
                break;
              }
            }
          }
          size_t paramEndExclusive =
              (closeIndex != std::string::npos ? closeIndex : tokens.size());
          collectSignatureParamsFromTokens(tokens, openIndex + 1,
                                           paramEndExclusive, lineIndex);
        }
      }
    }

    if (!inFunction && pendingSignature && !signatureStartedThisLine) {
      int parenDepth = 0;
      size_t closeIndex = std::string::npos;
      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind != LexToken::Kind::Punct)
          continue;
        if (tokens[i].text == "(") {
          parenDepth++;
          continue;
        }
        if (tokens[i].text == ")") {
          if (parenDepth == 0) {
            closeIndex = i;
            break;
          }
          parenDepth--;
        }
      }
      size_t paramEndExclusive =
          (closeIndex != std::string::npos ? closeIndex : tokens.size());
      collectSignatureParamsFromTokens(tokens, 0, paramEndExclusive, lineIndex);
    }

    if (inFunction) {
      rebuildVisibleLocals(currentSig);
      bool currentLineControlledByPendingUnbracedBranch = false;
      bool currentLineControlledByPendingUnbracedElse = false;
      bool consumePendingUnbracedBranch = false;
      if (!tokens.empty()) {
        auto pendingBranchIt =
            pendingUnbracedBranchLineByDepth.find(functionBraceDepth);
        if (pendingBranchIt != pendingUnbracedBranchLineByDepth.end() &&
            pendingBranchIt->second < lineIndex) {
          consumePendingUnbracedBranch = true;
          currentLineControlledByPendingUnbracedBranch =
              !lineStartsWithOpenBrace(tokens);
          auto pendingElseIt =
              pendingUnbracedBranchIsElseByDepth.find(functionBraceDepth);
          currentLineControlledByPendingUnbracedElse =
              currentLineControlledByPendingUnbracedBranch &&
              pendingElseIt != pendingUnbracedBranchIsElseByDepth.end() &&
              pendingElseIt->second;
        }
      }
      bool macroLocalsChanged = false;
      const auto macroLocalDeclarations =
          collectStatementLikeMacroLocalDeclarations(preprocessorView,
                                                     lineIndex, lineText);
      for (const auto &decl : macroLocalDeclarations) {
        if (decl.name.empty() || decl.type.empty())
          continue;
        if (hasDuplicateInCurrentScope(decl.name, currentSig)) {
          emitHighConfidenceDiagnostic(
              DiagnosticsRuleKind::SemanticSource, lineIndex,
              decl.invocationStart, decl.invocationEnd, 2,
              "Duplicate local declaration: " + decl.name + ".");
        }
        declareLocalInCurrentScope(decl.name,
                                   normalizeTypeForLine(decl.type, lineIndex),
                                   currentSig);
        macroLocalsChanged = true;
      }
      if (macroLocalsChanged)
        rebuildVisibleLocals(currentSig);
      size_t declarationEqTokenIndex = std::string::npos;
      if (!pendingMultilineLocalName.empty()) {
        bool hasSemi = false;
        for (const auto &t : tokens) {
          if (t.kind == LexToken::Kind::Punct && t.text == ";") {
            hasSemi = true;
            break;
          }
        }
        if (hasSemi && functionBraceDepth >= pendingMultilineLocalDepth) {
          pendingMultilineLocalName.clear();
          pendingMultilineLocalDepth = -1;
          pendingMultilineLocalLine = -1;
          pendingMultilineLocalStart = -1;
          pendingMultilineLocalEnd = -1;
        }
      }

      if (unreachableActive && functionBraceDepth == unreachableDepth) {
        for (const auto &token : tokens) {
          if (token.kind == LexToken::Kind::Punct &&
              (token.text == ";" || token.text == "}"))
            continue;
          emitHighConfidenceDiagnostic(DiagnosticsRuleKind::SemanticSource,
                                       lineIndex,
                                       static_cast<int>(token.start),
                                       static_cast<int>(token.end), 2,
                                       "Unreachable code.");
          unreachableActive = false;
          break;
        }
      }

      for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        const std::string kw = tokens[i].text;
        if (kw != "if" && kw != "for" && kw != "while" && kw != "switch")
          continue;
        if (tokens[i + 1].kind != LexToken::Kind::Punct ||
            tokens[i + 1].text != "(") {
          diags.a.push_back(
              makeDiagnostic(text, lineIndex, static_cast<int>(tokens[i].start),
                             static_cast<int>(tokens[i].end), 1, "nsf",
                             "Missing parentheses after " + kw + "."));
        }
      }

      const auto forInitializerDecls =
          extractForInitializerDeclarationsInLineShared(lineText);
      if (!forInitializerDecls.empty()) {
        const int forScopeId =
            enterLocalScope(functionBraceDepth, true, functionBraceDepth);
        pendingForScopes.push_back(
            PendingForScope{forScopeId, functionBraceDepth, lineIndex});
        bool localsChanged = false;
        for (const auto &decl : forInitializerDecls) {
          if (paramNames.find(decl.name) != paramNames.end()) {
            emitHighConfidenceDiagnostic(
                DiagnosticsRuleKind::SemanticSource, lineIndex,
                static_cast<int>(decl.start), static_cast<int>(decl.end), 2,
                "Local shadows parameter: " + decl.name + ".");
          } else if (hasDuplicateInCurrentScope(decl.name, currentSig)) {
            emitHighConfidenceDiagnostic(
                DiagnosticsRuleKind::SemanticSource, lineIndex,
                static_cast<int>(decl.start), static_cast<int>(decl.end), 2,
                "Duplicate local declaration: " + decl.name + ".");
          }
          declareLocalInCurrentScope(decl.name,
                                     normalizeTypeForLine(decl.type, lineIndex),
                                     currentSig);
          localsChanged = true;
        }
        if (localsChanged)
          rebuildVisibleLocals(currentSig);
      }

      bool handledSharedLocalDeclarations = false;
      bool lineHasSemicolon = false;
      for (const auto &t : tokens) {
        if (t.kind == LexToken::Kind::Punct && t.text == ";") {
          lineHasSemicolon = true;
          break;
        }
      }
      if (lineHasSemicolon) {
        const auto declaredNames = extractDeclaredNamesFromLine(lineText);
        if (!declaredNames.empty()) {
          handledSharedLocalDeclarations = true;
          auto findNameTokenIndex = [&](const std::string &name) -> size_t {
            for (size_t ti = 0; ti < tokens.size(); ti++) {
              if (tokens[ti].kind != LexToken::Kind::Identifier ||
                  tokens[ti].text != name) {
                continue;
              }
              if (ti > 0 && tokens[ti - 1].kind == LexToken::Kind::Punct &&
                   (tokens[ti - 1].text == "." || tokens[ti - 1].text == "->" ||
                    tokens[ti - 1].text == "::")) {
                continue;
              }
              return ti;
            }
            return tokens.size();
          };
          bool localsChanged = false;
          for (const auto &name : declaredNames) {
            std::string localType;
            if (!findTypeOfIdentifierInDeclarationLineShared(lineText, name,
                                                             localType)) {
              continue;
            }
            const size_t nameTokenIndex = findNameTokenIndex(name);
            const LexToken *nameToken =
                nameTokenIndex < tokens.size() ? &tokens[nameTokenIndex] : nullptr;
            int nameStart = nameToken ? static_cast<int>(nameToken->start) : 0;
            int nameEnd = nameToken ? static_cast<int>(nameToken->end) : 0;
            if (paramNames.find(name) != paramNames.end()) {
              emitHighConfidenceDiagnostic(
                  DiagnosticsRuleKind::SemanticSource, lineIndex, nameStart,
                  nameEnd, 2, "Local shadows parameter: " + name + ".");
            } else if (hasDuplicateInCurrentScope(name, currentSig)) {
              emitHighConfidenceDiagnostic(
                  DiagnosticsRuleKind::SemanticSource, lineIndex, nameStart,
                  nameEnd, 2, "Duplicate local declaration: " + name + ".");
            }
            const std::string normalizedLocalType =
                normalizeTypeForLine(localType, lineIndex);
            declareLocalInCurrentScope(name, normalizedLocalType, currentSig);
            localsChanged = true;
            if (nameTokenIndex < tokens.size()) {
              int parenDepth = 0;
              int bracketDepth = 0;
              int braceDepth = 0;
              size_t eqIndex = tokens.size();
              size_t segmentEnd = tokens.size();
              for (size_t ti = nameTokenIndex + 1; ti < tokens.size(); ti++) {
                if (tokens[ti].kind == LexToken::Kind::Punct) {
                  const std::string &punct = tokens[ti].text;
                  if (punct == "(")
                    parenDepth++;
                  else if (punct == ")" && parenDepth > 0)
                    parenDepth--;
                  else if (punct == "[")
                    bracketDepth++;
                  else if (punct == "]" && bracketDepth > 0)
                    bracketDepth--;
                  else if (punct == "{")
                    braceDepth++;
                  else if (punct == "}" && braceDepth > 0)
                    braceDepth--;
                  else if (parenDepth == 0 && bracketDepth == 0 &&
                           braceDepth == 0) {
                    if (punct == "=" && eqIndex == tokens.size()) {
                      eqIndex = ti;
                      continue;
                    }
                    if (punct == "," || punct == ";") {
                      segmentEnd = ti;
                      break;
                    }
                  }
                }
              }
              if (eqIndex < tokens.size()) {
                std::string lhsType = normalizedLocalType;
                TypeEvalResult rhsEval;
                rhsEval.type = normalizeTypeToken(inferExpressionTypeFromTokensRange(
                    tokens, eqIndex + 1, segmentEnd, localsVisibleTypes, uri,
                    text, scanRoots, workspaceFolders, includePaths,
                    scanExtensions, shaderExtensions, defines, structCache,
                    symbolCache, &fileTextCache, &preprocessorView,
                    lineIndex));
                rhsEval.confidence = TypeEvalConfidence::L2;
                if (rhsEval.type.empty() && isHalfFamilyType(lhsType)) {
                  rhsEval.type = inferNarrowingFallbackRhsTypeFromTokens(
                      tokens, eqIndex + 1, segmentEnd);
                  if (!rhsEval.type.empty()) {
                    rhsEval.confidence = TypeEvalConfidence::L3;
                    rhsEval.reasonCode =
                        IndeterminateReason::DiagnosticsHeavyRulesSkipped;
                  } else {
                    rhsEval.reasonCode =
                        IndeterminateReason::DiagnosticsRhsTypeEmpty;
                    if (!shouldSkipIndeterminateOnIncompleteLine(lineIndex)) {
                      emitIndeterminate(
                          lineIndex, static_cast<int>(tokens[eqIndex].start),
                          static_cast<int>(tokens[eqIndex].end),
                          "NSF_INDET_RHS_TYPE_EMPTY", rhsEval.reasonCode,
                          "Indeterminate assignment type: rhs type unavailable.");
                    }
                  }
                }
                const std::string rhsType = rhsEval.type;
                emitTypeRelationDiagnostic(
                    lineIndex, static_cast<int>(tokens[eqIndex].start),
                    static_cast<int>(tokens[eqIndex].end), lhsType, rhsType,
                    "Assignment type mismatch: " + lhsType + " = " + rhsType +
                        ".",
                    relationOptionsForExpressionRange(tokens, eqIndex + 1,
                                                       segmentEnd));
              }
            }
          }
          if (localsChanged)
            rebuildVisibleLocals(currentSig);
        }
      }

      size_t typeIndex = std::string::npos;
      for (size_t t = 0; t < tokens.size(); t++) {
        if (tokens[t].kind != LexToken::Kind::Identifier)
          continue;
        if (isQualifierToken(tokens[t].text))
          continue;
        if (tokens[t].text == "return" || tokens[t].text == "if" ||
            tokens[t].text == "for" || tokens[t].text == "while" ||
            tokens[t].text == "switch")
          break;
        typeIndex = t;
        break;
      }
      if (!handledSharedLocalDeclarations && typeIndex != std::string::npos &&
          typeIndex + 1 < tokens.size()) {
        if (tokens[typeIndex + 1].kind == LexToken::Kind::Identifier) {
          bool hasSemi = false;
          bool hasEq = false;
          for (const auto &t : tokens) {
            if (t.kind != LexToken::Kind::Punct)
              continue;
            if (t.text == ";") {
              hasSemi = true;
              break;
            }
            if (t.text == "=")
              hasEq = true;
          }
          if (hasSemi) {
            const std::string name = tokens[typeIndex + 1].text;
            if (paramNames.find(name) != paramNames.end()) {
              emitHighConfidenceDiagnostic(
                  DiagnosticsRuleKind::SemanticSource, lineIndex,
                  static_cast<int>(tokens[typeIndex + 1].start),
                  static_cast<int>(tokens[typeIndex + 1].end), 2,
                  "Local shadows parameter: " + name + ".");
            } else if (hasDuplicateInCurrentScope(name, currentSig)) {
              emitHighConfidenceDiagnostic(
                  DiagnosticsRuleKind::SemanticSource, lineIndex,
                  static_cast<int>(tokens[typeIndex + 1].start),
                  static_cast<int>(tokens[typeIndex + 1].end), 2,
                  "Duplicate local declaration: " + name + ".");
            }
            const std::string normalizedLocalType =
                normalizeTypeForLine(tokens[typeIndex].text, lineIndex);
            declareLocalInCurrentScope(name, normalizedLocalType, currentSig);
            rebuildVisibleLocals(currentSig);

            for (size_t k = typeIndex + 2; k + 1 < tokens.size(); k++) {
              if (tokens[k].kind == LexToken::Kind::Punct &&
                  tokens[k].text == "=") {
                declarationEqTokenIndex = k;
                std::string lhsType = normalizedLocalType;
                TypeEvalResult rhsEval;
                rhsEval.type = normalizeTypeToken(inferExpressionTypeFromTokens(
                    tokens, k + 1, localsVisibleTypes, uri, text, scanRoots,
                    workspaceFolders, includePaths, scanExtensions,
                    shaderExtensions, defines, structCache, symbolCache,
                    &fileTextCache, &preprocessorView, lineIndex));
                rhsEval.confidence = TypeEvalConfidence::L2;
                if (rhsEval.type.empty() && isHalfFamilyType(lhsType)) {
                  rhsEval.type = inferNarrowingFallbackRhsTypeFromTokens(
                      tokens, k + 1, tokens.size());
                  if (!rhsEval.type.empty()) {
                    rhsEval.confidence = TypeEvalConfidence::L3;
                    rhsEval.reasonCode =
                        IndeterminateReason::DiagnosticsHeavyRulesSkipped;
                  } else {
                    rhsEval.reasonCode =
                        IndeterminateReason::DiagnosticsRhsTypeEmpty;
                    if (!shouldSkipIndeterminateOnIncompleteLine(lineIndex)) {
                      emitIndeterminate(
                          lineIndex, static_cast<int>(tokens[k].start),
                          static_cast<int>(tokens[k].end),
                          "NSF_INDET_RHS_TYPE_EMPTY", rhsEval.reasonCode,
                          "Indeterminate assignment type: rhs type unavailable.");
                    }
                  }
                }
                const std::string rhsType = rhsEval.type;
                emitTypeRelationDiagnostic(
                    lineIndex, static_cast<int>(tokens[k].start),
                    static_cast<int>(tokens[k].end), lhsType, rhsType,
                    "Assignment type mismatch: " + lhsType + " = " + rhsType +
                        ".",
                    relationOptionsForExpressionRange(tokens, k + 1,
                                                       tokens.size()));
                break;
              }
            }
          } else if (hasEq) {
            const std::string name = tokens[typeIndex + 1].text;
            if (paramNames.find(name) != paramNames.end()) {
              emitHighConfidenceDiagnostic(
                  DiagnosticsRuleKind::SemanticSource, lineIndex,
                  static_cast<int>(tokens[typeIndex + 1].start),
                  static_cast<int>(tokens[typeIndex + 1].end), 2,
                  "Local shadows parameter: " + name + ".");
            } else if (hasDuplicateInCurrentScope(name, currentSig)) {
              emitHighConfidenceDiagnostic(
                  DiagnosticsRuleKind::SemanticSource, lineIndex,
                  static_cast<int>(tokens[typeIndex + 1].start),
                  static_cast<int>(tokens[typeIndex + 1].end), 2,
                  "Duplicate local declaration: " + name + ".");
            }
            declareLocalInCurrentScope(
                name, normalizeTypeForLine(tokens[typeIndex].text, lineIndex),
                currentSig);
            rebuildVisibleLocals(currentSig);
            pendingMultilineLocalName = name;
            pendingMultilineLocalDepth = functionBraceDepth;
            pendingMultilineLocalLine = lineIndex;
            pendingMultilineLocalStart =
                static_cast<int>(tokens[typeIndex + 1].start);
            pendingMultilineLocalEnd =
                static_cast<int>(tokens[typeIndex + 1].end);
          }
        }
      }

      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind != LexToken::Kind::Identifier ||
            tokens[i].text != "return")
          continue;
        bool sameLineIfBeforeReturn = false;
        bool sameLineElseBeforeReturn = false;
        for (size_t ti = 0; ti < i; ti++) {
          if (tokens[ti].kind == LexToken::Kind::Punct &&
              tokens[ti].text == ";") {
            sameLineIfBeforeReturn = false;
            sameLineElseBeforeReturn = false;
            continue;
          }
          if (tokens[ti].kind != LexToken::Kind::Identifier)
            continue;
          if (tokens[ti].text == "if")
            sameLineIfBeforeReturn = true;
          else if (tokens[ti].text == "else")
            sameLineElseBeforeReturn = true;
        }
        const bool branchLocalReturn =
            sameLineIfBeforeReturn || sameLineElseBeforeReturn ||
            currentLineControlledByPendingUnbracedBranch;
        const bool branchReturnCompletesIfElse =
            branchLocalReturn &&
            (sameLineElseBeforeReturn ||
             currentLineControlledByPendingUnbracedElse) &&
            !sameLineIfBeforeReturn;
        sawReturn = true;
        if (functionBraceDepth == 1) {
          if (branchLocalReturn) {
            conditionalReturnSeen = true;
            if (branchReturnCompletesIfElse)
              unconditionalReturnSeen = true;
          } else {
            unconditionalReturnSeen = true;
          }
          if (unconditionalReturnSeen)
            sawPotentialMissingReturn = false;
          sawTopLevelReturn = true;
        }
        bool hasValue = true;
        size_t exprStart = i + 1;
        if (exprStart >= tokens.size()) {
          hasValue = false;
        } else if (tokens[exprStart].kind == LexToken::Kind::Punct &&
                   tokens[exprStart].text == ";") {
          hasValue = false;
        }

        const std::string normalizedReturnType =
            normalizeTypeToken(functionReturnType);
        if (normalizedReturnType == "void") {
          if (hasValue) {
            emitHighConfidenceDiagnostic(
                DiagnosticsRuleKind::SemanticSource, lineIndex,
                static_cast<int>(tokens[i].start),
                static_cast<int>(tokens[i].end), 1,
                "Return value in void function.");
          }
          bool hasSemi = false;
          for (const auto &t : tokens) {
            if (t.kind == LexToken::Kind::Punct && t.text == ";") {
              hasSemi = true;
              break;
            }
          }
          if (!hasSemi && shouldReportMissingSemicolonOnLine(lineIndex))
            emitCurrentLineMissingSemicolon(
                static_cast<int>(tokens[i].start),
                static_cast<int>(tokens[i].end));
          if (!branchLocalReturn || branchReturnCompletesIfElse) {
            unreachableActive = true;
            unreachableDepth = functionBraceDepth;
          }
          continue;
        }

        if (!hasValue) {
          emitHighConfidenceDiagnostic(DiagnosticsRuleKind::SemanticSource,
                                       lineIndex,
                                       static_cast<int>(tokens[i].start),
                                       static_cast<int>(tokens[i].end), 1,
                                       "Missing return value.");
          continue;
        }

        std::string exprType = inferExpressionTypeFromTokens(
            tokens, exprStart, localsVisibleTypes, uri, text, scanRoots,
            workspaceFolders, includePaths, scanExtensions, shaderExtensions,
            defines, structCache, symbolCache, &fileTextCache,
            &preprocessorView, lineIndex);
        exprType = normalizeTypeToken(exprType);
        emitTypeRelationDiagnostic(
            lineIndex, static_cast<int>(tokens[i].start),
            static_cast<int>(tokens[i].end), normalizedReturnType, exprType,
            "Return type mismatch: expected " + normalizedReturnType +
                " but got " + exprType + ".",
            relationOptionsForExpressionRange(tokens, exprStart,
                                               tokens.size()));
        bool hasSemi = false;
        for (const auto &t : tokens) {
          if (t.kind == LexToken::Kind::Punct && t.text == ";") {
            hasSemi = true;
            break;
          }
        }
        if (!hasSemi && shouldReportMissingSemicolonOnLine(lineIndex))
          emitCurrentLineMissingSemicolon(static_cast<int>(tokens[i].start),
                                          static_cast<int>(tokens[i].end));
        if (!branchLocalReturn || branchReturnCompletesIfElse) {
          unreachableActive = true;
          unreachableDepth = functionBraceDepth;
        }
      }

      for (size_t i = 0; i + 2 < tokens.size(); i++) {
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        if (tokens[i + 1].kind != LexToken::Kind::Punct ||
            tokens[i + 1].text != "=")
          continue;
        if (declarationEqTokenIndex != std::string::npos &&
            i + 1 == declarationEqTokenIndex)
          continue;
        if (tokens[i + 2].kind == LexToken::Kind::Punct &&
            tokens[i + 2].text == "=")
          continue;
        if (i > 0 && tokens[i - 1].kind == LexToken::Kind::Punct &&
            (tokens[i - 1].text == "." || tokens[i - 1].text == "->" ||
             tokens[i - 1].text == "::"))
          continue;
        auto it = localsVisibleTypes.find(tokens[i].text);
        if (it == localsVisibleTypes.end())
          continue;
        std::string lhsType = normalizeTypeToken(it->second);
        TypeEvalResult rhsEval;
        rhsEval.type = normalizeTypeToken(inferExpressionTypeFromTokens(
            tokens, i + 2, localsVisibleTypes, uri, text, scanRoots,
            workspaceFolders, includePaths, scanExtensions, shaderExtensions,
            defines, structCache, symbolCache, &fileTextCache,
            &preprocessorView, lineIndex));
        rhsEval.confidence = TypeEvalConfidence::L2;
        if (rhsEval.type.empty() && isHalfFamilyType(lhsType)) {
          rhsEval.type = inferNarrowingFallbackRhsTypeFromTokens(tokens, i + 2,
                                                                 tokens.size());
          if (!rhsEval.type.empty()) {
            rhsEval.confidence = TypeEvalConfidence::L3;
            rhsEval.reasonCode =
                IndeterminateReason::DiagnosticsHeavyRulesSkipped;
          } else {
            rhsEval.reasonCode = IndeterminateReason::DiagnosticsRhsTypeEmpty;
            if (!shouldSkipIndeterminateOnIncompleteLine(lineIndex)) {
              emitIndeterminate(
                  lineIndex, static_cast<int>(tokens[i + 1].start),
                  static_cast<int>(tokens[i + 1].end),
                  "NSF_INDET_RHS_TYPE_EMPTY", rhsEval.reasonCode,
                  "Indeterminate assignment type: rhs type unavailable.");
            }
          }
        }
        const std::string rhsType = rhsEval.type;
        emitTypeRelationDiagnostic(
            lineIndex, static_cast<int>(tokens[i + 1].start),
            static_cast<int>(tokens[i + 1].end), lhsType, rhsType,
            "Assignment type mismatch: " + lhsType + " = " + rhsType + ".",
            relationOptionsForExpressionRange(tokens, i + 2, tokens.size()));
        bool hasSemi = false;
        for (const auto &t : tokens) {
          if (t.kind == LexToken::Kind::Punct && t.text == ";") {
            hasSemi = true;
            break;
          }
        }
        if (!hasSemi && shouldReportMissingSemicolonOnLine(lineIndex)) {
          if (!(tokens[i].text == pendingMultilineLocalName &&
                pendingMultilineLocalDepth == functionBraceDepth)) {
            emitCurrentLineMissingSemicolon(
                static_cast<int>(tokens[i + 1].start),
                static_cast<int>(tokens[i + 1].end));
          }
        }
      }

      for (size_t i = 0; i + 2 < tokens.size(); i++) {
        auto inferOperandType = [&](size_t index) -> std::string {
          if (index >= tokens.size())
            return "";

          auto skipBalancedAt = [&](size_t start, const std::string &open,
                                    const std::string &close) -> size_t {
            if (start >= tokens.size() ||
                tokens[start].kind != LexToken::Kind::Punct ||
                tokens[start].text != open) {
              return start;
            }
            int depth = 0;
            size_t cursor = start;
            while (cursor < tokens.size()) {
              if (tokens[cursor].kind == LexToken::Kind::Punct) {
                if (tokens[cursor].text == open)
                  depth++;
                else if (tokens[cursor].text == close) {
                  depth--;
                  if (depth == 0)
                    return cursor + 1;
                }
              }
              cursor++;
            }
            return tokens.size();
          };

          auto applyPostfixType = [&](size_t cursor,
                                      std::string baseType) -> std::string {
            std::string current = normalizeTypeToken(baseType);
            while (cursor < tokens.size()) {
              const LexToken &tok = tokens[cursor];
              if (tok.kind == LexToken::Kind::Punct && tok.text == "[") {
                cursor = skipBalancedAt(cursor, "[", "]");
                current = applyIndexAccessType(current);
                continue;
              }
              if (tok.kind == LexToken::Kind::Punct && tok.text == "." &&
                  cursor + 1 < tokens.size() &&
                  tokens[cursor + 1].kind == LexToken::Kind::Identifier) {
                cursor += 2;
                if (isSwizzleToken(tokens[cursor - 1].text)) {
                  const std::string swizzled =
                      applySwizzleType(current, tokens[cursor - 1].text);
                  if (!swizzled.empty())
                    current = swizzled;
                } else {
                  return "";
                }
                continue;
              }
              break;
            }
            return current;
          };

          NumericLiteralParseResult numeric =
              parseNumericLiteralFromTokens(tokens, index);
          if (numeric.matched && !numeric.type.empty())
            return applyPostfixType(
                index + std::max<size_t>(numeric.tokenSpan, 1),
                numeric.type);
          if (tokens[index].kind == LexToken::Kind::Identifier) {
            int dim = 0;
            int rows = 0;
            int cols = 0;
            if (isVectorType(tokens[index].text, dim) ||
                isScalarType(tokens[index].text) ||
                isMatrixType(tokens[index].text, rows, cols))
              return applyPostfixType(index + 1, tokens[index].text);
            auto it = localsVisibleTypes.find(tokens[index].text);
            if (it != localsVisibleTypes.end())
              return applyPostfixType(index + 1, it->second);
            return applyPostfixType(index + 1,
                                    inferLiteralType(tokens[index].text));
          }
          return applyPostfixType(
              index + 1,
              normalizeTypeToken(inferLiteralType(tokens[index].text)));
        };

        std::string leftType = normalizeTypeToken(inferOperandType(i));
        if (leftType.empty())
          continue;
        if (tokens[i + 1].kind != LexToken::Kind::Punct)
          continue;
        const std::string op = tokens[i + 1].text;
        if (op != "+" && op != "-" && op != "*" && op != "/" && op != "==" &&
            op != "!=" && op != "<" && op != ">" && op != "<=" && op != ">=" &&
            op != "&&" && op != "||")
          continue;
        std::string rightType = normalizeTypeToken(inferOperandType(i + 2));
        if (rightType.empty())
          continue;

        const bool isLogicalOperator = op == "&&" || op == "||";
        if (isLogicalOperator) {
          TypeRelationResult leftBool =
              relationForTypes("bool", leftType);
          TypeRelationResult rightBool =
              relationForTypes("bool", rightType);
          if (!leftBool.viable || !rightBool.viable) {
            emitHighConfidenceDiagnostic(
                DiagnosticsRuleKind::ExpressionType, lineIndex,
                static_cast<int>(tokens[i + 1].start),
                static_cast<int>(tokens[i + 1].end), 1,
                "Binary operator type mismatch: " + leftType + " " + op +
                    " " + rightType + ".");
            continue;
          }
          emitRelationWarning(lineIndex,
                              static_cast<int>(tokens[i + 1].start),
                              static_cast<int>(tokens[i + 1].end), leftBool);
          emitRelationWarning(lineIndex,
                              static_cast<int>(tokens[i + 1].start),
                              static_cast<int>(tokens[i + 1].end), rightBool);
          continue;
        }

        int leftDim = 0;
        int rightDim = 0;
        bool leftVec = isVectorType(leftType, leftDim);
        bool rightVec = isVectorType(rightType, rightDim);
        int leftRows = 0;
        int leftCols = 0;
        int rightRows = 0;
        int rightCols = 0;
        bool leftMat = isMatrixType(leftType, leftRows, leftCols);
        bool rightMat = isMatrixType(rightType, rightRows, rightCols);
        const bool isComparisonOperator =
            op == "==" || op == "!=" || op == "<" || op == ">" ||
            op == "<=" || op == ">=";
        if (leftMat || rightMat) {
          bool ok = false;
          if (op == "+" || op == "-") {
            ok = (leftMat && rightMat) ||
                 (leftMat && isNumericScalarType(rightType)) ||
                 (rightMat && isNumericScalarType(leftType));
          } else if (op == "*") {
            ok = (leftMat && rightMat && leftCols == rightRows) ||
                 (leftMat && isNumericScalarType(rightType)) ||
                 (rightMat && isNumericScalarType(leftType)) ||
                 (leftVec && rightMat && leftDim == rightRows) ||
                 (leftMat && rightVec && leftCols == rightDim);
          } else if (op == "/") {
            ok = (leftMat && isNumericScalarType(rightType));
          } else if (isComparisonOperator) {
            ok = leftMat && rightMat;
          }
          if (!ok) {
            emitHighConfidenceDiagnostic(
                DiagnosticsRuleKind::ExpressionType, lineIndex,
                static_cast<int>(tokens[i + 1].start),
                static_cast<int>(tokens[i + 1].end), 1,
                "Binary operator type mismatch: " + leftType + " " + op + " " +
                    rightType + ".");
            continue;
          }
          TypeBinaryConversionResult conversion =
              evaluateUsualArithmeticConversion(parseTypeDesc(leftType),
                                                parseTypeDesc(rightType));
          if (conversion.viable) {
            emitRelationWarning(lineIndex,
                                static_cast<int>(tokens[i + 1].start),
                                static_cast<int>(tokens[i + 1].end),
                                conversion.leftConversion);
            emitRelationWarning(lineIndex,
                                static_cast<int>(tokens[i + 1].start),
                                static_cast<int>(tokens[i + 1].end),
                                conversion.rightConversion);
          }
          continue;
        }

        if (op == "+" || op == "-" || op == "*" || op == "/" ||
            isComparisonOperator) {
          TypeBinaryConversionResult conversion =
              evaluateUsualArithmeticConversion(parseTypeDesc(leftType),
                                                parseTypeDesc(rightType));
          if (!conversion.viable) {
            emitHighConfidenceDiagnostic(
                DiagnosticsRuleKind::ExpressionType, lineIndex,
                static_cast<int>(tokens[i + 1].start),
                static_cast<int>(tokens[i + 1].end), 1,
                "Binary operator type mismatch: " + leftType + " " + op + " " +
                    rightType + ".");
            continue;
          }
          emitRelationWarning(lineIndex, static_cast<int>(tokens[i + 1].start),
                              static_cast<int>(tokens[i + 1].end),
                              conversion.leftConversion);
          emitRelationWarning(lineIndex, static_cast<int>(tokens[i + 1].start),
                              static_cast<int>(tokens[i + 1].end),
                              conversion.rightConversion);
        }
      }

      int builtinAttributeDepth = 0;
      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "[") {
          builtinAttributeDepth++;
          continue;
        }
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "]") {
          if (builtinAttributeDepth > 0)
            builtinAttributeDepth--;
          continue;
        }
        if (builtinAttributeDepth > 0)
          continue;
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        if (i + 1 >= tokens.size() ||
            tokens[i + 1].kind != LexToken::Kind::Punct ||
            tokens[i + 1].text != "(")
          continue;
        if (i > 0 && tokens[i - 1].kind == LexToken::Kind::Punct &&
            (tokens[i - 1].text == "." || tokens[i - 1].text == "->" ||
             tokens[i - 1].text == "::"))
          continue;
        const std::string name = tokens[i].text;
        int dim = 0;
        int rows = 0;
        int cols = 0;
        if (!isHlslBuiltinFunction(name))
          continue;
        if (isLikelyTypeConstructorCallName(name) || isVectorType(name, dim) ||
            isScalarType(name) || isMatrixType(name, rows, cols))
          continue;

        std::vector<std::pair<size_t, size_t>> argRanges;
        size_t closeParenIndex = std::string::npos;
        if (!collectCallArgumentTokenRanges(tokens, i + 1, argRanges,
                                            closeParenIndex)) {
          continue;
        }
        std::vector<std::string> argTypes;
        bool hasEmptyArgument = false;
        for (const auto &range : argRanges) {
          if (range.first >= range.second) {
            hasEmptyArgument = true;
            argTypes.push_back("");
            continue;
          }
          argTypes.push_back(
              normalizeTypeToken(inferExpressionTypeFromTokensRange(
                  tokens, range.first, range.second, localsVisibleTypes, uri,
                  text, scanRoots, workspaceFolders, includePaths,
                  scanExtensions, shaderExtensions, defines, structCache,
                  symbolCache, &fileTextCache, &preprocessorView,
                  lineIndex)));
        }
        if (closeParenIndex == std::string::npos) {
          continue;
        }

        if (!isHlslBuiltinTypeCheckedFunction(name)) {
          emitIndeterminate(
              lineIndex, static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end), "NSF_INDET_BUILTIN_UNMODELED",
              IndeterminateReason::DiagnosticsBuiltinUnmodeled,
              "Indeterminate builtin call: type rules not implemented. Name: " +
                  name + ". Args: " + formatTypeList(argTypes) + ".");
          continue;
        }

        std::vector<BuiltinTypeInfo> infos;
        infos.reserve(argTypes.size());
        if (hasEmptyArgument) {
          emitHighConfidenceDiagnostic(
              DiagnosticsRuleKind::CallType, lineIndex,
              static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end), 1,
              "Builtin call type mismatch: " + name +
                  ". Args: " + formatTypeList(argTypes) + ".");
          continue;
        }
        for (const auto &t : argTypes) {
          infos.push_back(parseBuiltinTypeInfo(t));
        }
        BuiltinResolveResult rr = resolveBuiltinCall(name, infos);
        if (rr.indeterminate) {
          emitIndeterminate(
              lineIndex, static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end),
              "NSF_INDET_BUILTIN_ARG_TYPE_UNKNOWN",
              IndeterminateReason::DiagnosticsBuiltinArgTypeUnknown,
              "Indeterminate builtin call: arg types unavailable. Name: " +
                  name + ". Args: " + formatTypeList(argTypes) + ".");
        } else if (!rr.ok) {
          emitHighConfidenceDiagnostic(
              DiagnosticsRuleKind::CallType, lineIndex,
              static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end), 1,
              "Builtin call type mismatch: " + name +
                  ". Args: " + formatTypeList(argTypes) + ".");
        } else {
          for (const auto &relation : rr.conversions) {
            emitRelationWarning(lineIndex, static_cast<int>(tokens[i].start),
                                static_cast<int>(tokens[i].end), relation);
          }
        }
      }

      int methodCallAttributeDepth = 0;
      for (size_t i = 0; i + 3 < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "[") {
          methodCallAttributeDepth++;
          continue;
        }
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "]") {
          if (methodCallAttributeDepth > 0)
            methodCallAttributeDepth--;
          continue;
        }
        if (methodCallAttributeDepth > 0)
          continue;
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        if (tokens[i + 1].kind != LexToken::Kind::Punct ||
            tokens[i + 1].text != ".")
          continue;
        if (tokens[i + 2].kind != LexToken::Kind::Identifier)
          continue;
        if (tokens[i + 3].kind != LexToken::Kind::Punct ||
            tokens[i + 3].text != "(")
          continue;

        const std::string baseName = tokens[i].text;
        const std::string baseType =
            normalizeTypeToken(resolveVisibleSymbolType(
                baseName, localsVisibleTypes, text, symbolCache));
        if (baseType.empty())
          continue;
        std::string baseFamily;
        if (!getTypeModelObjectFamily(baseType, baseFamily))
          continue;

        const std::string member = tokens[i + 2].text;
        std::vector<std::pair<size_t, size_t>> argRanges;
        size_t closeParenIndex = std::string::npos;
        if (!collectCallArgumentTokenRanges(tokens, i + 3, argRanges,
                                            closeParenIndex)) {
          continue;
        }
        std::vector<std::string> argTypes;
        bool hasEmptyArgument = false;
        for (const auto &range : argRanges) {
          if (range.first >= range.second) {
            hasEmptyArgument = true;
            argTypes.push_back("");
            continue;
          }
          argTypes.push_back(
              normalizeTypeToken(inferExpressionTypeFromTokensRange(
                  tokens, range.first, range.second, localsVisibleTypes, uri,
                  text, scanRoots, workspaceFolders, includePaths,
                  scanExtensions, shaderExtensions, defines, structCache,
                  symbolCache, &fileTextCache, &preprocessorView,
                  lineIndex)));
        }
        if (closeParenIndex == std::string::npos)
          continue;

        auto anyArgUnknown = [&]() -> bool {
          for (const auto &t : argTypes) {
            if (t.empty())
              return true;
          }
          return false;
        };

        auto emitUnmodeled = [&]() {
          emitIndeterminate(
              lineIndex, static_cast<int>(tokens[i + 2].start),
              static_cast<int>(tokens[i + 2].end),
              "NSF_INDET_BUILTIN_METHOD_UNMODELED",
              IndeterminateReason::DiagnosticsBuiltinMethodUnmodeled,
              "Indeterminate built-in method call: type rules not implemented. "
              "Base: " +
                  baseType + ". Method: " + member +
                  ". Args: " + formatTypeList(argTypes) + ".");
        };

        auto emitArgUnknown = [&]() {
          emitIndeterminate(
              lineIndex, static_cast<int>(tokens[i + 2].start),
              static_cast<int>(tokens[i + 2].end),
              "NSF_INDET_BUILTIN_METHOD_ARG_TYPE_UNKNOWN",
              IndeterminateReason::DiagnosticsBuiltinMethodArgTypeUnknown,
              "Indeterminate built-in method call: arg types unavailable. "
              "Base: " +
                  baseType + ". Method: " + member +
                  ". Args: " + formatTypeList(argTypes) + ".");
        };

        auto emitMismatch = [&]() {
          emitHighConfidenceDiagnostic(
              DiagnosticsRuleKind::CallType, lineIndex,
              static_cast<int>(tokens[i + 2].start),
              static_cast<int>(tokens[i + 2].end), 1,
              "Built-in method call type mismatch: " + member + ". Base: " +
                  baseType + ". Args: " + formatTypeList(argTypes) + ".");
        };

        HlslBuiltinMethodRule methodRule;
        if (!lookupHlslBuiltinMethodRule(member, baseType, methodRule)) {
          emitUnmodeled();
          continue;
        }
        if (hasEmptyArgument) {
          emitMismatch();
          continue;
        }
        if (anyArgUnknown()) {
          emitArgUnknown();
          continue;
        }
        if (argTypes.size() < static_cast<size_t>(methodRule.minArgs) ||
            argTypes.size() > static_cast<size_t>(methodRule.maxArgs)) {
          emitMismatch();
          continue;
        }
        if (member == "GetDimensions") {
          continue;
        }

        if (methodRule.parameterTypes.empty()) {
          emitUnmodeled();
          continue;
        }

        std::vector<CandidateSignature> methodCandidates;
        methodCandidates.reserve(methodRule.parameterTypes.size());
        for (const auto &paramTypes : methodRule.parameterTypes) {
          if (paramTypes.empty())
            continue;
          const size_t comparedCount =
              std::min(argTypes.size(), paramTypes.size());
          CandidateSignature candidate;
          candidate.name = member;
          candidate.displayLabel = member;
          candidate.displayParams.assign(paramTypes.begin(),
                                         paramTypes.begin() + comparedCount);
          candidate.params.reserve(comparedCount);
          for (size_t argIndex = 0; argIndex < comparedCount; argIndex++) {
            ParamDesc param;
            param.type = parseTypeDesc(paramTypes[argIndex]);
            candidate.params.push_back(std::move(param));
          }
          methodCandidates.push_back(std::move(candidate));
        }
        if (methodCandidates.empty()) {
          emitUnmodeled();
          continue;
        }

        size_t comparedArgCount = argTypes.size();
        for (const auto &candidate : methodCandidates) {
          comparedArgCount =
              std::min(comparedArgCount, candidate.params.size());
        }
        std::vector<TypeDesc> methodArgTypes;
        methodArgTypes.reserve(comparedArgCount);
        for (size_t argIndex = 0; argIndex < comparedArgCount; argIndex++)
          methodArgTypes.push_back(parseTypeDesc(argTypes[argIndex]));

        ResolveCallContext methodResolveContext;
        methodResolveContext.enableVisibilityFiltering = false;
        ResolveCallResult methodResolveResult =
            resolveCallCandidates(methodCandidates, methodArgTypes,
                                  methodResolveContext);
        if (methodResolveResult.status == ResolveCallStatus::Resolved ||
            methodResolveResult.status == ResolveCallStatus::Ambiguous) {
          if (!methodResolveResult.rankedCandidates.empty()) {
            for (const auto &relation :
                 methodResolveResult.rankedCandidates.front().perArgRelations) {
              emitRelationWarning(lineIndex,
                                  static_cast<int>(tokens[i + 2].start),
                                  static_cast<int>(tokens[i + 2].end),
                                  relation);
            }
          }
          continue;
        }

        emitMismatch();
      }

      int userCallAttributeDepth = 0;
      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "[") {
          userCallAttributeDepth++;
          continue;
        }
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "]") {
          if (userCallAttributeDepth > 0)
            userCallAttributeDepth--;
          continue;
        }
        if (userCallAttributeDepth > 0)
          continue;
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        if (i + 1 >= tokens.size() ||
            tokens[i + 1].kind != LexToken::Kind::Punct ||
            tokens[i + 1].text != "(")
          continue;
        if (i > 0 && tokens[i - 1].kind == LexToken::Kind::Punct &&
            (tokens[i - 1].text == "." || tokens[i - 1].text == "->" ||
             tokens[i - 1].text == "::"))
          continue;
        const std::string name = tokens[i].text;
        if (isBuiltinOrKeyword(name))
          continue;
        if (isHlslBuiltinTypeCheckedFunction(name))
          continue;
        int dim = 0;
        int rows = 0;
        int cols = 0;
        if (isLikelyTypeConstructorCallName(name) || isVectorType(name, dim) ||
            isScalarType(name) || isMatrixType(name, rows, cols))
          continue;
        std::vector<std::pair<size_t, size_t>> argRanges;
        size_t closeParenIndex = std::string::npos;
        if (!collectCallArgumentTokenRanges(tokens, i + 1, argRanges,
                                            closeParenIndex)) {
          continue;
        }
        std::vector<std::string> argTypes;
        for (const auto &range : argRanges) {
          argTypes.push_back(
              normalizeTypeToken(inferExpressionTypeFromTokensRange(
                  tokens, range.first, range.second, localsVisibleTypes, uri,
                  text, scanRoots, workspaceFolders, includePaths,
                  scanExtensions, shaderExtensions, defines, structCache,
                  symbolCache, &fileTextCache, &preprocessorView,
                  lineIndex)));
        }
        if (closeParenIndex == std::string::npos)
          continue;

        const auto &candidates = getUserFunctionCandidates(name);
        if (candidates.empty())
          continue;

        std::vector<CandidateSignature> resolverCandidates;
        resolverCandidates.reserve(candidates.size());
        for (const auto &cand : candidates) {
          CandidateSignature resolverCandidate;
          resolverCandidate.name = name;
          resolverCandidate.displayLabel = cand.label;
          resolverCandidate.displayParams = cand.paramTypes;
          resolverCandidate.sourceUri = cand.loc.uri;
          resolverCandidate.sourceLine = cand.loc.line;
          resolverCandidate.visibilityCondition = "";
          resolverCandidate.params.reserve(cand.paramTypes.size());
          for (const auto &paramType : cand.paramTypes) {
            ParamDesc paramDesc;
            paramDesc.type = parseTypeDesc(paramType);
            resolverCandidate.params.push_back(std::move(paramDesc));
          }
          resolverCandidates.push_back(std::move(resolverCandidate));
        }
        std::vector<TypeDesc> resolverArgTypes;
        resolverArgTypes.reserve(argTypes.size());
        for (const auto &argType : argTypes)
          resolverArgTypes.push_back(parseTypeDesc(argType));
        ResolveCallContext resolveContext;
        resolveContext.defines = defines;
        resolveContext.allowNarrowing = false;
        resolveContext.enableVisibilityFiltering = false;
        ResolveCallResult resolveResult = resolveCallCandidates(
            resolverCandidates, resolverArgTypes, resolveContext);
        if (resolveResult.status == ResolveCallStatus::Resolved ||
            resolveResult.status == ResolveCallStatus::Ambiguous) {
          if (!resolveResult.rankedCandidates.empty()) {
            for (const auto &relation :
                 resolveResult.rankedCandidates.front().perArgRelations) {
              emitRelationWarning(lineIndex, static_cast<int>(tokens[i].start),
                                  static_cast<int>(tokens[i].end), relation);
            }
          }
          continue;
        }

        bool anyArityMatch = false;
        bool anyPerfect = false;
        int bestMismatches = 1000000;
        int bestCompared = -1;
        int bestScore = -1;
        size_t bestIndex = 0;
        for (size_t ci = 0; ci < candidates.size(); ci++) {
          const auto &cand = candidates[ci];
          if (cand.paramTypes.size() != argTypes.size())
            continue;
          anyArityMatch = true;
          int mismatches = 0;
          int compared = 0;
          int score = 0;
          for (size_t k = 0; k < argTypes.size() && k < cand.paramTypes.size();
               k++) {
            const std::string &expected = cand.paramTypes[k];
            const std::string &actual = argTypes[k];
            if (expected.empty() || actual.empty())
              continue;
            compared++;
            TypeRelationResult relation = relationForTypes(expected, actual);
            if (relation.viable) {
              if (expected == actual)
                score += 3;
              else
                score += 1;
            } else {
              mismatches++;
            }
          }
          if (mismatches == 0) {
            anyPerfect = true;
            break;
          }
          if (mismatches < bestMismatches ||
              (mismatches == bestMismatches && compared > bestCompared) ||
              (mismatches == bestMismatches && compared == bestCompared &&
               score > bestScore)) {
            bestMismatches = mismatches;
            bestCompared = compared;
            bestScore = score;
            bestIndex = ci;
          }
        }

        if (anyPerfect)
          continue;

        auto buildDisplayTypes = [&](const std::vector<std::string> &types) {
          std::vector<std::string> out;
          out.reserve(types.size());
          for (const auto &t : types)
            out.push_back(t.empty() ? "?" : t);
          return out;
        };

        auto emitLowConfidenceFunctionIndeterminate =
            [&](const UserFunctionCandidate &best, const std::string &detail) {
              emitIndeterminate(
                  lineIndex, static_cast<int>(tokens[i].start),
                  static_cast<int>(tokens[i].end),
                  "NSF_INDET_FUNCTION_SIGNATURE_LOW_CONFIDENCE",
                  IndeterminateReason::
                      DiagnosticsFunctionSignatureLowConfidence,
                  "Indeterminate function call analysis: low-confidence "
                  "signature source for " +
                      name + ". " + detail + " Candidate at " +
                      formatLocationShort(best.loc) + ".");
            };

        if (anyArityMatch) {
          if (bestCompared <= 0)
            continue;
          const auto &best = candidates[bestIndex];
          if (best.confidence == FunctionCandidateConfidence::TextFallback &&
              best.loc.line == lineIndex) {
            std::string detail =
                "Expected " +
                formatTypeList(buildDisplayTypes(best.paramTypes)) + ", got " +
                formatTypeList(buildDisplayTypes(argTypes)) + ".";
            emitLowConfidenceFunctionIndeterminate(best, detail);
            continue;
          }
          std::string message =
              "Function call argument mismatch: " + name + ". Expected: " +
              formatTypeList(buildDisplayTypes(best.paramTypes)) +
              ". Got: " + formatTypeList(buildDisplayTypes(argTypes)) +
              ". Defined at: " + formatLocationShort(best.loc) + ".";
          emitHighConfidenceDiagnostic(
              DiagnosticsRuleKind::CallType, lineIndex,
              static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end), 1, message);
          continue;
        }

        const auto &best = candidates.front();
        if (best.confidence == FunctionCandidateConfidence::TextFallback &&
            best.loc.line == lineIndex) {
          std::ostringstream expectedCount;
          expectedCount << best.paramTypes.size();
          std::string detail = "Expected " + expectedCount.str() +
                               " argument(s), got " +
                               std::to_string(argTypes.size()) + ".";
          emitLowConfidenceFunctionIndeterminate(best, detail);
          continue;
        }
        std::ostringstream expectedCount;
        expectedCount << best.paramTypes.size();
        std::string message = "Function call argument count mismatch: " + name +
                              ". Expected " + expectedCount.str() +
                              " but got " + std::to_string(argTypes.size()) +
                              ". Defined at: " + formatLocationShort(best.loc) +
                              ".";
        emitHighConfidenceDiagnostic(
            DiagnosticsRuleKind::CallType, lineIndex,
            static_cast<int>(tokens[i].start),
            static_cast<int>(tokens[i].end), 1, message);
      }

      int attributeDepth = 0;
      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "[") {
          attributeDepth++;
          continue;
        }
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "]") {
          if (attributeDepth > 0)
            attributeDepth--;
          continue;
        }
        if (attributeDepth > 0)
          continue;
        if (tokens[i].kind != LexToken::Kind::Identifier &&
            !(tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "."))
          continue;
        NumericLiteralParseResult numeric =
            parseNumericLiteralFromTokens(tokens, i);
        if (numeric.matched) {
          if (!numeric.deprecatedSuffix.empty() &&
              numeric.deprecatedSuffixTokenIndex < tokens.size()) {
            const LexToken &suffixToken =
                tokens[numeric.deprecatedSuffixTokenIndex];
            diags.a.push_back(makeDiagnostic(
                text, lineIndex, static_cast<int>(suffixToken.start),
                static_cast<int>(suffixToken.end), 2, "nsf",
                std::string("Deprecated numeric literal suffix: ") +
                    numeric.deprecatedSuffix + ". Use " +
                    numeric.recommendedSuffix + "."));
          }
          if (!numeric.valid && numeric.invalidSuffix != 0 &&
              numeric.invalidSuffixTokenIndex < tokens.size()) {
            const LexToken &suffixToken = tokens[numeric.invalidSuffixTokenIndex];
            diags.a.push_back(makeDiagnostic(
                text, lineIndex, static_cast<int>(suffixToken.start),
                static_cast<int>(suffixToken.end), 1, "nsf",
                std::string("Invalid numeric literal suffix: ") +
                    numeric.invalidSuffix +
                    "."));
          }
          if (numeric.tokenSpan > 0)
            i += numeric.tokenSpan - 1;
          continue;
        }
        if (tokens[i].kind == LexToken::Kind::Punct)
          continue;
        const std::string word = tokens[i].text;
        if (!inferLiteralType(word).empty())
          continue;
        if (isBuiltinOrKeyword(word))
          continue;
        if (word.rfind("SasUi", 0) == 0)
          continue;
        if (isHlslSystemSemantic(word))
          continue;
        if (i > 0 && tokens[i - 1].kind == LexToken::Kind::Punct &&
            (tokens[i - 1].text == "." || tokens[i - 1].text == "->" ||
             tokens[i - 1].text == "::" || tokens[i - 1].text == ":" ||
             tokens[i - 1].text == "#"))
          continue;
        if (i + 1 < tokens.size() &&
            tokens[i + 1].kind == LexToken::Kind::Punct &&
            tokens[i + 1].text == ":")
          continue;
        if (i + 1 < tokens.size() &&
            tokens[i + 1].kind == LexToken::Kind::Punct &&
            tokens[i + 1].text == "(")
          continue;
        if (isKnownSymbol(word))
          continue;
        if (isExternallyResolvable(word))
          continue;
        emitHighConfidenceDiagnostic(
            DiagnosticsRuleKind::UndefinedIdentifier, lineIndex,
            static_cast<int>(tokens[i].start),
            static_cast<int>(tokens[i].end), 2,
            "Undefined identifier: " + word + ".");
      }

      if (consumePendingUnbracedBranch) {
        pendingUnbracedBranchLineByDepth.erase(functionBraceDepth);
        pendingUnbracedBranchIsElseByDepth.erase(functionBraceDepth);
      }
      if (isUnbracedBranchControlHeaderLine(tokens)) {
        pendingUnbracedBranchLineByDepth[functionBraceDepth] = lineIndex;
        pendingUnbracedBranchIsElseByDepth[functionBraceDepth] =
            isUnbracedElseControlHeaderLine(tokens);
      }
    }

    if (inFunction && normalizeTypeToken(functionReturnType) != "void" &&
        sawReturn) {
      if (conditionalReturnSeen && !unconditionalReturnSeen) {
        sawPotentialMissingReturn = true;
      } else if (unconditionalReturnSeen) {
        sawPotentialMissingReturn = false;
      }
    }

    if (!lineReportedMissingSemicolon && diags.a.size() < maxDiagnostics &&
        lineIndex >= 0 &&
        lineIndex < static_cast<int>(trimmedCodeLines.size())) {
      const bool currentLineIsPendingMultilineLocal =
          !pendingMultilineLocalName.empty() &&
          pendingMultilineLocalLine == lineIndex &&
          pendingMultilineLocalDepth == functionBraceDepth;
      if (!currentLineIsPendingMultilineLocal) {
        const std::string &trimmed = trimmedCodeLines[lineIndex];
        const std::string nextTrimmed = nextTrimmedCodeLine(lineIndex);
        const bool insideOpenGroupingBeforeLine =
            (lineIndex <
                 static_cast<int>(lineScan.parenDepthBeforeLine.size()) &&
             lineScan.parenDepthBeforeLine[lineIndex] > 0) ||
            (lineIndex <
                 static_cast<int>(lineScan.bracketDepthBeforeLine.size()) &&
             lineScan.bracketDepthBeforeLine[lineIndex] > 0);
        const bool insideOpenGroupingAfterLine =
            (lineIndex < static_cast<int>(lineScan.parenDepthAfterLine.size()) &&
             lineScan.parenDepthAfterLine[lineIndex] > 0) ||
            (lineIndex <
                 static_cast<int>(lineScan.bracketDepthAfterLine.size()) &&
             lineScan.bracketDepthAfterLine[lineIndex] > 0);
        if (shouldReportMissingSemicolonShared(
                previousTrimmedCodeLine(lineIndex), trimmed, nextTrimmed,
                insideOpenGroupingBeforeLine, insideOpenGroupingAfterLine)) {
          size_t endByte = lineText.find_last_not_of(" \t");
          if (endByte == std::string::npos)
            endByte = trimmed.empty() ? 0 : trimmed.size() - 1;
          emitCurrentLineMissingSemicolon(static_cast<int>(endByte),
                                          static_cast<int>(endByte + 1));
        }
      }
    }

    scanForBraces(lineText);
    pendingControlOpenBlockKind =
        nextOpenBraceKinds.empty() ? leadingControlKindForLine(tokens)
                                   : FlowBlockKind::Other;
    if (inFunction && !pendingForScopes.empty() && !tokens.empty()) {
      for (auto it = pendingForScopes.begin(); it != pendingForScopes.end();) {
        if (it->createdLine < lineIndex) {
          if (!localScopeStack.empty() && localScopeStack.back() == it->scopeId)
            localScopeStack.pop_back();
          it = pendingForScopes.erase(it);
        } else {
          ++it;
        }
      }
    }
    lineIndex++;
  }
}
