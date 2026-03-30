#include "server_request_handler_references.hpp"

#include "active_unit.hpp"
#include "expanded_source.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "server_occurrences.hpp"
#include "server_request_handler_common.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"
#include "workspace_summary_runtime.hpp"

#include <string>
#include <unordered_set>
#include <vector>

std::vector<LocatedOccurrence> collectOccurrencesForSymbol(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines);

namespace request_reference_handlers {

namespace {

std::vector<LocatedOccurrence> collectActiveOccurrencesInDocumentLocal(
    const std::string &uri, const std::string &text, const std::string &word,
    const std::unordered_map<std::string, int> &defines) {
  std::vector<LocatedOccurrence> locations;
  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  auto occurrences = findOccurrences(expandedSource.text, word);
  for (const auto &occ : occurrences) {
    locations.push_back(LocatedOccurrence{uri, occ.line, occ.start, occ.end});
  }
  return locations;
}

bool collectIncludeContextOccurrences(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    std::vector<LocatedOccurrence> &outOccurrences) {
  outOccurrences.clear();
  std::vector<std::string> candidateUnitPaths =
      collectIncludeContextUnitPaths(uri, ctx);
  if (candidateUnitPaths.size() <= 1)
    return false;

  std::unordered_set<std::string> seen;
  const auto documents = ctx.documentSnapshot();
  for (const auto &candidateUnitPath : candidateUnitPaths) {
    std::vector<std::string> orderedPaths;
    workspaceSummaryRuntimeCollectIncludeClosureForUnit(candidateUnitPath,
                                                        orderedPaths, 1024);
    std::vector<std::string> orderedUris;
    orderedUris.reserve(orderedPaths.size());
    for (const auto &path : orderedPaths) {
      if (!path.empty())
        orderedUris.push_back(pathToUri(path));
    }
    prefetchDocumentTexts(orderedUris, documents);
    for (const auto &candidateUri : orderedUris) {
      std::string text;
      if (!loadDocumentText(candidateUri, documents, text))
        continue;
      auto occurrences = collectActiveOccurrencesInDocumentLocal(
          candidateUri, text, word, ctx.preprocessorDefines);
      for (const auto &occ : occurrences) {
        const std::string key =
            occ.uri + "|" + std::to_string(occ.line) + "|" +
            std::to_string(occ.start) + "|" + std::to_string(occ.end);
        if (!seen.insert(key).second)
          continue;
        outOccurrences.push_back(occ);
      }
    }
  }
  return !outOccurrences.empty();
}

} // namespace

bool handleReferencesRequest(const std::string &method, const Json &id,
                             const Json *params, ServerRequestContext &ctx,
                             const std::vector<std::string> &,
                             const std::vector<std::string> &) {
  if (method != "textDocument/references" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  if (!textDocument || !position) {
    writeResponse(id, makeArray());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *lineValue = getObjectValue(*position, "line");
  const Json *charValue = getObjectValue(*position, "character");
  if (!uriValue || !lineValue || !charValue) {
    writeResponse(id, makeArray());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  int line = static_cast<int>(getNumberValue(*lineValue));
  int character = static_cast<int>(getNumberValue(*charValue));
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeArray());
    return true;
  }
  std::string lineText = getLineAt(doc->text, line);
  std::string word = extractWordAt(lineText, character);
  if (word.empty()) {
    writeResponse(id, makeArray());
    return true;
  }
  std::vector<LocatedOccurrence> occurrences;
  if (!collectIncludeContextOccurrences(uri, word, ctx, occurrences)) {
    occurrences = collectOccurrencesForSymbol(
        uri, word, ctx.documentSnapshot(), ctx.workspaceFolders,
        ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines);
  }
  Json locations = makeArray();
  for (const auto &occ : occurrences) {
    locations.a.push_back(
        makeLocationRange(occ.uri, occ.line, occ.start, occ.end));
  }
  writeResponse(id, locations);
  return true;
}

bool handlePrepareRenameRequest(const std::string &method, const Json &id,
                                const Json *params,
                                ServerRequestContext &ctx,
                                const std::vector<std::string> &,
                                const std::vector<std::string> &) {
  if (method != "textDocument/prepareRename" || !params)
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
  std::string lineText = getLineAt(doc->text, line);
  std::string word = extractWordAt(lineText, character);
  if (word.empty()) {
    writeResponse(id, makeNull());
    return true;
  }
  if (!getActiveUnitPath().empty()) {
    // fall through
  } else {
    const std::string path = uriToPath(uri);
    if (!path.empty() && !pathHasNsfExtension(path)) {
      std::vector<std::string> candidateUnitPaths =
          collectIncludeContextUnitPaths(uri, ctx);
      if (candidateUnitPaths.size() > 1) {
        writeResponse(id, makeNull());
        return true;
      }
    }
  }
  auto occs = findOccurrences(lineText, word);
  if (occs.empty()) {
    writeResponse(id, makeNull());
    return true;
  }
  Json range = makeRangeExact(line, occs.front().start, occs.front().end);
  Json result = makeObject();
  result.o["range"] = range;
  result.o["placeholder"] = makeString(word);
  writeResponse(id, result);
  return true;
}

bool handleRenameRequest(const std::string &method, const Json &id,
                         const Json *params, ServerRequestContext &ctx,
                         const std::vector<std::string> &,
                         const std::vector<std::string> &) {
  if (method != "textDocument/rename" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  const Json *newNameValue = getObjectValue(*params, "newName");
  const Json *uriValue =
      textDocument ? getObjectValue(*textDocument, "uri") : nullptr;
  if (!textDocument || !position || !newNameValue || !uriValue) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *lineValue = getObjectValue(*position, "line");
  const Json *charValue = getObjectValue(*position, "character");
  if (!lineValue || !charValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  int line = static_cast<int>(getNumberValue(*lineValue));
  int character = static_cast<int>(getNumberValue(*charValue));
  std::string newName = getStringValue(*newNameValue);
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string lineText = getLineAt(doc->text, line);
  std::string word = extractWordAt(lineText, character);
  if (word.empty()) {
    writeResponse(id, makeNull());
    return true;
  }
  if (!getActiveUnitPath().empty()) {
    // fall through
  } else {
    const std::string path = uriToPath(uri);
    if (!path.empty() && !pathHasNsfExtension(path)) {
      std::vector<std::string> candidateUnitPaths =
          collectIncludeContextUnitPaths(uri, ctx);
      if (candidateUnitPaths.size() > 1) {
        writeResponse(id, makeNull());
        return true;
      }
    }
  }
  auto occurrences = collectOccurrencesForSymbol(
      uri, word, ctx.documentSnapshot(), ctx.workspaceFolders, ctx.includePaths,
      ctx.shaderExtensions, ctx.preprocessorDefines);
  Json changes = makeObject();
  for (const auto &occ : occurrences) {
    Json edit = makeObject();
    edit.o["range"] = makeRangeExact(occ.line, occ.start, occ.end);
    edit.o["newText"] = makeString(newName);
    auto existing = changes.o.find(occ.uri);
    if (existing == changes.o.end()) {
      Json edits = makeArray();
      edits.a.push_back(std::move(edit));
      changes.o[occ.uri] = std::move(edits);
    } else {
      existing->second.a.push_back(std::move(edit));
    }
  }
  Json workspaceEdit = makeObject();
  workspaceEdit.o["changes"] = changes;
  writeResponse(id, workspaceEdit);
  return true;
}

} // namespace request_reference_handlers
