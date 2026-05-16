#include "semantic_snapshot.hpp"

#include "active_unit.hpp"
#include "expanded_source.hpp"
#include "hlsl_ast.hpp"
#include "nsf_lexer.hpp"
#include "preprocessor_view.hpp"
#include "server_parse.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace {

static std::string
makeDefinesFingerprint(const std::unordered_map<std::string, int> &defines) {
  std::vector<std::pair<std::string, int>> ordered(defines.begin(),
                                                   defines.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  std::ostringstream oss;
  for (const auto &entry : ordered) {
    oss << entry.first.size() << ":" << entry.first << "=" << entry.second
        << ";";
  }
  oss << "preset=" << getConfiguredPreprocessorMacrosFingerprint() << ";";
  return oss.str();
}

static SemanticCacheKey makeSemanticSnapshotCacheKey(
    const std::string &uri, const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &unitPath = std::string(),
    const std::string &analysisContextFingerprint = std::string()) {
  SemanticCacheKey key;
  key.workspaceFolders = workspaceFolders;
  key.includePaths = includePaths;
  key.shaderExtensions = shaderExtensions;
  key.definesFingerprint = makeDefinesFingerprint(defines);
  const std::string activeUnitPath = getActiveUnitPath();
  if (!unitPath.empty()) {
    key.unitPath = unitPath;
  } else {
    key.unitPath = activeUnitPath.empty() ? uriToPath(uri) : activeUnitPath;
  }
  key.analysisContextFingerprint = analysisContextFingerprint;
  return key;
}

static void populateSemanticSnapshotFromAst(
    const HlslAstDocument &document, SemanticSnapshot &snapshot) {
  snapshot.functions.clear();
  snapshot.functionsByName.clear();
  snapshot.structs.clear();
  snapshot.structByName.clear();
  snapshot.globals.clear();
  snapshot.globalByName.clear();
  snapshot.cbuffers.clear();
  snapshot.cbufferByName.clear();

  snapshot.functions.reserve(document.functions.size());
  for (const auto &function : document.functions) {
    SemanticSnapshot::FunctionInfo info;
    info.name = function.name;
    info.line = function.line;
    info.character = function.character;
    info.label = function.label;
    info.returnType = function.returnType;
    info.signatureEndLine = function.signatureEndLine;
    info.bodyStartLine = function.bodyStartLine;
    info.bodyEndLine = function.bodyEndLine;
    info.hasBody = function.hasBody;
    info.parameters.reserve(function.parameters.size());
    info.parameterInfos.reserve(function.parameters.size());
    info.parameterDetails.reserve(function.parameters.size());
    for (const auto &parameter : function.parameters) {
      info.parameters.push_back(parameter.text);
      info.parameterInfos.push_back({parameter.name, parameter.type});
      SemanticSnapshot::FunctionInfo::ParameterInfo parameterInfo;
      parameterInfo.name = parameter.name;
      parameterInfo.type = parameter.type;
      parameterInfo.line = parameter.line;
      parameterInfo.character = parameter.character;
      parameterInfo.offset = parameter.offset;
      info.parameterDetails.push_back(std::move(parameterInfo));
    }
    const size_t index = snapshot.functions.size();
    snapshot.functions.push_back(std::move(info));
    snapshot.functionsByName[function.name].push_back(index);
  }

  snapshot.structs.reserve(document.structs.size());
  for (const auto &decl : document.structs) {
    SemanticSnapshot::StructInfo info;
    info.name = decl.name;
    info.line = decl.line;
    info.fields.reserve(decl.fields.size());
    for (const auto &field : decl.fields) {
      SemanticSnapshot::FieldInfo fieldInfo;
      fieldInfo.name = field.name;
      fieldInfo.type = field.type;
      fieldInfo.line = field.line;
      fieldInfo.character = field.character;
      fieldInfo.offset = field.offset;
      info.fields.push_back(std::move(fieldInfo));
    }
    const size_t index = snapshot.structs.size();
    snapshot.structs.push_back(std::move(info));
    snapshot.structByName[decl.name] = index;
  }

  snapshot.globals.reserve(document.globalVariables.size());
  for (const auto &decl : document.globalVariables) {
    SemanticSnapshot::GlobalInfo info;
    info.name = decl.name;
    info.type = decl.type;
    info.line = decl.line;
    info.character = decl.character;
    info.offset = decl.offset;
    const size_t index = snapshot.globals.size();
    snapshot.globals.push_back(std::move(info));
    snapshot.globalByName[decl.name] = index;
  }

  snapshot.cbuffers.reserve(document.cbuffers.size());
  for (const auto &decl : document.cbuffers) {
    SemanticSnapshot::CBufferInfo info;
    info.name = decl.name;
    info.line = decl.line;
    info.fields.reserve(decl.fields.size());
    for (const auto &field : decl.fields) {
      SemanticSnapshot::FieldInfo fieldInfo;
      fieldInfo.name = field.name;
      fieldInfo.type = field.type;
      fieldInfo.line = field.line;
      fieldInfo.character = field.character;
      fieldInfo.offset = field.offset;
      info.fields.push_back(std::move(fieldInfo));
    }
    const size_t index = snapshot.cbuffers.size();
    snapshot.cbuffers.push_back(std::move(info));
    snapshot.cbufferByName[decl.name] = index;
  }

  snapshot.semanticDataComplete = true;
}

static void populateSemanticSnapshotLocals(const std::string &text,
                                           SemanticSnapshot &snapshot) {
  const std::vector<std::string> lines = splitLinesShared(text);
  std::vector<size_t> lineStarts;
  lineStarts.reserve(lines.size() + 1);
  size_t offset = 0;
  for (const auto &line : lines) {
    lineStarts.push_back(offset);
    offset += line.size() + 1;
  }
  lineStarts.push_back(text.size());

  struct ScopeFrame {
    int id = 0;
    int parentId = -1;
    int depth = 0;
    size_t startOffset = 0;
    size_t endOffset = 0;
    bool forScope = false;
    int closeAfterReturnToDepth = -1;
  };
  struct PendingForScope {
    int scopeId = -1;
    int parentDepth = 0;
    int createdLine = -1;
  };

  auto lineStart = [&](int line) -> size_t {
    if (line < 0)
      return 0;
    if (line < static_cast<int>(lineStarts.size()))
      return lineStarts[static_cast<size_t>(line)];
    return text.size();
  };

  auto lineEnd = [&](int line) -> size_t {
    if (line < 0)
      return 0;
    if (line + 1 < static_cast<int>(lineStarts.size()))
      return lineStarts[static_cast<size_t>(line + 1)];
    return text.size();
  };

  for (auto &function : snapshot.functions) {
    function.locals.clear();
    if (!function.hasBody || function.bodyStartLine < 0 ||
        function.bodyEndLine < 0 ||
        function.bodyStartLine >= function.bodyEndLine) {
      continue;
    }

    std::vector<ScopeFrame> scopes;
    std::vector<int> scopeStack;
    std::vector<PendingForScope> pendingForScopes;
    int nextScopeId = 0;
    int functionBraceDepth = 1;

    auto enterScope = [&](size_t startOffset, bool forScope = false,
                          int closeAfterReturnToDepth = -1) -> int {
      ScopeFrame scope;
      scope.id = nextScopeId++;
      scope.parentId = scopeStack.empty() ? -1 : scopeStack.back();
      scope.depth = static_cast<int>(scopeStack.size());
      scope.startOffset = startOffset;
      scope.endOffset = lineEnd(function.bodyEndLine);
      scope.forScope = forScope;
      scope.closeAfterReturnToDepth = closeAfterReturnToDepth;
      scopes.push_back(scope);
      scopeStack.push_back(scope.id);
      return scope.id;
    };

    auto scopeById = [&](int id) -> ScopeFrame * {
      for (auto &scope : scopes) {
        if (scope.id == id)
          return &scope;
      }
      return nullptr;
    };

    auto exitTopScope = [&](size_t endOffset) {
      if (scopeStack.size() <= 1)
        return;
      ScopeFrame *scope = scopeById(scopeStack.back());
      if (scope)
        scope->endOffset = endOffset;
      scopeStack.pop_back();
    };

    auto closeForScopesReturnedToDepth = [&](int depth, size_t endOffset) {
      while (scopeStack.size() > 1) {
        ScopeFrame *scope = scopeById(scopeStack.back());
        if (!scope || !scope->forScope ||
            scope->closeAfterReturnToDepth != depth) {
          break;
        }
        scope->endOffset = endOffset;
        scopeStack.pop_back();
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

    auto currentScope = [&]() -> ScopeFrame * {
      if (scopeStack.empty())
        return nullptr;
      return scopeById(scopeStack.back());
    };

    enterScope(lineStart(function.bodyStartLine));

    bool inBlockComment = false;
    for (int line = function.bodyStartLine + 1;
         line < function.bodyEndLine &&
         line < static_cast<int>(lines.size());
         line++) {
      const std::string &lineText = lines[static_cast<size_t>(line)];
      std::string code = lineText;
      const size_t lineComment = code.find("//");
      if (lineComment != std::string::npos)
        code = code.substr(0, lineComment);

      const auto forDecls = extractForInitializerDeclarationsInLineShared(code);
      if (!forDecls.empty()) {
        const int forScopeId =
            enterScope(lineStart(line), true, functionBraceDepth);
        pendingForScopes.push_back(
            PendingForScope{forScopeId, functionBraceDepth, line});
        for (const auto &decl : forDecls) {
          ScopeFrame *scope = currentScope();
          if (!scope)
            continue;
          SemanticSnapshot::FunctionInfo::LocalInfo local;
          local.name = decl.name;
          local.type = decl.type;
          local.offset = lineStart(line) + decl.start;
          local.line = line;
          local.character =
              byteOffsetInLineToUtf16(lineText, static_cast<int>(decl.start));
          local.depth = scope->depth;
          local.scopeId = scope->id;
          local.parentScopeId = scope->parentId;
          function.locals.push_back(std::move(local));
        }
      }

      if (code.find(';') != std::string::npos) {
        const auto declarations = extractDeclarationsInLineShared(code);
        for (const auto &decl : declarations) {
          ScopeFrame *scope = currentScope();
          if (!scope)
            continue;
          SemanticSnapshot::FunctionInfo::LocalInfo local;
          local.name = decl.name;
          local.type = decl.type;
          local.offset = lineStart(line) + decl.start;
          local.line = line;
          local.character =
              byteOffsetInLineToUtf16(lineText, static_cast<int>(decl.start));
          local.depth = scope->depth;
          local.scopeId = scope->id;
          local.parentScopeId = scope->parentId;
          function.locals.push_back(std::move(local));
        }
      }

      for (size_t i = 0; i < lineText.size(); i++) {
        const char ch = lineText[i];
        const char next = i + 1 < lineText.size() ? lineText[i + 1] : '\0';
        const size_t charOffset = lineStart(line) + i;
        if (inBlockComment) {
          if (ch == '*' && next == '/') {
            inBlockComment = false;
            i++;
          }
          continue;
        }
        if (ch == '/' && next == '*') {
          inBlockComment = true;
          i++;
          continue;
        }
        if (ch == '/' && next == '/')
          break;
        if (ch == '{') {
          for (size_t pendingIndex = 0; pendingIndex < pendingForScopes.size();
               pendingIndex++) {
            if (pendingForScopes[pendingIndex].parentDepth == functionBraceDepth) {
              const int pendingScopeId = pendingForScopes[pendingIndex].scopeId;
              ScopeFrame *scope = scopeById(pendingScopeId);
              if (scope)
                scope->closeAfterReturnToDepth = functionBraceDepth;
              erasePendingForScope(pendingScopeId);
              break;
            }
          }
          functionBraceDepth++;
          enterScope(charOffset + 1);
          continue;
        }
        if (ch == '}' && functionBraceDepth > 1) {
          exitTopScope(charOffset);
          functionBraceDepth--;
          closeForScopesReturnedToDepth(functionBraceDepth, charOffset);
        }
      }

      const auto tokens = lexLineTokens(code);
      const bool meaningfulLine = !tokens.empty();
      if (meaningfulLine) {
        for (auto it = pendingForScopes.begin();
             it != pendingForScopes.end();) {
          if (it->createdLine < line) {
            ScopeFrame *scope = scopeById(it->scopeId);
            if (scope)
              scope->endOffset = lineEnd(line);
            if (!scopeStack.empty() && scopeStack.back() == it->scopeId)
              scopeStack.pop_back();
            it = pendingForScopes.erase(it);
          } else {
            ++it;
          }
        }
      }
    }

    const size_t functionEnd = lineEnd(function.bodyEndLine);
    for (auto &scope : scopes) {
      if (scope.endOffset == 0 || scope.endOffset > functionEnd)
        scope.endOffset = functionEnd;
    }
    for (auto &local : function.locals) {
      ScopeFrame *scope = scopeById(local.scopeId);
      if (!scope)
        continue;
      local.scopeStartOffset = scope->startOffset;
      local.scopeEndOffset = scope->endOffset;
    }
  }
}

static std::shared_ptr<const SemanticSnapshot> getOrBuildSemanticSnapshot(
    const std::string &uri, const ExpandedSource &expandedSource,
    uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &unitPath = std::string(),
    const std::string &analysisContextFingerprint = std::string()) {
  SemanticCacheKey key = makeSemanticSnapshotCacheKey(
      uri, workspaceFolders, includePaths, shaderExtensions, defines, unitPath,
      analysisContextFingerprint);
  auto snapshot = semanticCacheGetSnapshot(key, uri, epoch);
  if (snapshot && snapshot->semanticDataComplete)
    return snapshot;

  SemanticSnapshot created = snapshot ? *snapshot : SemanticSnapshot{};
  created.uri = uri;
  created.documentEpoch = epoch;
  const HlslAstDocument document = buildHlslAstDocument(expandedSource);
  populateSemanticSnapshotFromAst(document, created);
  populateSemanticSnapshotLocals(expandedSource.text, created);
  semanticCacheUpsertSnapshot(key, created);
  return semanticCacheGetSnapshot(key, uri, epoch);
}

static std::shared_ptr<const SemanticSnapshot> getOrBuildSemanticSnapshot(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines) {
  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  return getOrBuildSemanticSnapshot(uri, expandedSource, epoch, workspaceFolders,
                                    includePaths, shaderExtensions, defines);
}

static int lineIndexForOffset(const std::string &text, size_t offset) {
  offset = std::min(offset, text.size());
  int line = 0;
  for (size_t i = 0; i < offset; i++) {
    if (text[i] == '\n')
      line++;
  }
  return line;
}

static int sourceLineForOutputLine(const ExpandedSource &expandedSource,
                                   int outputLine) {
  if (outputLine < 0)
    return outputLine;
  if (outputLine <
      static_cast<int>(expandedSource.sourceMap.outputLineToSourceLine.size())) {
    return expandedSource.sourceMap.outputLineToSourceLine[outputLine];
  }
  return outputLine;
}

static void appendStructFieldInfosFromLine(
    const std::string &lineText, int sourceLine,
    std::vector<SemanticSnapshotStructFieldInfo> &fieldsOut) {
  const auto declarations = extractDeclarationsInLineShared(lineText);
  for (const auto &decl : declarations) {
    SemanticSnapshotStructFieldInfo field;
    field.name = decl.name;
    field.type = decl.type;
    field.line = sourceLine;
    fieldsOut.push_back(std::move(field));
  }
}

static bool collectStructFieldInfosFromExpandedText(
    const ExpandedSource &expandedSource, const std::string &structName,
    std::vector<SemanticSnapshotStructFieldInfo> &fieldsOut) {
  fieldsOut.clear();
  if (structName.empty())
    return false;

  std::istringstream stream(expandedSource.text);
  std::string lineText;
  bool inTargetStruct = false;
  int braceDepth = 0;
  int outputLine = 0;

  while (std::getline(stream, lineText)) {
    std::string code = lineText;
    const size_t lineComment = code.find("//");
    if (lineComment != std::string::npos)
      code = code.substr(0, lineComment);

    if (!inTargetStruct) {
      std::string candidateName;
      if (extractStructNameInLine(code, candidateName) &&
          candidateName == structName) {
        inTargetStruct = true;
        const size_t openBrace = code.find('{');
        braceDepth = openBrace != std::string::npos ? 1 : 0;
        if (openBrace != std::string::npos && openBrace + 1 < code.size()) {
          appendStructFieldInfosFromLine(
              code.substr(openBrace + 1),
              sourceLineForOutputLine(expandedSource, outputLine), fieldsOut);
        }
      }
      outputLine++;
      continue;
    }

    if (braceDepth == 0) {
      const size_t openBrace = code.find('{');
      if (openBrace != std::string::npos) {
        braceDepth = 1;
        if (openBrace + 1 < code.size()) {
          appendStructFieldInfosFromLine(
              code.substr(openBrace + 1),
              sourceLineForOutputLine(expandedSource, outputLine), fieldsOut);
        }
      }
      outputLine++;
      continue;
    }

    if (braceDepth == 1) {
      appendStructFieldInfosFromLine(
          code, sourceLineForOutputLine(expandedSource, outputLine), fieldsOut);
    }

    for (char ch : code) {
      if (ch == '{') {
        braceDepth++;
      } else if (ch == '}') {
        braceDepth--;
        if (braceDepth <= 0)
          return !fieldsOut.empty();
      }
    }

    outputLine++;
  }

  return !fieldsOut.empty();
}

} // namespace

bool querySemanticSnapshotFunctionSignature(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name, int lineIndex, int nameCharacter,
    std::string &labelOut, std::vector<std::string> &parametersOut) {
  labelOut.clear();
  parametersOut.clear();
  if (uri.empty() || name.empty())
    return false;

  auto snapshot = getOrBuildSemanticSnapshot(uri, text, epoch, workspaceFolders,
                                             includePaths, shaderExtensions,
                                             defines);
  if (!snapshot)
    return false;

  auto byNameIt = snapshot->functionsByName.find(name);
  if (byNameIt == snapshot->functionsByName.end() || byNameIt->second.empty())
    return false;

  const SemanticSnapshot::FunctionInfo *chosen = nullptr;
  for (size_t index : byNameIt->second) {
    if (index >= snapshot->functions.size())
      continue;
    const auto &function = snapshot->functions[index];
    if (function.line == lineIndex && function.character == nameCharacter) {
      chosen = &function;
      break;
    }
  }
  if (!chosen) {
    for (size_t index : byNameIt->second) {
      if (index >= snapshot->functions.size())
        continue;
      const auto &function = snapshot->functions[index];
      if (function.line == lineIndex) {
        chosen = &function;
        break;
      }
    }
  }
  if (!chosen)
    chosen = &snapshot->functions[byNameIt->second.front()];

  labelOut = chosen->label;
  parametersOut = chosen->parameters;
  return !labelOut.empty();
}

bool querySemanticSnapshotFunctionOverloads(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name,
    std::vector<SemanticSnapshotFunctionOverloadInfo> &overloadsOut) {
  overloadsOut.clear();
  if (uri.empty() || name.empty())
    return false;

  auto snapshot = getOrBuildSemanticSnapshot(uri, text, epoch, workspaceFolders,
                                             includePaths, shaderExtensions,
                                             defines);
  if (!snapshot)
    return false;

  auto byNameIt = snapshot->functionsByName.find(name);
  if (byNameIt == snapshot->functionsByName.end() || byNameIt->second.empty())
    return false;

  overloadsOut.reserve(byNameIt->second.size());
  for (size_t index : byNameIt->second) {
    if (index >= snapshot->functions.size())
      continue;
    const auto &function = snapshot->functions[index];
    SemanticSnapshotFunctionOverloadInfo info;
    info.label = function.label;
    info.parameters = function.parameters;
    info.returnType = function.returnType;
    info.line = function.line;
    info.character = function.character;
    info.hasBody = function.hasBody;
    overloadsOut.push_back(std::move(info));
  }
  return !overloadsOut.empty();
}

bool querySemanticSnapshotParameterTypeAtOffset(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name, size_t offset, std::string &typeOut) {
  typeOut.clear();
  if (uri.empty() || name.empty())
    return false;

  auto snapshot = getOrBuildSemanticSnapshot(uri, text, epoch, workspaceFolders,
                                             includePaths, shaderExtensions,
                                             defines);
  if (!snapshot)
    return false;

  const int line = lineIndexForOffset(text, offset);
  const SemanticSnapshot::FunctionInfo *best = nullptr;
  int bestSpan = 0;
  for (const auto &function : snapshot->functions) {
    const int visibleEndLine =
        function.hasBody && function.bodyEndLine >= 0
            ? function.bodyEndLine
            : (function.signatureEndLine >= 0 ? function.signatureEndLine
                                              : function.line);
    if (line < function.line || line > visibleEndLine)
      continue;
    const int span = visibleEndLine - function.line;
    if (!best || span < bestSpan) {
      best = &function;
      bestSpan = span;
    }
  }
  if (!best)
    return false;

  for (const auto &parameter : best->parameterInfos) {
    if (parameter.first != name)
      continue;
    typeOut = parameter.second;
    return !typeOut.empty();
  }
  return false;
}

bool querySemanticSnapshotLocalTypeAtOffset(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name, size_t offset, std::string &typeOut) {
  typeOut.clear();
  if (uri.empty() || name.empty())
    return false;

  auto snapshot = getOrBuildSemanticSnapshot(uri, text, epoch, workspaceFolders,
                                             includePaths, shaderExtensions,
                                             defines);
  if (!snapshot)
    return false;

  const int line = lineIndexForOffset(text, offset);
  const SemanticSnapshot::FunctionInfo *bestFunction = nullptr;
  int bestSpan = 0;
  for (const auto &function : snapshot->functions) {
    if (!function.hasBody || function.bodyStartLine < 0 ||
        function.bodyEndLine < 0) {
      continue;
    }
    if (line < function.bodyStartLine || line > function.bodyEndLine)
      continue;
    const int span = function.bodyEndLine - function.bodyStartLine;
    if (!bestFunction || span < bestSpan) {
      bestFunction = &function;
      bestSpan = span;
    }
  }
  if (!bestFunction)
    return false;

  const SemanticSnapshot::FunctionInfo::LocalInfo *bestLocal = nullptr;
  for (const auto &local : bestFunction->locals) {
    if (local.name != name || local.offset > offset)
      continue;
    if (local.scopeStartOffset > offset ||
        (local.scopeEndOffset > 0 && offset >= local.scopeEndOffset)) {
      continue;
    }
    if (!bestLocal || local.depth > bestLocal->depth ||
        (local.depth == bestLocal->depth && local.offset >= bestLocal->offset)) {
      bestLocal = &local;
    }
  }
  if (!bestLocal)
    return false;

  typeOut = bestLocal->type;
  return !typeOut.empty();
}

bool querySemanticSnapshotStructFields(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &structName, std::vector<std::string> &fieldsOut) {
  fieldsOut.clear();
  std::vector<SemanticSnapshotStructFieldInfo> fieldInfos;
  if (!querySemanticSnapshotStructFieldInfos(
          uri, text, epoch, workspaceFolders, includePaths, shaderExtensions,
          defines, structName, fieldInfos) ||
      fieldInfos.empty()) {
    return false;
  }

  fieldsOut.reserve(fieldInfos.size());
  for (const auto &field : fieldInfos)
    fieldsOut.push_back(field.name);
  return !fieldsOut.empty();
}

bool querySemanticSnapshotStructFieldInfos(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &structName,
    std::vector<SemanticSnapshotStructFieldInfo> &fieldsOut) {
  fieldsOut.clear();
  if (uri.empty() || structName.empty())
    return false;

  auto snapshot = getOrBuildSemanticSnapshot(uri, text, epoch, workspaceFolders,
                                             includePaths, shaderExtensions,
                                             defines);
  if (snapshot) {
    auto it = snapshot->structByName.find(structName);
    if (it != snapshot->structByName.end() && it->second < snapshot->structs.size()) {
      const auto &info = snapshot->structs[it->second];
      fieldsOut.reserve(info.fields.size());
      for (const auto &field : info.fields) {
        SemanticSnapshotStructFieldInfo item;
        item.name = field.name;
        item.type = field.type;
        item.line = field.line;
        fieldsOut.push_back(std::move(item));
      }
      if (!fieldsOut.empty())
        return true;
    }
  }

  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  return collectStructFieldInfosFromExpandedText(expandedSource, structName,
                                                 fieldsOut);
}

bool querySemanticSnapshotStructField(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &structName, const std::string &fieldName,
    SemanticSnapshotFieldQueryResult &resultOut) {
  resultOut = SemanticSnapshotFieldQueryResult{};
  if (uri.empty() || structName.empty() || fieldName.empty())
    return false;

  std::vector<SemanticSnapshotStructFieldInfo> fields;
  if (!querySemanticSnapshotStructFieldInfos(
          uri, text, epoch, workspaceFolders, includePaths, shaderExtensions,
          defines, structName, fields)) {
    return false;
  }

  for (const auto &field : fields) {
    if (field.name != fieldName)
      continue;
    resultOut.type = field.type;
    resultOut.line = field.line;
    return !resultOut.type.empty();
  }
  return false;
}

bool querySemanticSnapshotGlobalType(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name, std::string &typeOut) {
  typeOut.clear();
  if (uri.empty() || name.empty())
    return false;

  auto snapshot = getOrBuildSemanticSnapshot(uri, text, epoch, workspaceFolders,
                                             includePaths, shaderExtensions,
                                             defines);
  if (!snapshot)
    return false;

  auto it = snapshot->globalByName.find(name);
  if (it == snapshot->globalByName.end() || it->second >= snapshot->globals.size())
    return false;

  typeOut = snapshot->globals[it->second].type;
  return !typeOut.empty();
}

bool querySemanticSnapshotSymbolType(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name, std::string &typeOut) {
  typeOut.clear();
  if (uri.empty() || name.empty())
    return false;

  if (querySemanticSnapshotGlobalType(uri, text, epoch, workspaceFolders,
                                      includePaths, shaderExtensions, defines,
                                      name, typeOut)) {
    return true;
  }

  auto snapshot = getOrBuildSemanticSnapshot(uri, text, epoch, workspaceFolders,
                                             includePaths, shaderExtensions,
                                             defines);
  if (!snapshot)
    return false;

  for (const auto &function : snapshot->functions) {
    for (const auto &parameter : function.parameterInfos) {
      if (parameter.first != name)
        continue;
      typeOut = parameter.second;
      return !typeOut.empty();
    }
    for (const auto &local : function.locals) {
      if (local.name != name)
        continue;
      typeOut = local.type;
      return !typeOut.empty();
    }
  }

  return false;
}

std::shared_ptr<const SemanticSnapshot> getSemanticSnapshotView(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines) {
  return getOrBuildSemanticSnapshot(uri, text, epoch, workspaceFolders,
                                    includePaths, shaderExtensions, defines);
}

std::shared_ptr<const SemanticSnapshot> getSemanticSnapshotViewFromExpandedSource(
    const std::string &uri, const ExpandedSource &expandedSource,
    uint64_t epoch, const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &unitPath,
    const std::string &analysisContextFingerprint) {
  return getOrBuildSemanticSnapshot(
      uri, expandedSource, epoch, workspaceFolders, includePaths,
      shaderExtensions, defines, unitPath, analysisContextFingerprint);
}
