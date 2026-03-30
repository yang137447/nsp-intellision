#include "diagnostics_symbol_type.hpp"
#include "diagnostics_semantic_common.hpp"


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
#include <vector>


static bool parseStructMembersFromText(
    const std::string &text, const std::string &structName,
    std::unordered_map<std::string, std::string> &membersOut) {
  std::istringstream stream(text);
  std::string lineText;
  bool inStruct = false;
  int braceDepth = 0;
  bool inBlockComment = false;
  while (std::getline(stream, lineText)) {
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
    if (tokens.empty())
      continue;

    if (!inStruct) {
      std::string trimmed = trimLeftCopy(lineText);
      if (trimmed.rfind("struct", 0) != 0)
        continue;
      bool sawName = false;
      for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Identifier &&
            tokens[i].text == "struct" &&
            tokens[i + 1].kind == LexToken::Kind::Identifier &&
            tokens[i + 1].text == structName) {
          sawName = true;
          break;
        }
      }
      if (!sawName)
        continue;
      inStruct = true;
      for (const auto &t : tokens) {
        if (t.kind != LexToken::Kind::Punct)
          continue;
        if (t.text == "{")
          braceDepth++;
        else if (t.text == "}")
          braceDepth = braceDepth > 0 ? braceDepth - 1 : 0;
      }
      continue;
    }

    for (const auto &t : tokens) {
      if (t.kind != LexToken::Kind::Punct)
        continue;
      if (t.text == "{")
        braceDepth++;
      else if (t.text == "}")
        braceDepth = braceDepth > 0 ? braceDepth - 1 : 0;
    }

    if (inStruct && braceDepth == 0)
      break;

    if (!inStruct || braceDepth != 1)
      continue;

    size_t typeIndex = std::string::npos;
    for (size_t i = 0; i < tokens.size(); i++) {
      if (tokens[i].kind != LexToken::Kind::Identifier)
        continue;
      if (isQualifierToken(tokens[i].text))
        continue;
      typeIndex = i;
      break;
    }
    if (typeIndex == std::string::npos)
      continue;
    if (typeIndex + 1 >= tokens.size())
      continue;
    if (tokens[typeIndex + 1].kind != LexToken::Kind::Identifier)
      continue;
    std::string memberName = tokens[typeIndex + 1].text;
    size_t stop = tokens.size();
    for (size_t i = typeIndex + 2; i < tokens.size(); i++) {
      if (tokens[i].kind == LexToken::Kind::Punct &&
          (tokens[i].text == ":" || tokens[i].text == ";" ||
           tokens[i].text == "["))
        break;
      if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == ";") {
        stop = i;
        break;
      }
    }
    std::string memberType = tokens[typeIndex].text;
    if (!memberName.empty() && !memberType.empty())
      membersOut.emplace(memberName, memberType);
  }
  return !membersOut.empty();
}

bool hasStructDeclarationInText(const std::string &text,
                                       const std::string &structName) {
  if (structName.empty())
    return false;
  std::istringstream stream(text);
  std::string lineText;
  bool inBlockComment = false;
  while (std::getline(stream, lineText)) {
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
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
      if (tokens[i].kind == LexToken::Kind::Identifier &&
          tokens[i].text == "struct" &&
          tokens[i + 1].kind == LexToken::Kind::Identifier &&
          tokens[i + 1].text == structName) {
        return true;
      }
    }
  }
  return false;
}

static std::string
parseSymbolTypeFromLineTokens(const std::vector<LexToken> &tokens,
                              const std::string &symbol) {
  for (size_t i = 0; i < tokens.size(); i++) {
    if (tokens[i].kind != LexToken::Kind::Identifier ||
        tokens[i].text != symbol)
      continue;
    if (i > 0) {
      const auto &prev = tokens[i - 1];
      if (prev.kind == LexToken::Kind::Punct &&
          (prev.text == "." || prev.text == "->" || prev.text == "::"))
        continue;
    }
    bool hasAssignBefore = false;
    for (size_t j = 0; j < i; j++) {
      if (tokens[j].kind == LexToken::Kind::Punct && tokens[j].text == "=") {
        hasAssignBefore = true;
        break;
      }
    }
    if (hasAssignBefore)
      continue;
    if (i == 0)
      continue;
    const auto &typeToken = tokens[i - 1];
    if (typeToken.kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(typeToken.text))
      continue;
    if (typeToken.text == "return" || typeToken.text == "if" ||
        typeToken.text == "for" || typeToken.text == "while" ||
        typeToken.text == "switch")
      continue;
    return typeToken.text;
  }
  return "";
}

