#include "preprocessor_view.hpp"

#include "conditional_ast.hpp"
#include "include_resolver.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

struct PreprocMacro {
  bool functionLike = false;
  std::vector<LexToken> replacement;
};

static bool parseIntToken(const std::string &text, int &out) {
  if (text.empty())
    return false;
  try {
    size_t idx = 0;
    int value = std::stoi(text, &idx, 0);
    if (idx == 0)
      return false;
    out = value;
    return true;
  } catch (...) {
    return false;
  }
}

static PreprocMacro makeNumericPreprocMacro(int value) {
  const std::string text = std::to_string(value);
  PreprocMacro macro;
  macro.replacement.push_back(
      LexToken{LexToken::Kind::Identifier, text, 0, text.size()});
  return macro;
}

class PreprocessorExprParser {
public:
  PreprocessorExprParser(
      const std::vector<LexToken> &tokens, size_t start,
      const std::unordered_map<std::string, PreprocMacro> &macros, int line,
      std::vector<PreprocessorConditionDiagnostic> &diagnostics,
      std::unordered_set<std::string> expansionStack = {})
      : tokens_(tokens), i_(start), macros_(macros), line_(line),
        diagnostics_(diagnostics),
        expansionStack_(std::move(expansionStack)) {}

  int evaluate() { return parseLogicalOr(); }

private:
  const LexToken *peek() const {
    if (i_ >= tokens_.size())
      return nullptr;
    return &tokens_[i_];
  }

  const LexToken *consume() {
    if (i_ >= tokens_.size())
      return nullptr;
    return &tokens_[i_++];
  }

  bool matchPunct(const std::string &punct) {
    const LexToken *token = peek();
    if (!token || token->kind != LexToken::Kind::Punct ||
        token->text != punct) {
      return false;
    }
    consume();
    return true;
  }

  void addDiagnostic(const LexToken &token, const std::string &message) {
    diagnostics_.push_back(PreprocessorConditionDiagnostic{
        line_, static_cast<int>(token.start), static_cast<int>(token.end), 1,
        message});
  }

  void consumeFunctionLikeInvocation() {
    if (!matchPunct("("))
      return;
    int depth = 1;
    while (depth > 0) {
      const LexToken *token = consume();
      if (!token)
        break;
      if (token->kind != LexToken::Kind::Punct)
        continue;
      if (token->text == "(")
        depth++;
      else if (token->text == ")")
        depth--;
    }
  }

  int parseDefinedExpr() {
    consume();
    bool hasParen = matchPunct("(");
    std::string name;
    const LexToken *token = peek();
    if (token && token->kind == LexToken::Kind::Identifier) {
      name = token->text;
      consume();
    }
    if (hasParen)
      matchPunct(")");
    if (name.empty())
      return 0;
    return macros_.find(name) != macros_.end() ? 1 : 0;
  }

  int evaluateMacro(const LexToken &token) {
    auto it = macros_.find(token.text);
    if (it == macros_.end()) {
      addDiagnostic(token, "Undefined macro in preprocessor expression: " +
                               token.text + ".");
      return 0;
    }

    const PreprocMacro &macro = it->second;
    if (macro.functionLike) {
      addDiagnostic(token, "Function-like macro is not supported in "
                           "preprocessor expression: " +
                               token.text + ".");
      consumeFunctionLikeInvocation();
      return 0;
    }

    if (macro.replacement.empty())
      return 1;

    if (expansionStack_.find(token.text) != expansionStack_.end()) {
      addDiagnostic(token,
                    "Recursive macro expansion in preprocessor expression: " +
                        token.text + ".");
      return 0;
    }

    auto nestedExpansionStack = expansionStack_;
    nestedExpansionStack.insert(token.text);
    PreprocessorExprParser nested(macro.replacement, 0, macros_, line_,
                                  diagnostics_,
                                  std::move(nestedExpansionStack));
    return nested.evaluate();
  }

  int parsePrimary() {
    if (matchPunct("(")) {
      int value = parseLogicalOr();
      matchPunct(")");
      return value;
    }

    const LexToken *token = peek();
    if (!token)
      return 0;

    if (token->kind == LexToken::Kind::Identifier && token->text == "defined")
      return parseDefinedExpr();

    if (token->kind == LexToken::Kind::Identifier) {
      LexToken identifier = *consume();
      int parsed = 0;
      if (parseIntToken(identifier.text, parsed))
        return parsed;
      return evaluateMacro(identifier);
    }

    consume();
    return 0;
  }

