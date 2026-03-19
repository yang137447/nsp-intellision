#pragma once

#include "json.hpp"

#include <string>
#include <vector>

struct SemanticTokenLegend {
  std::vector<std::string> tokenTypes;
  std::vector<std::string> tokenModifiers;
};

SemanticTokenLegend createDefaultSemanticTokenLegend();

Json buildSemanticTokensFull(const std::string &text,
                             const SemanticTokenLegend &legend);

Json buildSemanticTokensRange(const std::string &text, int startLine,
                              int startCharacter, int endLine,
                              int endCharacter,
                              const SemanticTokenLegend &legend);
