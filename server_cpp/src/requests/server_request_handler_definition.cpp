#include "server_request_handler_definition.hpp"

#include "include_resolver.hpp"
#include "interactive_semantic_runtime.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "server_parse.hpp"
#include "server_request_handler_common.hpp"
#include "symbol_query.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"
#include "workspace_summary_runtime.hpp"

#include <string>
#include <vector>

bool request_definition_handlers::handleDefinitionRequest(
    const std::string &method, const Json &id, const Json *params,
    ServerRequestContext &ctx, const std::vector<std::string> &,
    const std::vector<std::string> &) {
  if (method != "textDocument/definition" || !params)
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
  std::string includePath;
  if (extractIncludePath(lineText, includePath)) {
    auto candidates =
        resolveIncludeCandidates(uri, includePath, ctx.workspaceFolders,
                                 ctx.includePaths, ctx.shaderExtensions);
    for (const auto &candidate : candidates) {
#ifdef _WIN32
      struct _stat statBuffer;
      if (_stat(candidate.c_str(), &statBuffer) == 0) {
        Json locations = makeArray();
        locations.a.push_back(makeLocation(pathToUri(candidate)));
        writeResponse(id, locations);
        includePath.clear();
        break;
      }
#endif
    }
    if (includePath.empty())
      return true;
  }
  std::string word = extractWordAt(lineText, character);
  if (word.empty()) {
    writeResponse(id, makeArray());
    return true;
  }
  const size_t cursorOffset = positionToOffsetUtf16(doc->text, line, character);
  {
    std::vector<DefinitionLocation> includeContextLocations;
    if (collectIncludeContextDefinitionLocations(uri, word, ctx,
                                                 includeContextLocations)) {
      Json locations = makeArray();
      for (const auto &location : includeContextLocations) {
        locations.a.push_back(makeLocationRange(location.uri, location.line,
                                                location.start, location.end));
      }
      writeResponse(id, locations);
      return true;
    }
  }
  {
    DefinitionLocation interactiveLocation;
    if (interactiveResolveDefinitionLocation(uri, *doc, word, cursorOffset, ctx,
                                             interactiveLocation)) {
      Json locations = makeArray();
      locations.a.push_back(
          makeLocationRange(interactiveLocation.uri, interactiveLocation.line,
                            interactiveLocation.start, interactiveLocation.end));
      writeResponse(id, locations);
      return true;
    }
  }
  {
    DefinitionLocation workspaceLocation;
    if (pickBestWorkspaceDefinitionForCurrentContext(uri, word,
                                                     workspaceLocation)) {
      Json locations = makeArray();
      locations.a.push_back(makeLocationRange(workspaceLocation.uri,
                                              workspaceLocation.line,
                                              workspaceLocation.start,
                                              workspaceLocation.end));
      writeResponse(id, locations);
      return true;
    }
  }
  {
    SymbolDefinitionResolveOptions resolveOptions;
    resolveOptions.order =
        SymbolDefinitionSearchOrder::MacroThenWorkspace;
    ResolvedSymbolDefinitionTarget resolvedTarget;
    if (resolveSymbolDefinitionTarget(uri, word, ctx, resolveOptions,
                                      resolvedTarget)) {
      Json locations = makeArray();
      locations.a.push_back(makeLocationRange(resolvedTarget.location.uri,
                                              resolvedTarget.location.line,
                                              resolvedTarget.location.start,
                                              resolvedTarget.location.end));
      writeResponse(id, locations);
      return true;
    }
  }
  {
    std::vector<IndexedDefinition> defs;
    if (workspaceSummaryRuntimeFindDefinitions(word, defs, 256) &&
        !defs.empty()) {
      Json locations = makeArray();
      for (const auto &def : defs) {
        locations.a.push_back(makeLocationRange(def.uri, def.line, def.start,
                                                def.end));
      }
      writeResponse(id, locations);
      return true;
    }
  }
  writeResponse(id, makeArray());
  return true;
}