  int parseUnary() {
    if (matchPunct("!"))
      return parseUnary() == 0 ? 1 : 0;
    if (matchPunct("+"))
      return parseUnary();
    if (matchPunct("-"))
      return -parseUnary();
    if (matchPunct("~"))
      return ~parseUnary();
    return parsePrimary();
  }

  int parseMultiplicative() {
    int left = parseUnary();
    while (true) {
      if (matchPunct("*")) {
        left *= parseUnary();
        continue;
      }
      if (matchPunct("/")) {
        int right = parseUnary();
        left = right == 0 ? 0 : left / right;
        continue;
      }
      if (matchPunct("%")) {
        int right = parseUnary();
        left = right == 0 ? 0 : left % right;
        continue;
      }
      break;
    }
    return left;
  }

  int parseAdditive() {
    int left = parseMultiplicative();
    while (true) {
      if (matchPunct("+")) {
        left += parseMultiplicative();
        continue;
      }
      if (matchPunct("-")) {
        left -= parseMultiplicative();
        continue;
      }
      break;
    }
    return left;
  }

  int parseShift() {
    int left = parseAdditive();
    while (true) {
      if (matchPunct("<<")) {
        left <<= parseAdditive();
        continue;
      }
      if (matchPunct(">>")) {
        left >>= parseAdditive();
        continue;
      }
      break;
    }
    return left;
  }

  int parseRelational() {
    int left = parseShift();
    while (true) {
      if (matchPunct("<=")) {
        left = left <= parseShift() ? 1 : 0;
        continue;
      }
      if (matchPunct(">=")) {
        left = left >= parseShift() ? 1 : 0;
        continue;
      }
      if (matchPunct("<")) {
        left = left < parseShift() ? 1 : 0;
        continue;
      }
      if (matchPunct(">")) {
        left = left > parseShift() ? 1 : 0;
        continue;
      }
      break;
    }
    return left;
  }

  int parseEquality() {
    int left = parseRelational();
    while (true) {
      if (matchPunct("==")) {
        left = left == parseRelational() ? 1 : 0;
        continue;
      }
      if (matchPunct("!=")) {
        left = left != parseRelational() ? 1 : 0;
        continue;
      }
      break;
    }
    return left;
  }

  int parseBitwiseAnd() {
    int left = parseEquality();
    while (matchPunct("&"))
      left &= parseEquality();
    return left;
  }

  int parseBitwiseXor() {
    int left = parseBitwiseAnd();
    while (matchPunct("^"))
      left ^= parseBitwiseAnd();
    return left;
  }

  int parseBitwiseOr() {
    int left = parseBitwiseXor();
    while (matchPunct("|"))
      left |= parseBitwiseXor();
    return left;
  }

  int parseLogicalAnd() {
    int left = parseBitwiseOr();
    while (matchPunct("&&"))
      left = (left != 0 && parseBitwiseOr() != 0) ? 1 : 0;
    return left;
  }

  int parseLogicalOr() {
    int left = parseLogicalAnd();
    while (matchPunct("||"))
      left = (left != 0 || parseLogicalAnd() != 0) ? 1 : 0;
    return left;
  }

  const std::vector<LexToken> &tokens_;
  size_t i_ = 0;
  const std::unordered_map<std::string, PreprocMacro> &macros_;
  int line_ = 0;
  std::vector<PreprocessorConditionDiagnostic> &diagnostics_;
  std::unordered_set<std::string> expansionStack_;
};

static int evalPreprocessorExpr(
    const std::vector<LexToken> &tokens, size_t start,
    const std::unordered_map<std::string, PreprocMacro> &macros, int line,
    std::vector<PreprocessorConditionDiagnostic> &diagnostics) {
  PreprocessorExprParser parser(tokens, start, macros, line, diagnostics);
  return parser.evaluate();
}

struct ActiveFrame {
  bool parentActive = true;
  bool currentActive = true;
  bool branchChosen = false;
};

struct BranchFrame {
  int id = 0;
  int branchIndex = 0;
  int nextBranchIndex = 1;
};

struct PreprocessorInterpreterState {
  const ConditionalAst &ast;
  std::unordered_map<std::string, PreprocMacro> macros;
  PreprocessorView result;
  std::vector<ActiveFrame> activeStack;
  std::vector<BranchFrame> branchStack;
  bool active = true;
  int nextBranchId = 1;
  const PreprocessorIncludeContext *includeContext = nullptr;
  std::string currentUri;
  int includeDepth = 0;
  std::unordered_map<std::string, ConditionalAst> *includeAstCache = nullptr;
  std::unordered_set<std::string> *includeExpansionStack = nullptr;
};

