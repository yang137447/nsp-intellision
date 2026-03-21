#pragma once

#include "nsf_lexer.hpp"

#include <cstddef>
#include <string>
#include <vector>

enum class ConditionalDirectiveKind {
  None,
  If,
  Ifdef,
  Ifndef,
  Elif,
  Else,
  Endif,
  Define,
  Undef,
  Include,
  Unknown,
};

struct ConditionalAstLine {
  int line = -1;
  std::string text;
  std::vector<char> codeMask;
  std::vector<LexToken> tokens;
  bool isDirective = false;
  ConditionalDirectiveKind directiveKind = ConditionalDirectiveKind::None;
};

struct ConditionalAstBranch {
  int directiveLine = -1;
  ConditionalDirectiveKind directiveKind = ConditionalDirectiveKind::None;
  std::vector<size_t> childNodeIndices;
};

struct ConditionalAstNode {
  enum class Kind { Line, Conditional };

  Kind kind = Kind::Line;
  int line = -1;
  std::vector<ConditionalAstBranch> branches;
  int endifLine = -1;
};

struct ConditionalAst {
  std::vector<ConditionalAstLine> lines;
  std::vector<ConditionalAstNode> nodes;
  std::vector<size_t> rootNodeIndices;
};

ConditionalAst buildConditionalAst(const std::string &text);

