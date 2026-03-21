#include "member_query.hpp"

#include "definition_fallback.hpp"
#include "hover_docs.hpp"
#include "hover_markdown.hpp"
#include "semantic_snapshot.hpp"
#include "server_parse.hpp"
#include "server_request_handlers.hpp"
#include "text_utils.hpp"
#include "workspace_index.hpp"
#include "workspace_scan_plan.hpp"

#include <sstream>
#include <unordered_set>
#include <utility>

bool findDeclaredIdentifierInDeclarationLine(const std::string &line,
                                             const std::string &word,
                                             size_t &posOut);
std::vector<std::string> getIncludeGraphUrisCached(
    const std::string &rootUri,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions);

namespace {

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

    if (lineIndex == startLine) {
      size_t pos = 0;
      if (findDeclaredIdentifierInDeclarationLine(line, memberName, pos)) {
        lineOut = lineIndex;
        int endByte = static_cast<int>(pos + memberName.size());
        minCharacterOut = byteOffsetInLineToUtf16(line, endByte);
        return true;
      }
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

bool tryResolveMemberHoverInfoFromDocument(const std::string &docUri,
                                           const std::string &ownerType,
                                           const std::string &memberName,
                                           ServerRequestContext &ctx,
                                           MemberHoverInfo &out) {
  std::string docText;
  if (!ctx.readDocumentText(docUri, docText))
    return false;

  uint64_t docEpoch = 0;
  if (const Document *doc = ctx.findDocument(docUri))
    docEpoch = doc->epoch;

  SemanticSnapshotFieldQueryResult fieldInfo;
  if (!querySemanticSnapshotStructField(
          docUri, docText, docEpoch, ctx.workspaceFolders, ctx.includePaths,
          ctx.shaderExtensions, ctx.preprocessorDefines, ownerType, memberName,
          fieldInfo)) {
    return false;
  }

  out.memberType = fieldInfo.type;
  out.ownerStructLocation.uri = docUri;
  out.ownerStructLocation.line = fieldInfo.line;
  out.hasStructLocation = true;

  int memberLine = -1;
  int memberMinChar = 0;
  if (findStructMemberDeclarationAtOrAfterLine(
          docText, fieldInfo.line >= 0 ? fieldInfo.line : 0, memberName,
          memberLine, memberMinChar)) {
    out.ownerStructLocation.line = memberLine;
    out.memberLeadingDoc = extractLeadingDocumentationAtLine(docText, memberLine);
    out.memberInlineDoc =
        extractTrailingInlineCommentAtLine(docText, memberLine, memberMinChar);
  }

  out.found = !out.memberType.empty();
  return out.found;
}

bool queryStructFieldsFromSemanticSnapshot(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::string &structName, const ServerRequestContext &ctx,
    std::vector<std::string> &fieldsOut) {
  return querySemanticSnapshotStructFields(
             uri, text, epoch, ctx.workspaceFolders, ctx.includePaths,
             ctx.shaderExtensions, ctx.preprocessorDefines, structName,
             fieldsOut) &&
         !fieldsOut.empty();
}

bool collectStructFieldsFromSemanticSnapshotInIncludeGraph(
    const std::string &rootUri, const std::string &structName,
    const ServerRequestContext &ctx, std::vector<std::string> &fieldsOut) {
  const auto orderedUris = getIncludeGraphUrisCached(
      rootUri, ctx.documentSnapshot(), ctx.workspaceFolders, ctx.includePaths,
      ctx.shaderExtensions);
  prefetchDocumentTexts(orderedUris, ctx.documentSnapshot());
  for (const auto &candidateUri : orderedUris) {
    std::string candidateText;
    if (!ctx.readDocumentText(candidateUri, candidateText))
      continue;
    uint64_t candidateEpoch = 0;
    if (const Document *candidateDoc = ctx.findDocument(candidateUri))
      candidateEpoch = candidateDoc->epoch;
    if (queryStructFieldsFromSemanticSnapshot(candidateUri, candidateText,
                                             candidateEpoch, structName, ctx,
                                             fieldsOut)) {
      return true;
    }
  }
  return false;
}

bool querySymbolTypeFromSemanticSnapshotInIncludeGraph(
    const std::string &rootUri, const std::string &symbol,
    const ServerRequestContext &ctx, std::string &typeOut) {
  typeOut.clear();
  if (rootUri.empty() || symbol.empty())
    return false;

  const auto orderedUris = getIncludeGraphUrisCached(
      rootUri, ctx.documentSnapshot(), ctx.workspaceFolders, ctx.includePaths,
      ctx.shaderExtensions);
  prefetchDocumentTexts(orderedUris, ctx.documentSnapshot());
  for (const auto &candidateUri : orderedUris) {
    std::string candidateText;
    if (!ctx.readDocumentText(candidateUri, candidateText))
      continue;
    uint64_t candidateEpoch = 0;
    if (const Document *candidateDoc = ctx.findDocument(candidateUri))
      candidateEpoch = candidateDoc->epoch;
    if (querySemanticSnapshotSymbolType(
            candidateUri, candidateText, candidateEpoch, ctx.workspaceFolders,
            ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
            symbol, typeOut) &&
        !typeOut.empty()) {
      return true;
    }
  }
  return false;
}

} // namespace

MemberAccessBaseTypeResult resolveMemberAccessBaseType(
    const std::string &uri, const Document &doc, const std::string &base,
    size_t cursorOffset, const ServerRequestContext &ctx,
    const MemberAccessBaseTypeOptions &options) {
  MemberAccessBaseTypeResult result;
  if (querySemanticSnapshotLocalTypeAtOffset(
          uri, doc.text, doc.epoch, ctx.workspaceFolders, ctx.includePaths,
          ctx.shaderExtensions, ctx.preprocessorDefines, base, cursorOffset,
          result.typeName) ||
      querySemanticSnapshotParameterTypeAtOffset(
          uri, doc.text, doc.epoch, ctx.workspaceFolders, ctx.includePaths,
          ctx.shaderExtensions, ctx.preprocessorDefines, base, cursorOffset,
          result.typeName)) {
    result.resolved = true;
    return result;
  }

  if (querySemanticSnapshotGlobalType(
          uri, doc.text, doc.epoch, ctx.workspaceFolders, ctx.includePaths,
          ctx.shaderExtensions, ctx.preprocessorDefines, base,
          result.typeName)) {
    result.resolved = !result.typeName.empty();
    if (result.resolved)
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
    if (querySymbolTypeFromSemanticSnapshotInIncludeGraph(uri, base, ctx,
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

  if (tryResolveMemberHoverInfoFromDocument(uri, ownerType, memberName, ctx,
                                            out)) {
    return true;
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
        uint64_t structEpoch = 0;
        if (const Document *structDoc = ctx.findDocument(out.ownerStructLocation.uri))
          structEpoch = structDoc->epoch;
        SemanticSnapshotFieldQueryResult fieldInfo;
        if (querySemanticSnapshotStructField(
                out.ownerStructLocation.uri, structText, structEpoch,
                ctx.workspaceFolders, ctx.includePaths, ctx.shaderExtensions,
                ctx.preprocessorDefines, ownerType, memberName, fieldInfo)) {
          out.memberType = fieldInfo.type;
        }
        int memberLine = -1;
        int memberMinChar = 0;
        if (findStructMemberDeclarationAtOrAfterLine(
                structText,
                fieldInfo.line >= 0 ? fieldInfo.line : out.ownerStructLocation.line,
                memberName, memberLine, memberMinChar)) {
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

  std::string currentText;
  if (ctx.readDocumentText(uri, currentText)) {
    std::vector<std::string> localFields;
    uint64_t currentEpoch = 0;
    if (const Document *currentDoc = ctx.findDocument(uri))
      currentEpoch = currentDoc->epoch;
    if (queryStructFieldsFromSemanticSnapshot(uri, currentText, currentEpoch,
                                              ownerType, ctx, localFields)) {
      out.fields = std::move(localFields);
      return true;
    }
  }

  if (collectStructFieldsFromSemanticSnapshotInIncludeGraph(uri, ownerType, ctx,
                                                            out.fields)) {
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
        uint64_t scannedEpoch = 0;
        if (const Document *scannedDoc = ctx.findDocument(scanned.uri))
          scannedEpoch = scannedDoc->epoch;
        if (queryStructFieldsFromSemanticSnapshot(scanned.uri, defText,
                                                  scannedEpoch, ownerType, ctx,
                                                  scannedFields)) {
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
