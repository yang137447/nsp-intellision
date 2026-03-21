#include "conditional_ast.hpp"

#include <cctype>
#include <sstream>
#include <utility>

namespace {

static std::vector<char> buildCodeMaskForLine(const std::string &lineText,
                                              bool &inBlockCommentInOut) {
  std::vector<char> mask(lineText.size(), 1);
  bool inString = false;
  bool inLineComment = false;
  for (size_t i = 0; i < lineText.size(); i++) {
    char ch = lineText[i];
    char next = (i + 1 < lineText.size()) ? lineText[i + 1] : '\0';

    if (inLineComment) {
      mask[i] = 0;
      continue;
    }
    if (inBlockCommentInOut) {
      mask[i] = 0;
      if (ch == '*' && next == '/') {
        if (i + 1 < mask.size())
          mask[i + 1] = 0;
        inBlockCommentInOut = false;
        i++;
      }
      continue;
    }
    if (inString) {
      if (ch == '"' && (i == 0 || lineText[i - 1] != '\\'))
        inString = false;
      continue;
    }
    if (ch == '"') {
      inString = true;
      continue;
    }
    if (ch == '/' && next == '/') {
      mask[i] = 0;
      if (i + 1 < mask.size())
        mask[i + 1] = 0;
      inLineComment = true;
      i++;
      continue;
    }
    if (ch == '/' && next == '*') {
      mask[i] = 0;
      if (i + 1 < mask.size())
        mask[i + 1] = 0;
      inBlockCommentInOut = true;
      i++;
      continue;
    }
  }
  return mask;
}

static bool isPreprocessorDirectiveLine(const std::string &lineText,
                                        const std::vector<char> &mask) {
  for (size_t i = 0; i < lineText.size() && i < mask.size(); i++) {
    if (!mask[i])
      continue;
    if (std::isspace(static_cast<unsigned char>(lineText[i])))
      continue;
    return lineText[i] == '#';
  }
  return false;
}

static ConditionalDirectiveKind
classifyDirectiveKind(const std::vector<LexToken> &tokens) {
  if (tokens.size() < 2)
    return ConditionalDirectiveKind::Unknown;
  if (tokens[0].kind != LexToken::Kind::Punct || tokens[0].text != "#")
    return ConditionalDirectiveKind::Unknown;
  if (tokens[1].kind != LexToken::Kind::Identifier)
    return ConditionalDirectiveKind::Unknown;

  const std::string &directive = tokens[1].text;
  if (directive == "if")
    return ConditionalDirectiveKind::If;
  if (directive == "ifdef")
    return ConditionalDirectiveKind::Ifdef;
  if (directive == "ifndef")
    return ConditionalDirectiveKind::Ifndef;
  if (directive == "elif")
    return ConditionalDirectiveKind::Elif;
  if (directive == "else")
    return ConditionalDirectiveKind::Else;
  if (directive == "endif")
    return ConditionalDirectiveKind::Endif;
  if (directive == "define")
    return ConditionalDirectiveKind::Define;
  if (directive == "undef")
    return ConditionalDirectiveKind::Undef;
  if (directive == "include")
    return ConditionalDirectiveKind::Include;
  return ConditionalDirectiveKind::Unknown;
}

struct ParseSequenceResult {
  std::vector<size_t> nodeIndices;
  size_t nextLineIndex = 0;
  int stopLine = -1;
  ConditionalDirectiveKind stopKind = ConditionalDirectiveKind::None;
};

static bool isConditionalBoundaryDirective(ConditionalDirectiveKind kind) {
  return kind == ConditionalDirectiveKind::Elif ||
         kind == ConditionalDirectiveKind::Else ||
         kind == ConditionalDirectiveKind::Endif;
}

static size_t appendLineNode(ConditionalAst &ast, int line) {
  ConditionalAstNode node;
  node.kind = ConditionalAstNode::Kind::Line;
  node.line = line;
  ast.nodes.push_back(std::move(node));
  return ast.nodes.size() - 1;
}

static size_t parseConditionalNode(ConditionalAst &ast, size_t &lineIndex);

static ParseSequenceResult parseSequence(ConditionalAst &ast, size_t lineIndex,
                                         bool stopAtConditionalBoundary) {
  ParseSequenceResult result;
  while (lineIndex < ast.lines.size()) {
    const ConditionalDirectiveKind kind = ast.lines[lineIndex].directiveKind;
    if (stopAtConditionalBoundary && isConditionalBoundaryDirective(kind)) {
      result.stopLine = static_cast<int>(lineIndex);
      result.stopKind = kind;
      break;
    }

    if (kind == ConditionalDirectiveKind::If ||
        kind == ConditionalDirectiveKind::Ifdef ||
        kind == ConditionalDirectiveKind::Ifndef) {
      result.nodeIndices.push_back(parseConditionalNode(ast, lineIndex));
      continue;
    }

    result.nodeIndices.push_back(
        appendLineNode(ast, static_cast<int>(lineIndex)));
    lineIndex++;
  }

  result.nextLineIndex = lineIndex;
  return result;
}

static size_t parseConditionalNode(ConditionalAst &ast, size_t &lineIndex) {
  ConditionalAstNode node;
  node.kind = ConditionalAstNode::Kind::Conditional;

  ConditionalAstBranch firstBranch;
  firstBranch.directiveLine = static_cast<int>(lineIndex);
  firstBranch.directiveKind = ast.lines[lineIndex].directiveKind;
  lineIndex++;

  ParseSequenceResult branchResult = parseSequence(ast, lineIndex, true);
  firstBranch.childNodeIndices = std::move(branchResult.nodeIndices);
  node.branches.push_back(std::move(firstBranch));
  lineIndex = branchResult.nextLineIndex;

  while (branchResult.stopKind == ConditionalDirectiveKind::Elif ||
         branchResult.stopKind == ConditionalDirectiveKind::Else) {
    ConditionalAstBranch branch;
    branch.directiveLine = branchResult.stopLine;
    branch.directiveKind = branchResult.stopKind;
    lineIndex++;

    branchResult = parseSequence(ast, lineIndex, true);
    branch.childNodeIndices = std::move(branchResult.nodeIndices);
    node.branches.push_back(std::move(branch));
    lineIndex = branchResult.nextLineIndex;
  }

  if (branchResult.stopKind == ConditionalDirectiveKind::Endif) {
    node.endifLine = branchResult.stopLine;
    lineIndex++;
  }

  ast.nodes.push_back(std::move(node));
  return ast.nodes.size() - 1;
}

} // namespace

ConditionalAst buildConditionalAst(const std::string &text) {
  ConditionalAst ast;

  std::istringstream stream(text);
  std::string lineText;
  bool inBlockComment = false;
  int lineIndex = 0;
  while (std::getline(stream, lineText)) {
    ConditionalAstLine line;
    line.line = lineIndex;
    line.text = lineText;
    bool maskBlock = inBlockComment;
    line.codeMask = buildCodeMaskForLine(lineText, maskBlock);
    inBlockComment = maskBlock;
    line.isDirective = isPreprocessorDirectiveLine(lineText, line.codeMask);

    const auto rawTokens = lexLineTokens(lineText);
    line.tokens.reserve(rawTokens.size());
    for (const auto &token : rawTokens) {
      if (token.start < line.codeMask.size() && line.codeMask[token.start])
        line.tokens.push_back(token);
    }

    if (line.isDirective)
      line.directiveKind = classifyDirectiveKind(line.tokens);

    ast.lines.push_back(std::move(line));
    lineIndex++;
  }

  ParseSequenceResult root = parseSequence(ast, 0, false);
  ast.rootNodeIndices = std::move(root.nodeIndices);
  return ast;
}