std::string resolveSymbolTypeInText(const std::string &text,
                                           const std::string &symbol) {
  std::istringstream stream(text);
  std::string lineText;
  bool inBlockComment = false;
  while (std::getline(stream, lineText)) {
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
    const std::string type = parseSymbolTypeFromLineTokens(tokens, symbol);
    if (!type.empty())
      return type;
  }
  return "";
}

std::string resolveSymbolTypeByWorkspaceSummary(
    const std::string &symbol, SymbolTypeCache &cache) {
  std::string indexed;
  if (workspaceSummaryRuntimeGetSymbolType(symbol, indexed) &&
      !indexed.empty()) {
    cache.typeBySymbol.emplace(symbol, indexed);
    return indexed;
  }
  auto cached = cache.typeBySymbol.find(symbol);
  if (cached != cache.typeBySymbol.end())
    return cached->second;
  bool attempted = false;
  auto attemptIt = cache.attempted.find(symbol);
  if (attemptIt != cache.attempted.end())
    attempted = attemptIt->second;
  if (attempted)
    return "";
  cache.attempted[symbol] = true;
  cache.typeBySymbol.emplace(symbol, "");
  return "";
}

std::string resolveVisibleSymbolType(
    const std::string &symbol,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentText, SymbolTypeCache &cache) {
  if (symbol.empty())
    return "";
  auto localIt = locals.find(symbol);
  if (localIt != locals.end() && !localIt->second.empty())
    return localIt->second;
  std::string inText = resolveSymbolTypeInText(currentText, symbol);
  if (!inText.empty())
    return inText;
  return resolveSymbolTypeByWorkspaceSummary(symbol, cache);
}

std::string resolveStructMemberType(
    const std::string &structName, const std::string &memberName,
    const std::string &currentUri, const std::string &currentText,
    const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &scanExtensions,
    const std::unordered_map<std::string, int> &defines,
    StructTypeCache &cache) {
  std::string indexed;
    if (workspaceSummaryRuntimeGetStructMemberType(structName, memberName,
                                                   indexed) &&
        !indexed.empty()) {
    return indexed;
  }
  auto cached = cache.membersByStruct.find(structName);
  if (cached != cache.membersByStruct.end()) {
    auto it = cached->second.find(memberName);
    if (it != cached->second.end())
      return it->second;
    return "";
  }

  bool attempted = false;
  auto attemptIt = cache.attempted.find(structName);
  if (attemptIt != cache.attempted.end())
    attempted = attemptIt->second;
  if (attempted)
    return "";
  cache.attempted[structName] = true;

  std::unordered_map<std::string, std::string> members;
  std::vector<SemanticSnapshotStructFieldInfo> fieldInfos;
  if (!currentUri.empty() &&
      querySemanticSnapshotStructFieldInfos(currentUri, currentText, 0,
                                           workspaceFolders, includePaths,
                                           scanExtensions, defines, structName,
                                           fieldInfos)) {
    for (const auto &field : fieldInfos)
      members.emplace(field.name, field.type);
  }
  if (members.empty())
    parseStructMembersFromText(currentText, structName, members);
  if (members.empty()) {
    DefinitionLocation location;
    if (workspaceSummaryRuntimeFindStructDefinition(structName, location)) {
      std::string path = uriToPath(location.uri);
      if (path.empty())
        path = location.uri;
      std::string otherText;
      if (diagnosticsReadFileToString(path, otherText)) {
        fieldInfos.clear();
        if (querySemanticSnapshotStructFieldInfos(
                location.uri, otherText, 0, workspaceFolders, includePaths,
                scanExtensions, defines, structName, fieldInfos)) {
          for (const auto &field : fieldInfos)
            members.emplace(field.name, field.type);
        }
        if (members.empty())
          parseStructMembersFromText(otherText, structName, members);
      }
    }
  }

  cache.membersByStruct.emplace(structName, members);
  auto newCached = cache.membersByStruct.find(structName);
  if (newCached == cache.membersByStruct.end())
    return "";
  auto it = newCached->second.find(memberName);
  if (it != newCached->second.end())
    return it->second;
  return "";
}
