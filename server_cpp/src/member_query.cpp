#include "member_query.hpp"

#include "definition_fallback.hpp"
#include "fast_ast.hpp"
#include "hover_docs.hpp"
#include "hover_markdown.hpp"
#include "include_resolver.hpp"
#include "server_parse.hpp"
#include "server_request_handlers.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"
#include "workspace_scan_plan.hpp"

#include <sstream>
#include <unordered_set>
#include <utility>

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
bool collectStructFieldsInIncludeGraph(
    const std::string &uri, const std::string &structName,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::unordered_set<std::string> &visited,
    std::vector<std::string> &fieldsOut);
bool collectStructFieldsFromText(const std::string &text,
                                 const std::string &structName,
                                 std::vector<std::string> &fieldsOut);
bool findDeclaredIdentifierInDeclarationLine(const std::string &line,
                                             const std::string &word,
                                             size_t &posOut);
size_t positionToOffset(const std::string &text, int line, int character);

namespace {

void collectMemberNamesFromTextRecursive(
    const std::string &baseUri, const std::string &text,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions, int depth,
    std::unordered_set<std::string> &visitedUris,
    std::unordered_set<std::string> &seenNames,
    std::vector<std::string> &outFields) {
  if (depth <= 0 || outFields.size() >= 512) {
    return;
  }
  if (!visitedUris.insert(baseUri).second) {
    return;
  }

  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    std::string code = line;
    size_t lineComment = code.find("//");
    if (lineComment != std::string::npos) {
      code = code.substr(0, lineComment);
    }
    std::string includePath;
    if (extractIncludePath(code, includePath)) {
      auto candidates =
          resolveIncludeCandidates(baseUri, includePath, workspaceFolders,
                                   includePaths, shaderExtensions);
      std::vector<std::string> candidateUris;
      candidateUris.reserve(candidates.size());
      for (const auto &candidate : candidates) {
        candidateUris.push_back(pathToUri(candidate));
      }
      prefetchDocumentTexts(candidateUris, documents);
      for (const auto &candidate : candidates) {
        std::string candidateUri = pathToUri(candidate);
        std::string candidateText;
        if (!loadDocumentText(candidateUri, documents, candidateText)) {
          continue;
        }
        collectMemberNamesFromTextRecursive(
            candidateUri, candidateText, documents, workspaceFolders,
            includePaths, shaderExtensions, depth - 1, visitedUris, seenNames,
            outFields);
        break;
      }
      continue;
    }
    auto names = extractDeclaredNamesFromLine(code);
    for (const auto &name : names) {
      if (!seenNames.insert(name).second) {
        continue;
      }
      outFields.push_back(name);
    }
    if (outFields.size() >= 512) {
      return;
    }
  }
}

bool collectStructFieldsFromTextWithInlineIncludes(
    const std::string &baseUri, const std::string &text,
    const std::string &structName,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::vector<std::string> &fieldsOut) {
  fieldsOut.clear();
  std::unordered_set<std::string> seen;
  std::istringstream stream(text);
  std::string line;
  bool inTargetStruct = false;
  int braceDepth = 0;
  while (std::getline(stream, line)) {
    std::string code = line;
    size_t lineComment = code.find("//");
    if (lineComment != std::string::npos) {
      code = code.substr(0, lineComment);
    }
    if (!inTargetStruct) {
      std::string name;
      if (extractStructNameInLine(code, name) && name == structName) {
        inTargetStruct = true;
        braceDepth = code.find('{') != std::string::npos ? 1 : 0;
      }
      continue;
    }

    if (braceDepth == 0) {
      if (code.find('{') != std::string::npos) {
        braceDepth = 1;
      }
      continue;
    }

    if (braceDepth == 1) {
      std::string includePath;
      if (extractIncludePath(code, includePath)) {
        auto candidates =
            resolveIncludeCandidates(baseUri, includePath, workspaceFolders,
                                     includePaths, shaderExtensions);
        std::vector<std::string> candidateUris;
        candidateUris.reserve(candidates.size());
        for (const auto &candidate : candidates) {
          candidateUris.push_back(pathToUri(candidate));
        }
        prefetchDocumentTexts(candidateUris, documents);
        for (const auto &candidate : candidates) {
          std::string candidateUri = pathToUri(candidate);
          std::string candidateText;
          if (!loadDocumentText(candidateUri, documents, candidateText)) {
            continue;
          }
          std::unordered_set<std::string> visited;
          collectMemberNamesFromTextRecursive(
              candidateUri, candidateText, documents, workspaceFolders,
              includePaths, shaderExtensions, 12, visited, seen, fieldsOut);
          break;
        }
      } else {
        auto names = extractDeclaredNamesFromLine(code);
        for (const auto &name : names) {
          if (!seen.insert(name).second) {
            continue;
          }
          fieldsOut.push_back(name);
        }
      }
    }

    for (char ch : code) {
      if (ch == '{') {
        braceDepth++;
      } else if (ch == '}') {
        braceDepth--;
        if (braceDepth <= 0) {
          return !fieldsOut.empty();
        }
      }
    }
    if (fieldsOut.size() >= 512) {
      return true;
    }
  }
  return false;
}

