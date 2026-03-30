#pragma once

#include "server_request_handlers.hpp"

#include <string>
#include <vector>

namespace request_signature_handlers {

bool handleSignatureHelpRequest(const std::string &method, const Json &id,
                                const Json *params,
                                ServerRequestContext &ctx,
                                const std::vector<std::string> &keywords,
                                const std::vector<std::string> &directives);

} // namespace request_signature_handlers
