#pragma once

#include "json.hpp"

#include <string>

struct Document;
struct ServerRequestContext;

Json inlayHintsRuntimeBuildFullDocument(const std::string &uri,
                                        const Document &doc,
                                        ServerRequestContext &ctx);

Json inlayHintsRuntimeBuildRange(const std::string &uri, const Document &doc,
                                 ServerRequestContext &ctx, int startLine,
                                 int startChar, int endLine, int endChar);

Json inlayHintsRuntimeBuildOrGetDeferredFull(const std::string &uri,
                                             const Document &doc,
                                             ServerRequestContext &ctx);

Json inlayHintsRuntimeFilterRange(const Json &fullHints, int startLine,
                                  int startChar, int endLine, int endChar);
