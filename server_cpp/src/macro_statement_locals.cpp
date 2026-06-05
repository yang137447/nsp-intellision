#include "macro_statement_locals.hpp"

#include "nsf_lexer.hpp"
#include "server_parse.hpp"

#include <utility>

namespace {

bool isStandaloneStatementMacroToken(const std::vector<LexToken> &tokens,
                                     size_t tokenIndex) {
  if (tokenIndex >= tokens.size() ||
      tokens[tokenIndex].kind != LexToken::Kind::Identifier) {
    return false;
  }
  for (size_t i = 0; i < tokens.size(); i++) {
    if (i == tokenIndex)
      continue;
    const LexToken &token = tokens[i];
    if (token.kind == LexToken::Kind::Punct && token.text == ";")
      continue;
    return false;
  }
  return true;
}

} // namespace

std::vector<MacroStatementLocalDeclaration>
collectStatementLikeMacroLocalDeclarations(const PreprocessorView &view,
                                           int lineIndex,
                                           const std::string &lineText,
                                           size_t lineStartOffset) {
  std::vector<MacroStatementLocalDeclaration> out;
  if (lineIndex < 0 || lineText.empty())
    return out;
  if (lineIndex < static_cast<int>(view.lineActive.size()) &&
      !view.lineActive[static_cast<size_t>(lineIndex)]) {
    return out;
  }

  const std::vector<LexToken> tokens = lexLineTokens(lineText);
  for (size_t i = 0; i < tokens.size(); i++) {
    const LexToken &token = tokens[i];
    if (token.kind != LexToken::Kind::Identifier)
      continue;
    if (!isStandaloneStatementMacroToken(tokens, i))
      continue;

    PreprocessorMacroReplacement replacement;
    if (!lookupActivePreprocessorMacroReplacement(view, lineIndex, token.text,
                                                  replacement)) {
      continue;
    }
    if (replacement.functionLike || replacement.replacement.empty())
      continue;

    const auto declarations =
        extractDeclarationsInLineShared(replacement.replacement);
    for (const auto &decl : declarations) {
      if (decl.name.empty() || decl.type.empty())
        continue;
      MacroStatementLocalDeclaration item;
      item.name = decl.name;
      item.type = decl.type;
      item.macroName = token.text;
      item.invocationLine = lineIndex;
      item.invocationStart = static_cast<int>(token.start);
      item.invocationEnd = static_cast<int>(token.end);
      item.invocationOffset = lineStartOffset + token.start;
      item.sourceUri = replacement.sourceUri;
      item.sourceLine = replacement.sourceLine;
      item.sourceStart = replacement.sourceStart;
      item.sourceEnd = replacement.sourceEnd;
      out.push_back(std::move(item));
    }
  }
  return out;
}
