#pragma once

#include "server_request_handlers.hpp"

#include <string>
#include <vector>

// References/rename request-family handlers extracted from
// server_request_handlers.cpp.
//
// Responsibilities:
// - implement the references / prepareRename / rename handlers without changing
//   the existing request-layer behavior
// - keep request-local occurrence gathering helpers near the handlers that own
//   them
//
// Non-goals:
// - does not redefine include-context resolution facts
// - does not introduce a second workspace traversal path

namespace request_reference_handlers {

bool handleReferencesRequest(const std::string &method, const Json &id,
                             const Json *params, ServerRequestContext &ctx,
                             const std::vector<std::string> &keywords,
                             const std::vector<std::string> &directives);

bool handlePrepareRenameRequest(const std::string &method, const Json &id,
                                const Json *params,
                                ServerRequestContext &ctx,
                                const std::vector<std::string> &keywords,
                                const std::vector<std::string> &directives);

bool handleRenameRequest(const std::string &method, const Json &id,
                         const Json *params, ServerRequestContext &ctx,
                         const std::vector<std::string> &keywords,
                         const std::vector<std::string> &directives);

} // namespace request_reference_handlers
