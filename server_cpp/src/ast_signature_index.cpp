#include "ast_signature_index.hpp"

#include "hlsl_ast.hpp"

void indexFunctionSignatures(
    const std::string &text, std::vector<AstFunctionSignatureEntry> &outEntries,
    std::unordered_map<std::string, std::vector<size_t>> &outByName) {
  outEntries.clear();
  outByName.clear();
  const HlslAstDocument document = buildHlslAstDocument(text);
  outEntries.reserve(document.functions.size());
  for (const auto &function : document.functions) {
    AstFunctionSignatureEntry entry;
    entry.name = function.name;
    entry.line = function.line;
    entry.character = function.character;
    entry.label = function.label;
    entry.parameters.reserve(function.parameters.size());
    for (const auto &parameter : function.parameters)
      entry.parameters.push_back(parameter.text);
    const size_t index = outEntries.size();
    outEntries.push_back(std::move(entry));
    outByName[function.name].push_back(index);
  }
}