bool findStructMemberDeclarationAtOrAfterLine(const std::string &text,
                                              int startLine,
                                              const std::string &memberName,
                                              int &lineOut,
                                              int &minCharacterOut) {
  lineOut = -1;
  minCharacterOut = 0;
  if (memberName.empty()) {
    return false;
  }

  std::istringstream stream(text);
  std::string line;
  int lineIndex = 0;
  bool inBlockComment = false;
  bool inString = false;
  bool bodyStarted = false;
  int braceDepth = 0;

  while (std::getline(stream, line)) {
    if (lineIndex < startLine) {
      lineIndex++;
      continue;
    }

    if (bodyStarted && braceDepth == 1) {
      size_t pos = 0;
      if (findDeclaredIdentifierInDeclarationLine(line, memberName, pos)) {
        lineOut = lineIndex;
        int endByte = static_cast<int>(pos + memberName.size());
        minCharacterOut = byteOffsetInLineToUtf16(line, endByte);
        return true;
      }
    }

    bool inLineComment = false;
    for (size_t i = 0; i < line.size(); i++) {
      char ch = line[i];
      char next = i + 1 < line.size() ? line[i + 1] : '\0';
      if (inLineComment) {
        break;
      }
      if (inBlockComment) {
        if (ch == '*' && next == '/') {
          inBlockComment = false;
          i++;
        }
        continue;
      }
      if (inString) {
        if (ch == '"' && (i == 0 || line[i - 1] != '\\')) {
          inString = false;
        }
        continue;
      }
      if (ch == '/' && next == '/') {
        inLineComment = true;
        break;
      }
      if (ch == '/' && next == '*') {
        inBlockComment = true;
        i++;
        continue;
      }
      if (ch == '"') {
        inString = true;
        continue;
      }
      if (ch == '{') {
        if (!bodyStarted) {
          bodyStarted = true;
          braceDepth = 1;
        } else {
          braceDepth++;
        }
      } else if (ch == '}' && bodyStarted) {
        braceDepth--;
        if (braceDepth <= 0) {
          return false;
        }
      }
    }

    lineIndex++;
  }
  return false;
}

} // namespace

MemberAccessBaseTypeResult resolveMemberAccessBaseType(
    const std::string &uri, const Document &doc, const std::string &base,
    size_t cursorOffset, const ServerRequestContext &ctx,
    const MemberAccessBaseTypeOptions &options) {
  MemberAccessBaseTypeResult result;
  if (queryFastAstLocalType(uri, doc.text, doc.epoch, base, cursorOffset,
                            result.typeName) ||
      findTypeOfIdentifierInTextUpTo(doc.text, base, cursorOffset,
                                     result.typeName) ||
      findParameterTypeInTextUpTo(doc.text, base, cursorOffset,
                                  result.typeName)) {
    result.resolved = true;
    return result;
  }

  if (options.includeWorkspaceIndexFallback &&
      workspaceIndexGetSymbolType(base, result.typeName)) {
    result.resolved = !result.typeName.empty();
    if (result.resolved) {
      return result;
    }
  }

  if (options.includeIncludeGraphFallback) {
    std::unordered_set<std::string> visitedTypes;
    if (findTypeOfIdentifierInIncludeGraph(
            uri, base, ctx.documentSnapshot(), ctx.workspaceFolders,
            ctx.includePaths, ctx.shaderExtensions, visitedTypes,
            result.typeName)) {
      result.resolved = !result.typeName.empty();
    }
  }

  return result;
}

