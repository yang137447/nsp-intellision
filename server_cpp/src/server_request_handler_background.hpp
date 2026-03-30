#pragma once

#include "server_request_handlers.hpp"

#include <string>
#include <vector>

// Background request-family handlers extracted from server_request_handlers.cpp.
//
// Responsibilities:
// - implement deferred/background-only request handlers that already sit behind
//   the request-layer scheduling contract
// - keep the moved logic behavior-identical while shrinking the main dispatch
//   translation unit
//
// Non-goals:
// - does not redefine the dispatch contract owned by server_request_handlers.hpp
// - does not own shared semantic/runtime truth; handlers still defer to the
//   existing runtimes and workspace summary boundary

namespace request_background_handlers {

bool handleInlayHintRequest(const std::string &method, const Json &id,
                            const Json *params, ServerRequestContext &ctx,
                            const std::vector<std::string> &keywords,
                            const std::vector<std::string> &directives);

bool handleSemanticTokensFullRequest(
    const std::string &method, const Json &id, const Json *params,
    ServerRequestContext &ctx, const std::vector<std::string> &keywords,
    const std::vector<std::string> &directives);

bool handleSemanticTokensRangeRequest(
    const std::string &method, const Json &id, const Json *params,
    ServerRequestContext &ctx, const std::vector<std::string> &keywords,
    const std::vector<std::string> &directives);

bool handleDocumentSymbolRequest(const std::string &method, const Json &id,
                                 const Json *params,
                                 ServerRequestContext &ctx,
                                 const std::vector<std::string> &keywords,
                                 const std::vector<std::string> &directives);

bool handleWorkspaceSymbolRequest(const std::string &method, const Json &id,
                                  const Json *params,
                                  ServerRequestContext &ctx,
                                  const std::vector<std::string> &keywords,
                                  const std::vector<std::string> &directives);

} // namespace request_background_handlers
