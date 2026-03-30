#include "semantic_snapshot.hpp"

#include "active_unit.hpp"
#include "expanded_source.hpp"
#include "hlsl_ast.hpp"
#include "nsf_lexer.hpp"
#include "server_parse.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <sstream>

namespace {

static std::string
makeDefinesFingerprint(const std::unordered_map<std::string, int> &defines) {
  if (defines.empty())
    return std::string();

  std::vector<std::pair<std::string, int>> ordered(defines.begin(),
                                                   defines.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  std::ostringstream oss;
  for (const auto &entry : ordered) {
    oss << entry.first.size() << ":" << entry.first << "=" << entry.second
        << ";";
  }
  return oss.str();
}

static SemanticCacheKey makeSemanticSnapshotCacheKey(
    const std::string &uri, const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines) {
  SemanticCacheKey key;
  key.workspaceFolders = workspaceFolders;
  key.includePaths = includePaths;
  key.shaderExtensions = shaderExtensions;
  key.definesFingerprint = makeDefinesFingerprint(defines);
  const std::string activeUnitPath = getActiveUnitPath();
  key.unitPath = activeUnitPath.empty() ? uriToPath(uri) : activeUnitPath;
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
    for (const auto &parameter : function.parameters) {
      info.parameters.push_back(parameter.text);
      info.parameterInfos.push_back({parameter.name, parameter.type});
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
    const size_t index = snapshot.globals.size();
    snapshot.globals.push_back(std::move(info));
    snapshot.globalByName[decl.name] = index;
  }

  snapshot.semanticDataComplete = true;
}

static void populateSemanticSnapshotLocals(const std::string &text,
                                           SemanticSnapshot &snapshot) {
  std::istringstream stream(text);
  std::string lineText;
  size_t lineStartOffset = 0;
  int currentLine = 0;
  int currentDepth = 0;
  bool inBlockComment = false;

  auto findOwnerFunction =
      [&](int line) -> SemanticSnapshot::FunctionInfo * {
    SemanticSnapshot::FunctionInfo *best = nullptr;
    int bestSpan = 0;
    for (auto &function : snapshot.functions) {
      if (!function.hasBody || function.bodyStartLine < 0 ||
          function.bodyEndLine < 0) {
        continue;
      }
      if (line < function.bodyStartLine || line > function.bodyEndLine)
        continue;
      const int span = function.bodyEndLine - function.bodyStartLine;
      if (!best || span < bestSpan) {
        best = &function;
        bestSpan = span;
      }
    }
    return best;
  };

  while (std::getline(stream, lineText)) {
    SemanticSnapshot::FunctionInfo *ownerFunction = findOwnerFunction(currentLine);
    std::string code = lineText;
    size_t lineComment = code.find("//");
    if (lineComment != std::string::npos)
      code = code.substr(0, lineComment);

    if (ownerFunction && !code.empty() && code.find(';') != std::string::npos) {
      const auto declarations = extractDeclarationsInLineShared(code);
      for (const auto &decl : declarations) {
        SemanticSnapshot::FunctionInfo::LocalInfo local;
        local.name = decl.name;
        local.type = decl.type;
        local.offset = lineStartOffset + decl.start;
        local.depth = currentDepth;
        ownerFunction->locals.push_back(std::move(local));
      }
    }

    for (size_t i = 0; i < lineText.size(); i++) {
      char ch = lineText[i];
      char next = i + 1 < lineText.size() ? lineText[i + 1] : '\0';
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
      if (ch == '{') {
        currentDepth++;
      } else if (ch == '}' && currentDepth > 0) {
        currentDepth--;
      }
    }

    lineStartOffset += lineText.size() + 1;
    currentLine++;
  }
}

static std::shared_ptr<const SemanticSnapshot> getOrBuildSemanticSnapshot(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines) {
  SemanticCacheKey key = makeSemanticSnapshotCacheKey(
      uri, workspaceFolders, includePaths, shaderExtensions, defines);
  auto snapshot = semanticCacheGetSnapshot(key, uri, epoch);
  if (snapshot && snapshot->semanticDataComplete)
    return snapshot;

  SemanticSnapshot created = snapshot ? *snapshot : SemanticSnapshot{};
  created.uri = uri;
  created.documentEpoch = epoch;
  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  const HlslAstDocument document = buildHlslAstDocument(expandedSource);
  populateSemanticSnapshotFromAst(document, created);
  populateSemanticSnapshotLocals(expandedSource.text, created);
  semanticCacheUpsertSnapshot(key, created);
  return semanticCacheGetSnapshot(key, uri, epoch);
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