static void initializeLineStateStorage(PreprocessorInterpreterState &state) {
  state.result.lineActive.assign(state.ast.lines.size(), 0);
  state.result.branchSigs.resize(state.ast.lines.size());
}

static void normalizeDocumentTextInPlace(std::string &text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (char ch : text) {
    if (ch != '\r')
      normalized.push_back(ch);
  }
  text.swap(normalized);
}

static bool readTextFromDiskPath(const std::string &path, std::string &text) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::string content((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof())
    return false;
  text = std::move(content);
  normalizeDocumentTextInPlace(text);
  return true;
}

static bool loadIncludeDocumentText(PreprocessorInterpreterState &state,
                                    const std::string &uri,
                                    std::string &text) {
  text.clear();
  if (state.includeContext && state.includeContext->loadText) {
    if (state.includeContext->loadText(uri, text)) {
      normalizeDocumentTextInPlace(text);
      return true;
    }
  }

  std::string path = uriToPath(uri);
  if (path.empty())
    path = uri;
  if (path.empty())
    return false;
  return readTextFromDiskPath(path, text);
}

static bool loadIncludeConditionalAst(PreprocessorInterpreterState &state,
                                      const std::string &uri,
                                      const ConditionalAst *&astOut) {
  astOut = nullptr;
  if (uri.empty() || !state.includeAstCache)
    return false;
  auto cacheIt = state.includeAstCache->find(uri);
  if (cacheIt != state.includeAstCache->end()) {
    astOut = &cacheIt->second;
    return true;
  }

  std::string text;
  if (!loadIncludeDocumentText(state, uri, text))
    return false;

  auto inserted =
      state.includeAstCache->emplace(uri, buildConditionalAst(text));
  astOut = &inserted.first->second;
  return true;
}

static bool parseIncludePathFromDirective(const ConditionalAstLine &line,
                                          std::string &includePath) {
  includePath.clear();
  if (!line.isDirective || line.directiveKind != ConditionalDirectiveKind::Include)
    return false;
  if (line.tokens.size() < 3)
    return false;
  const size_t start = line.tokens[2].start;
  if (start >= line.text.size())
    return false;

  char open = line.text[start];
  char close = '\0';
  if (open == '"')
    close = '"';
  else if (open == '<')
    close = '>';
  else
    return false;

  const size_t end = line.text.find(close, start + 1);
  if (end == std::string::npos || end <= start + 1)
    return false;
  includePath = line.text.substr(start + 1, end - start - 1);
  return !includePath.empty();
}

static void writeLineState(PreprocessorInterpreterState &state, int line) {
  if (line < 0 || line >= static_cast<int>(state.result.lineActive.size()))
    return;
  PreprocBranchSig sig;
  sig.reserve(state.branchStack.size());
  for (const auto &frame : state.branchStack) {
    sig.push_back({frame.id, frame.branchIndex});
  }
  state.result.branchSigs[line] = std::move(sig);
  state.result.lineActive[line] = state.active ? 1 : 0;
}

static bool evaluateConditionalDirective(
    PreprocessorInterpreterState &state, const ConditionalAstLine &line) {
  if (!state.active)
    return false;

  if (line.directiveKind == ConditionalDirectiveKind::Ifdef ||
      line.directiveKind == ConditionalDirectiveKind::Ifndef) {
    if (line.tokens.size() < 3 ||
        line.tokens[2].kind != LexToken::Kind::Identifier) {
      return false;
    }
    const bool defined = state.macros.find(line.tokens[2].text) !=
                         state.macros.end();
    return line.directiveKind == ConditionalDirectiveKind::Ifdef ? defined
                                                                 : !defined;
  }

  return evalPreprocessorExpr(line.tokens, 2, state.macros, line.line,
                              state.result.conditionDiagnostics) != 0;
}

static void interpretNode(PreprocessorInterpreterState &state,
                          const ConditionalAstNode &node);
static void interpretNodeList(PreprocessorInterpreterState &state,
                              const std::vector<size_t> &nodeIndices);

