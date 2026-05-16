#pragma once

// LSP semantic-token rendering for shader documents.
//
// Responsibilities:
// - define the process semantic-token legend
// - scan source text into LSP semantic-token payloads
// - when a deferred SemanticSnapshot is available, refine variable-like
//   identifiers with parameter/property/declaration/modification facts
//
// Non-goals:
// - this module does not own semantic snapshot construction or caching
// - it does not color comments or strings; editor grammar remains responsible

#include "json.hpp"

#include <string>
#include <vector>

struct SemanticSnapshot;

struct SemanticTokenLegend {
  std::vector<std::string> tokenTypes;
  std::vector<std::string> tokenModifiers;
};

SemanticTokenLegend createDefaultSemanticTokenLegend();

Json buildSemanticTokensFull(const std::string &text,
                             const SemanticTokenLegend &legend,
                             const SemanticSnapshot *snapshot = nullptr);

Json buildSemanticTokensRange(const std::string &text, int startLine,
                              int startCharacter, int endLine,
                              int endCharacter,
                              const SemanticTokenLegend &legend,
                              const SemanticSnapshot *snapshot = nullptr);
