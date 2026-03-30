#include "server_request_handler_background.hpp"

#include "deferred_doc_runtime.hpp"
#include "inlay_hints_runtime.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "text_utils.hpp"
#include "workspace_summary_runtime.hpp"

namespace request_background_handlers {

namespace {

bool isRequestCancelled(ServerRequestContext &ctx) {
  if (!ctx.isCancellationRequested)
    return false;
  return ctx.isCancellationRequested();
}

} // namespace

bool handleInlayHintRequest(const std::string &method, const Json &id,
                            const Json *params, ServerRequestContext &ctx,
                            const std::vector<std::string> &,
                            const std::vector<std::string> &) {
  if (method != "textDocument/inlayHint" || !params)
    return false;

  if (!ctx.inlayHintsEnabled || !ctx.inlayHintsParameterNamesEnabled) {
    writeResponse(id, makeArray());
    return true;
  }

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *range = getObjectValue(*params, "range");
  if (!textDocument || !range) {
    writeResponse(id, makeArray());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *start = getObjectValue(*range, "start");
  const Json *end = getObjectValue(*range, "end");
  if (!uriValue || !start || !end) {
    writeResponse(id, makeArray());
    return true;
  }
  const Json *startLineValue = getObjectValue(*start, "line");
  const Json *startCharValue = getObjectValue(*start, "character");
  const Json *endLineValue = getObjectValue(*end, "line");
  const Json *endCharValue = getObjectValue(*end, "character");
  if (!startLineValue || !startCharValue || !endLineValue || !endCharValue) {
    writeResponse(id, makeArray());
    return true;
  }

  std::string uri = getStringValue(*uriValue);
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeArray());
    return true;
  }

  int startLine = static_cast<int>(getNumberValue(*startLineValue));
  int startChar = static_cast<int>(getNumberValue(*startCharValue));
  int endLine = static_cast<int>(getNumberValue(*endLineValue));
  int endChar = static_cast<int>(getNumberValue(*endCharValue));
  if (startLine < 0 || startChar < 0 || endLine < 0 || endChar < 0) {
    writeResponse(id, makeArray());
    return true;
  }

  const std::string &text = doc->text;
  size_t startOffset = positionToOffsetUtf16(text, startLine, startChar);
  size_t endOffset = positionToOffsetUtf16(text, endLine, endChar);
  if (endOffset < startOffset)
    std::swap(startOffset, endOffset);
  if (endOffset == startOffset) {
    writeResponse(id, makeArray());
    return true;
  }
  Json fullHints = inlayHintsRuntimeBuildOrGetDeferredFull(uri, *doc, ctx);
  if (isRequestCancelled(ctx)) {
    writeError(id, -32800, "Request cancelled");
    return true;
  }
  writeResponse(id, inlayHintsRuntimeFilterRange(fullHints, startLine, startChar,
                                                 endLine, endChar));
  return true;
}

bool handleSemanticTokensFullRequest(const std::string &method, const Json &id,
                                     const Json *params,
                                     ServerRequestContext &ctx,
                                     const std::vector<std::string> &,
                                     const std::vector<std::string> &) {
  if (method != "textDocument/semanticTokens/full" || !params)
    return false;
  if (!ctx.semanticTokensEnabled) {
    writeResponse(id, makeNull());
    return true;
  }

  const Json *textDocument = getObjectValue(*params, "textDocument");
  if (!textDocument) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  if (!uriValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }
  writeResponse(id, buildDeferredSemanticTokensFull(uri, *doc, ctx));
  return true;
}

bool handleSemanticTokensRangeRequest(const std::string &method, const Json &id,
                                      const Json *params,
                                      ServerRequestContext &ctx,
                                      const std::vector<std::string> &,
                                      const std::vector<std::string> &) {
  if (method != "textDocument/semanticTokens/range" || !params)
    return false;
  if (!ctx.semanticTokensEnabled) {
    writeResponse(id, makeNull());
    return true;
  }

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *range = getObjectValue(*params, "range");
  if (!textDocument || !range) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *start = getObjectValue(*range, "start");
  const Json *end = getObjectValue(*range, "end");
  if (!uriValue || !start || !end) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *startLineValue = getObjectValue(*start, "line");
  const Json *startCharValue = getObjectValue(*start, "character");
  const Json *endLineValue = getObjectValue(*end, "line");
  const Json *endCharValue = getObjectValue(*end, "character");
  if (!startLineValue || !startCharValue || !endLineValue || !endCharValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }
  writeResponse(id, buildDeferredSemanticTokensRange(
                        uri, *doc,
                        static_cast<int>(getNumberValue(*startLineValue)),
                        static_cast<int>(getNumberValue(*startCharValue)),
                        static_cast<int>(getNumberValue(*endLineValue)),
                        static_cast<int>(getNumberValue(*endCharValue)), ctx));
  return true;
}

bool handleDocumentSymbolRequest(const std::string &method, const Json &id,
                                 const Json *params,
                                 ServerRequestContext &ctx,
                                 const std::vector<std::string> &,
                                 const std::vector<std::string> &) {
  if (method != "textDocument/documentSymbol" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  if (!textDocument) {
    writeResponse(id, makeArray());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  if (!uriValue) {
    writeResponse(id, makeArray());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeArray());
    return true;
  }
  writeResponse(id, buildDeferredDocumentSymbols(uri, *doc, ctx));
  return true;
}

bool handleWorkspaceSymbolRequest(const std::string &method, const Json &id,
                                  const Json *params, ServerRequestContext &,
                                  const std::vector<std::string> &,
                                  const std::vector<std::string> &) {
  if (method != "workspace/symbol")
    return false;

  std::string query;
  if (params) {
    const Json *queryValue = getObjectValue(*params, "query");
    if (queryValue && queryValue->type == Json::Type::String)
      query = queryValue->s;
  }

  std::vector<IndexedDefinition> defs;
  workspaceSummaryRuntimeQuerySymbols(query, defs, query.empty() ? 128 : 256);
  Json result = makeArray();
  result.a.reserve(defs.size());
  for (const auto &def : defs) {
    if (def.name.empty() || def.uri.empty())
      continue;
    Json symbol = makeObject();
    symbol.o["name"] = makeString(def.name);
    symbol.o["kind"] =
        makeNumber(static_cast<double>(def.kind > 0 ? def.kind : 13));
    symbol.o["location"] =
        makeLocationRange(def.uri, def.line, def.start, def.end);
    if (!def.type.empty())
      symbol.o["containerName"] = makeString(def.type);
    result.a.push_back(std::move(symbol));
  }
  writeResponse(id, result);
  return true;
}

} // namespace request_background_handlers