bool resolveMemberHoverInfo(const std::string &uri,
                            const std::string &ownerType,
                            const std::string &memberName,
                            ServerRequestContext &ctx,
                            MemberHoverInfo &out) {
  out = MemberHoverInfo{};
  if (ownerType.empty() || memberName.empty()) {
    return false;
  }

  workspaceIndexGetStructMemberType(ownerType, memberName, out.memberType);
  out.hasStructLocation =
      workspaceIndexFindStructDefinition(ownerType, out.ownerStructLocation);

  if (out.memberType.empty()) {
    WorkspaceScanPlanOptions scanOptions;
    scanOptions.appendWorkspaceFolders = false;
    scanOptions.fallbackToWorkspaceFoldersWhenEmpty = true;
    scanOptions.requiredExtensions = {".nsf", ".hlsli", ".h"};
    WorkspaceScanPlan scanPlan = buildWorkspaceScanPlan(ctx, "", scanOptions);
    if (!out.hasStructLocation) {
      out.hasStructLocation = findStructDefinitionByWorkspaceScan(
          ownerType, scanPlan.roots, scanPlan.extensions,
          out.ownerStructLocation);
    }
    if (out.hasStructLocation) {
      std::string structText;
      if (ctx.readDocumentText(out.ownerStructLocation.uri, structText)) {
        int memberLine = -1;
        int memberMinChar = 0;
        if (findStructMemberDeclarationAtOrAfterLine(
                structText, out.ownerStructLocation.line, memberName, memberLine,
                memberMinChar)) {
          size_t memberOffset = positionToOffset(
              structText, memberLine, std::max(0, memberMinChar));
          findTypeOfIdentifierInTextUpTo(
              structText, memberName, memberOffset + memberName.size(),
              out.memberType);
          out.memberLeadingDoc =
              extractLeadingDocumentationAtLine(structText, memberLine);
          out.memberInlineDoc = extractTrailingInlineCommentAtLine(
              structText, memberLine, memberMinChar);
        }
      }
    }
  } else if (out.hasStructLocation) {
    std::string structText;
    if (ctx.readDocumentText(out.ownerStructLocation.uri, structText)) {
      int memberLine = -1;
      int memberMinChar = 0;
      if (findStructMemberDeclarationAtOrAfterLine(
              structText, out.ownerStructLocation.line, memberName, memberLine,
              memberMinChar)) {
        out.memberLeadingDoc =
            extractLeadingDocumentationAtLine(structText, memberLine);
        out.memberInlineDoc = extractTrailingInlineCommentAtLine(
            structText, memberLine, memberMinChar);
      }
    }
  }

  out.found = !out.memberType.empty();
  return out.found;
}

bool collectMemberCompletionQuery(const std::string &uri,
                                  const std::string &ownerType,
                                  ServerRequestContext &ctx,
                                  MemberCompletionQuery &out) {
  out = MemberCompletionQuery{};
  out.ownerType = ownerType;
  if (ownerType.empty()) {
    return false;
  }

  listHlslBuiltinMethodsForType(ownerType, out.methods);

  std::unordered_set<std::string> visitedStructs;
  if (collectStructFieldsInIncludeGraph(
          uri, ownerType, ctx.documentSnapshot(), ctx.workspaceFolders,
          ctx.includePaths, ctx.shaderExtensions, visitedStructs, out.fields) &&
      !out.fields.empty()) {
    return true;
  }

  WorkspaceScanPlanOptions scanPlanOptions;
  scanPlanOptions.includeDocumentDirectory = true;
  scanPlanOptions.requiredExtensions = {".nsf", ".hlsli", ".h"};
  WorkspaceScanPlan scanPlan =
      buildWorkspaceScanPlan(ctx, uri, scanPlanOptions);
  resetWorkspaceScanCachesIfPlanChanged(ctx, scanPlan);

  auto cached = ctx.scanStructFieldsCache.find(ownerType);
  if (cached != ctx.scanStructFieldsCache.end()) {
    out.fields = cached->second;
    return !out.fields.empty() || !out.methods.empty();
  }

  if (workspaceIndexGetStructFields(ownerType, out.fields) && !out.fields.empty()) {
    ctx.scanStructFieldsCache.emplace(ownerType, out.fields);
    return true;
  }

  if (ctx.scanStructFieldsMisses.find(ownerType) ==
      ctx.scanStructFieldsMisses.end()) {
    DefinitionLocation scanned;
    if (findStructDefinitionByWorkspaceScan(ownerType, scanPlan.roots,
                                            scanPlan.extensions, scanned)) {
      std::string defText;
      if (ctx.readDocumentText(scanned.uri, defText)) {
        std::vector<std::string> scannedFields;
        if (collectStructFieldsFromText(defText, ownerType, scannedFields) &&
            !scannedFields.empty()) {
          ctx.scanStructFieldsCache.emplace(ownerType, scannedFields);
          out.fields = std::move(scannedFields);
          return true;
        }
      }
    }
    ctx.scanStructFieldsMisses.insert(ownerType);
  }

  return !out.methods.empty();
}
