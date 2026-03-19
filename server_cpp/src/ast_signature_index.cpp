#include "ast_signature_index.hpp"

#include "nsf_lexer.hpp"
#include "signature_help.hpp"
#include "text_utils.hpp"

#include <sstream>

void indexFunctionSignatures(
    const std::string &text, std::vector<AstFunctionSignatureEntry> &outEntries,
    std::unordered_map<std::string, std::vector<size_t>> &outByName) {
  outEntries.clear();
  outByName.clear();
  std::istringstream stream(text);
  std::string lineText;
  int lineIndex = 0;
  while (std::getline(stream, lineText)) {
    const auto tokens = lexLineTokens(lineText);
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
      if (tokens[i].kind != LexToken::Kind::Identifier)
        continue;
      if (tokens[i + 1].kind != LexToken::Kind::Punct ||
          tokens[i + 1].text != "(") {
        continue;
      }
      const std::string &name = tokens[i].text;
      const int nameChar =
          byteOffsetInLineToUtf16(lineText, static_cast<int>(tokens[i].start));
      std::string label;
      std::vector<std::string> parameters;
      if (!extractFunctionSignatureAt(text, lineIndex, nameChar, name, label,
                                      parameters) ||
          label.empty()) {
        continue;
      }
      AstFunctionSignatureEntry entry;
      entry.name = name;
      entry.line = lineIndex;
      entry.character = nameChar;
      entry.label = std::move(label);
      entry.parameters = std::move(parameters);
      const size_t index = outEntries.size();
      outEntries.push_back(std::move(entry));
      outByName[name].push_back(index);
      break;
    }
    lineIndex++;
  }
}