static void interpretIncludeDirective(PreprocessorInterpreterState &state,
                                      const ConditionalAstLine &line) {
  if (!state.active || !state.includeContext)
    return;
  const int maxDepth =
      std::max(1, state.includeContext->maxDepth <= 0
                      ? 32
                      : state.includeContext->maxDepth);
  if (state.includeDepth >= maxDepth)
    return;
  if (state.currentUri.empty())
    return;

  std::string includePath;
  if (!parseIncludePathFromDirective(line, includePath))
    return;

  auto candidates =
      resolveIncludeCandidates(state.currentUri, includePath,
                               state.includeContext->workspaceFolders,
                               state.includeContext->includePaths,
                               state.includeContext->shaderExtensions);
  for (const auto &candidatePath : candidates) {
    std::string candidateUri = pathToUri(candidatePath);
    if (candidateUri.empty())
      candidateUri = candidatePath;
    if (candidateUri.empty())
      continue;
    if (state.includeExpansionStack &&
        state.includeExpansionStack->find(candidateUri) !=
            state.includeExpansionStack->end()) {
      continue;
    }

    const ConditionalAst *includeAst = nullptr;
    if (!loadIncludeConditionalAst(state, candidateUri, includeAst) ||
        !includeAst) {
      continue;
    }

    PreprocessorInterpreterState includeState{*includeAst};
    includeState.macros = state.macros;
    includeState.includeContext = state.includeContext;
    includeState.currentUri = candidateUri;
    includeState.includeDepth = state.includeDepth + 1;
    includeState.nextBranchId = state.nextBranchId;
    includeState.includeAstCache = state.includeAstCache;
    includeState.includeExpansionStack = state.includeExpansionStack;
    initializeLineStateStorage(includeState);

    if (includeState.includeExpansionStack)
      includeState.includeExpansionStack->insert(candidateUri);
    interpretNodeList(includeState, includeAst->rootNodeIndices);
    if (includeState.includeExpansionStack)
      includeState.includeExpansionStack->erase(candidateUri);

    state.macros = std::move(includeState.macros);
    if (state.includeContext->collectIncludeConditionDiagnostics &&
        !includeState.result.conditionDiagnostics.empty()) {
      state.result.conditionDiagnostics.insert(
          state.result.conditionDiagnostics.end(),
          includeState.result.conditionDiagnostics.begin(),
          includeState.result.conditionDiagnostics.end());
    }
    return;
  }
}

static void applyDirectiveSideEffects(PreprocessorInterpreterState &state,
                                      const ConditionalAstLine &line) {
  if (!state.active || !line.isDirective)
    return;

  if (line.directiveKind == ConditionalDirectiveKind::Define) {
    if (line.tokens.size() >= 3 &&
        line.tokens[2].kind == LexToken::Kind::Identifier) {
      std::string name = line.tokens[2].text;
      PreprocMacro macro;
      size_t replacementStart = 3;
      if (line.tokens.size() >= 4 &&
          line.tokens[3].kind == LexToken::Kind::Punct &&
          line.tokens[3].text == "(" &&
          line.tokens[2].end == line.tokens[3].start) {
        macro.functionLike = true;
        int depth = 0;
        size_t index = 3;
        for (; index < line.tokens.size(); index++) {
          const auto &token = line.tokens[index];
          if (token.kind != LexToken::Kind::Punct)
            continue;
          if (token.text == "(") {
            depth++;
            continue;
          }
          if (token.text == ")") {
            depth--;
            if (depth == 0) {
              index++;
              break;
            }
          }
        }
        replacementStart = index;
      }
      if (replacementStart < line.tokens.size()) {
        macro.replacement.assign(line.tokens.begin() + replacementStart,
                                 line.tokens.end());
      }
      state.macros[name] = std::move(macro);
    }
    return;
  }

  if (line.directiveKind == ConditionalDirectiveKind::Undef) {
    if (line.tokens.size() >= 3 &&
        line.tokens[2].kind == LexToken::Kind::Identifier) {
      state.macros.erase(line.tokens[2].text);
    }
    return;
  }

  if (line.directiveKind == ConditionalDirectiveKind::Include) {
    interpretIncludeDirective(state, line);
  }
}

static void interpretNodeList(PreprocessorInterpreterState &state,
                              const std::vector<size_t> &nodeIndices) {
  for (size_t nodeIndex : nodeIndices) {
    if (nodeIndex >= state.ast.nodes.size())
      continue;
    interpretNode(state, state.ast.nodes[nodeIndex]);
  }
}

