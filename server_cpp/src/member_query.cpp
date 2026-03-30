#include "member_query.hpp"

#include "hover_docs.hpp"
#include "hover_markdown.hpp"
#include "interactive_semantic_runtime.hpp"
#include "semantic_snapshot.hpp"
#include "server_parse.hpp"
#include "server_request_handlers.hpp"
#include "text_utils.hpp"
#include "workspace_summary_runtime.hpp"

#include <sstream>
#include <utility>

bool findDeclaredIdentifierInDeclarationLine(const std::string &line,
                                             const std::string &word,
                                             size_t &posOut);
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

} // namespace

MemberAccessBaseTypeResult resolveMemberAccessBaseType(
    const std::string &uri, const Document &doc, const std::string &base,
    size_t cursorOffset, const ServerRequestContext &ctx,
    const MemberAccessBaseTypeOptions &options) {
  return interactiveResolveMemberAccessBaseType(uri, doc, base, cursorOffset,
                                                ctx, options);
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

  workspaceSummaryRuntimeGetStructMemberType(ownerType, memberName,
                                             out.memberType);
  out.hasStructLocation =
      workspaceSummaryRuntimeFindStructDefinition(ownerType,
                                                  out.ownerStructLocation);

  if (out.memberType.empty())
    return false;

  if (out.hasStructLocation) {
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
  return interactiveCollectMemberCompletionQuery(uri, ownerType, ctx, out) &&
         (!out.fields.empty() || !out.methods.empty());
}