static void interpretConditionalNode(PreprocessorInterpreterState &state,
                                     const ConditionalAstNode &node) {
  if (node.branches.empty())
    return;

  const ConditionalAstBranch &firstBranch = node.branches.front();
  writeLineState(state, firstBranch.directiveLine);

  BranchFrame branchFrame;
  branchFrame.id = state.nextBranchId++;
  branchFrame.branchIndex = 0;
  branchFrame.nextBranchIndex = 1;
  state.branchStack.push_back(branchFrame);

  ActiveFrame activeFrame;
  activeFrame.parentActive = state.active;
  activeFrame.currentActive = activeFrame.parentActive &&
                              evaluateConditionalDirective(
                                  state, state.ast.lines[firstBranch.directiveLine]);
  activeFrame.branchChosen = activeFrame.currentActive;
  state.activeStack.push_back(activeFrame);
  state.active = activeFrame.currentActive;

  interpretNodeList(state, firstBranch.childNodeIndices);

  for (size_t i = 1; i < node.branches.size(); i++) {
    const ConditionalAstBranch &branch = node.branches[i];
    writeLineState(state, branch.directiveLine);

    if (state.activeStack.empty() || state.branchStack.empty())
      break;

    BranchFrame &currentBranch = state.branchStack.back();
    currentBranch.branchIndex = currentBranch.nextBranchIndex;
    currentBranch.nextBranchIndex++;

    ActiveFrame &currentActive = state.activeStack.back();
    bool branchActive = false;
    if (branch.directiveKind == ConditionalDirectiveKind::Elif) {
      if (currentActive.parentActive && !currentActive.branchChosen) {
        branchActive = evaluateConditionalDirective(
            state, state.ast.lines[branch.directiveLine]);
        if (branchActive)
          currentActive.branchChosen = true;
      }
    } else if (branch.directiveKind == ConditionalDirectiveKind::Else) {
      branchActive = currentActive.parentActive && !currentActive.branchChosen;
      if (branchActive)
        currentActive.branchChosen = true;
    }
    currentActive.currentActive = branchActive;
    state.active = branchActive;

    interpretNodeList(state, branch.childNodeIndices);
  }

  if (node.endifLine >= 0)
    writeLineState(state, node.endifLine);

  bool parentActive = true;
  if (!state.activeStack.empty()) {
    parentActive = state.activeStack.back().parentActive;
    state.activeStack.pop_back();
  }
  state.active = parentActive;
  if (!state.branchStack.empty())
    state.branchStack.pop_back();
}

static void interpretNode(PreprocessorInterpreterState &state,
                          const ConditionalAstNode &node) {
  if (node.kind == ConditionalAstNode::Kind::Conditional) {
    interpretConditionalNode(state, node);
    return;
  }

  if (node.line < 0 || node.line >= static_cast<int>(state.ast.lines.size()))
    return;
  writeLineState(state, node.line);
  applyDirectiveSideEffects(state, state.ast.lines[node.line]);
}

} // namespace

PreprocessorView
buildPreprocessorView(const ConditionalAst &ast,
                      const std::unordered_map<std::string, int> &defines) {
  PreprocessorInterpreterState state{ast};
  initializeLineStateStorage(state);
  for (const auto &entry : defines) {
    state.macros[entry.first] = makeNumericPreprocMacro(entry.second);
  }

  interpretNodeList(state, ast.rootNodeIndices);
  return state.result;
}

PreprocessorView
buildPreprocessorView(const std::string &text,
                      const std::unordered_map<std::string, int> &defines) {
  return buildPreprocessorView(buildConditionalAst(text), defines);
}

PreprocessorView
buildPreprocessorView(const std::string &text,
                      const std::unordered_map<std::string, int> &defines,
                      const PreprocessorIncludeContext &includeContext) {
  const ConditionalAst ast = buildConditionalAst(text);
  PreprocessorInterpreterState state{ast};
  initializeLineStateStorage(state);
  for (const auto &entry : defines) {
    state.macros[entry.first] = makeNumericPreprocMacro(entry.second);
  }

  std::unordered_map<std::string, ConditionalAst> includeAstCache;
  std::unordered_set<std::string> includeExpansionStack;
  state.includeContext = &includeContext;
  state.currentUri = includeContext.currentUri;
  state.includeDepth = 0;
  state.includeAstCache = &includeAstCache;
  state.includeExpansionStack = &includeExpansionStack;
  if (!state.currentUri.empty() && state.includeExpansionStack) {
    state.includeExpansionStack->insert(state.currentUri);
  }
  interpretNodeList(state, ast.rootNodeIndices);
  if (!state.currentUri.empty() && state.includeExpansionStack) {
    state.includeExpansionStack->erase(state.currentUri);
  }
  return state.result;
}
