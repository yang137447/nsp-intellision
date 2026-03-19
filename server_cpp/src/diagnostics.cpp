#include "diagnostics.hpp"

#include "callsite_parser.hpp"
#include "definition_fallback.hpp"
#include "fast_ast.hpp"
#include "full_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_markdown.hpp"
#include "include_resolver.hpp"
#include "indeterminate_reasons.hpp"
#include "language_registry.hpp"
#include "lsp_helpers.hpp"
#include "macro_generated_functions.hpp"
#include "nsf_lexer.hpp"
#include "overload_resolver.hpp"
#include "semantic_cache.hpp"
#include "server_parse.hpp"
#include "signature_help.hpp"
#include "text_utils.hpp"
#include "type_desc.hpp"
#include "type_eval.hpp"
#include "type_model.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

static std::string getLineByIndex(const std::string &text, int lineIndex);

static std::string formatTypeList(const std::vector<std::string> &types) {
  std::ostringstream oss;
  oss << "(";
  for (size_t i = 0; i < types.size(); i++) {
    if (i > 0)
      oss << ", ";
    oss << types[i];
  }
  oss << ")";
  return oss.str();
}

static Json makeDiagnostic(const std::string &text, int line, int startByte,
                           int endByte, int severity, const std::string &source,
                           const std::string &message) {
  std::string lineText = getLineByIndex(text, line);
  int start = byteOffsetInLineToUtf16(lineText, startByte);
  int end = byteOffsetInLineToUtf16(lineText, endByte);
  if (end < start)
    end = start;
  Json diag = makeObject();
  diag.o["range"] = makeRangeExact(line, start, end);
  diag.o["severity"] = makeNumber(severity);
  diag.o["source"] = makeString(source);
  diag.o["message"] = makeString(message);
  return diag;
}

static Json makeDiagnosticWithCodeAndReason(
    const std::string &text, int line, int startByte, int endByte, int severity,
    const std::string &source, const std::string &message,
    const std::string &code, const std::string &reasonCode) {
  Json diag =
      makeDiagnostic(text, line, startByte, endByte, severity, source, message);
  diag.o["code"] = makeString(code);
  Json data = makeObject();
  data.o["reasonCode"] = makeString(reasonCode);
  diag.o["data"] = data;
  return diag;
}

static std::string
makeDefinesFingerprint(const std::unordered_map<std::string, int> &defines) {
  std::vector<std::pair<std::string, int>> ordered(defines.begin(),
                                                   defines.end());
  std::sort(
      ordered.begin(), ordered.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
  std::ostringstream oss;
  for (const auto &entry : ordered) {
    oss << entry.first << "=" << entry.second << ";";
  }
  return oss.str();
}

static bool findIncludePathSpan(const std::string &lineText, size_t includePos,
                                int &startOut, int &endOut) {
  size_t start = lineText.find('"', includePos);
  if (start != std::string::npos) {
    size_t end = lineText.find('"', start + 1);
    if (end != std::string::npos && end > start + 1) {
      startOut = static_cast<int>(start + 1);
      endOut = static_cast<int>(end);
      return true;
    }
  }
  start = lineText.find('<', includePos);
  if (start != std::string::npos) {
    size_t end = lineText.find('>', start + 1);
    if (end != std::string::npos && end > start + 1) {
      startOut = static_cast<int>(start + 1);
      endOut = static_cast<int>(end);
      return true;
    }
  }
  return false;
}

static size_t findIncludeDirectiveOutsideComments(const std::string &lineText,
                                                  bool &inBlockComment) {
  bool inString = false;
  for (size_t i = 0; i < lineText.size(); i++) {
    char ch = lineText[i];
    char next = (i + 1 < lineText.size()) ? lineText[i + 1] : '\0';

    if (inBlockComment) {
      if (ch == '*' && next == '/') {
        inBlockComment = false;
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
      break;
    }

    if (ch == '/' && next == '*') {
      inBlockComment = true;
      i++;
      continue;
    }

    if (ch == '#') {
      static const std::string includeToken = "#include";
      if (lineText.compare(i, includeToken.size(), includeToken) == 0) {
        return i;
      }
    }
  }
  return std::string::npos;
}

static bool hasUnterminatedBlockComment(const std::string &text, int &lineOut,
                                        int &charOut) {
  bool inString = false;
  bool inLineComment = false;
  bool inBlockComment = false;
  int line = 0;
  int character = 0;
  int lastBlockLine = -1;
  int lastBlockChar = -1;

  for (size_t i = 0; i < text.size(); i++) {
    char ch = text[i];
    char next = (i + 1 < text.size()) ? text[i + 1] : '\0';

    if (ch == '\n') {
      line++;
      character = 0;
      inLineComment = false;
      continue;
    }

    if (inString) {
      if (ch == '"' && (i == 0 || text[i - 1] != '\\'))
        inString = false;
      character++;
      continue;
    }

    if (inLineComment) {
      character++;
      continue;
    }

    if (inBlockComment) {
      if (ch == '*' && next == '/') {
        inBlockComment = false;
        i++;
        character += 2;
        continue;
      }
      character++;
      continue;
    }

    if (ch == '"') {
      inString = true;
      character++;
      continue;
    }
    if (ch == '/' && next == '/') {
      inLineComment = true;
      i++;
      character += 2;
      continue;
    }
    if (ch == '/' && next == '*') {
      inBlockComment = true;
      lastBlockLine = line;
      lastBlockChar = character;
      i++;
      character += 2;
      continue;
    }
    character++;
  }

  if (inBlockComment) {
    lineOut = lastBlockLine;
    charOut = lastBlockChar;
    return true;
  }
  return false;
}

static bool isWhitespace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

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
      mask[i] = 0;
      if (ch == '"' && (i == 0 || lineText[i - 1] != '\\'))
        inString = false;
      continue;
    }
    if (ch == '"') {
      mask[i] = 0;
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

enum class PreprocTri { False, True, Unknown };

static PreprocTri triFromInt(int value) {
  return value == 0 ? PreprocTri::False : PreprocTri::True;
}

static PreprocTri triNot(PreprocTri value) {
  if (value == PreprocTri::True)
    return PreprocTri::False;
  if (value == PreprocTri::False)
    return PreprocTri::True;
  return PreprocTri::Unknown;
}

static PreprocTri triAnd(PreprocTri left, PreprocTri right) {
  if (left == PreprocTri::False || right == PreprocTri::False)
    return PreprocTri::False;
  if (left == PreprocTri::True && right == PreprocTri::True)
    return PreprocTri::True;
  return PreprocTri::Unknown;
}

static PreprocTri triOr(PreprocTri left, PreprocTri right) {
  if (left == PreprocTri::True || right == PreprocTri::True)
    return PreprocTri::True;
  if (left == PreprocTri::False && right == PreprocTri::False)
    return PreprocTri::False;
  return PreprocTri::Unknown;
}

struct PreprocMacro {
  bool knownValue = false;
  int value = 0;
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

static PreprocTri evalPreprocessorExpr(
    const std::vector<LexToken> &tokens, size_t start,
    const std::unordered_map<std::string, PreprocMacro> &macros) {
  size_t i = start;

  auto peek = [&]() -> const LexToken * {
    if (i >= tokens.size())
      return nullptr;
    return &tokens[i];
  };

  auto consume = [&]() -> const LexToken * {
    if (i >= tokens.size())
      return nullptr;
    return &tokens[i++];
  };

  std::function<PreprocTri()> parsePrimary;
  std::function<PreprocTri()> parseUnary;
  std::function<PreprocTri()> parseAndExpr;
  std::function<PreprocTri()> parseOrExpr;

  parsePrimary = [&]() -> PreprocTri {
    const LexToken *t = peek();
    if (!t)
      return PreprocTri::Unknown;
    if (t->kind == LexToken::Kind::Punct && t->text == "(") {
      consume();
      PreprocTri inner = parseOrExpr();
      const LexToken *close = peek();
      if (close && close->kind == LexToken::Kind::Punct && close->text == ")")
        consume();
      return inner;
    }
    if (t->kind == LexToken::Kind::Identifier && t->text == "defined") {
      consume();
      const LexToken *next = peek();
      std::string name;
      if (next && next->kind == LexToken::Kind::Punct && next->text == "(") {
        consume();
        const LexToken *id = peek();
        if (id && id->kind == LexToken::Kind::Identifier) {
          name = id->text;
          consume();
        }
        const LexToken *close = peek();
        if (close && close->kind == LexToken::Kind::Punct && close->text == ")")
          consume();
      } else {
        const LexToken *id = peek();
        if (id && id->kind == LexToken::Kind::Identifier) {
          name = id->text;
          consume();
        }
      }
      if (name.empty())
        return PreprocTri::Unknown;
      return macros.find(name) != macros.end() ? PreprocTri::True
                                               : PreprocTri::False;
    }
    if (t->kind == LexToken::Kind::Identifier) {
      std::string name = t->text;
      consume();
      int parsed = 0;
      if (parseIntToken(name, parsed))
        return triFromInt(parsed);
      auto it = macros.find(name);
      if (it == macros.end())
        return PreprocTri::Unknown;
      if (!it->second.knownValue)
        return PreprocTri::Unknown;
      return triFromInt(it->second.value);
    }
    return PreprocTri::Unknown;
  };

  parseUnary = [&]() -> PreprocTri {
    const LexToken *t = peek();
    if (t && t->kind == LexToken::Kind::Punct && t->text == "!") {
      consume();
      return triNot(parseUnary());
    }
    return parsePrimary();
  };

  parseAndExpr = [&]() -> PreprocTri {
    PreprocTri left = parseUnary();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "&&")
        break;
      consume();
      PreprocTri right = parseUnary();
      left = triAnd(left, right);
    }
    return left;
  };

  parseOrExpr = [&]() -> PreprocTri {
    PreprocTri left = parseAndExpr();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "||")
        break;
      consume();
      PreprocTri right = parseAndExpr();
      left = triOr(left, right);
    }
    return left;
  };

  PreprocTri result = parseOrExpr();
  return result;
}

static std::vector<char>
computeActiveLineMask(const std::string &text,
                      const std::unordered_map<std::string, int> &defines) {
  std::unordered_map<std::string, PreprocMacro> macros;
  for (const auto &entry : defines) {
    macros[entry.first] = PreprocMacro{true, entry.second};
  }

  struct Frame {
    bool parentActive = true;
    bool currentActive = true;
    bool anyDefChosen = false;
    bool allPrevDefFalse = true;
  };

  std::vector<Frame> stack;
  bool active = true;

  std::istringstream stream(text);
  std::string lineText;
  bool inBlockComment = false;
  std::vector<char> lineActive;

  while (std::getline(stream, lineText)) {
    bool maskBlock = inBlockComment;
    const auto mask = buildCodeMaskForLine(lineText, maskBlock);
    inBlockComment = maskBlock;

    const auto rawTokens = lexLineTokens(lineText);
    std::vector<LexToken> tokens;
    tokens.reserve(rawTokens.size());
    for (const auto &token : rawTokens) {
      if (token.start < mask.size() && mask[token.start])
        tokens.push_back(token);
    }

    bool isDirective = isPreprocessorDirectiveLine(lineText, mask);
    if (!isDirective) {
      lineActive.push_back(active ? 1 : 0);
      continue;
    }

    lineActive.push_back(active ? 1 : 0);
    if (tokens.size() < 2)
      continue;
    if (tokens[0].kind != LexToken::Kind::Punct || tokens[0].text != "#")
      continue;
    if (tokens[1].kind != LexToken::Kind::Identifier)
      continue;

    const std::string directive = tokens[1].text;
    if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
      PreprocTri cond = PreprocTri::Unknown;
      if (directive == "ifdef" || directive == "ifndef") {
        if (tokens.size() >= 3 &&
            tokens[2].kind == LexToken::Kind::Identifier) {
          bool defined = macros.find(tokens[2].text) != macros.end();
          cond = defined ? PreprocTri::True : PreprocTri::False;
          if (directive == "ifndef")
            cond = triNot(cond);
        }
      } else {
        cond = evalPreprocessorExpr(tokens, 2, macros);
      }

      Frame frame;
      frame.parentActive = active;
      frame.anyDefChosen = false;
      frame.allPrevDefFalse = true;
      frame.currentActive = frame.parentActive && cond != PreprocTri::False;
      if (frame.parentActive && frame.allPrevDefFalse &&
          cond == PreprocTri::True) {
        frame.anyDefChosen = true;
      }
      frame.allPrevDefFalse =
          frame.allPrevDefFalse && cond == PreprocTri::False;
      stack.push_back(frame);
      active = frame.currentActive;
      continue;
    }

    if (directive == "elif") {
      if (stack.empty())
        continue;
      Frame &frame = stack.back();
      PreprocTri cond = evalPreprocessorExpr(tokens, 2, macros);
      bool eligible = frame.parentActive && !frame.anyDefChosen;
      bool branchActive = eligible && cond != PreprocTri::False;
      if (eligible && frame.allPrevDefFalse && cond == PreprocTri::True) {
        frame.anyDefChosen = true;
      }
      frame.allPrevDefFalse =
          frame.allPrevDefFalse && cond == PreprocTri::False;
      frame.currentActive = branchActive;
      active = frame.currentActive;
      continue;
    }

    if (directive == "else") {
      if (stack.empty())
        continue;
      Frame &frame = stack.back();
      bool eligible = frame.parentActive && !frame.anyDefChosen;
      bool branchActive = eligible;
      if (eligible && frame.allPrevDefFalse) {
        frame.anyDefChosen = true;
      }
      frame.allPrevDefFalse = false;
      frame.currentActive = branchActive;
      active = frame.currentActive;
      continue;
    }

    if (directive == "endif") {
      if (stack.empty())
        continue;
      Frame frame = stack.back();
      stack.pop_back();
      active = frame.parentActive;
      continue;
    }

    if (!active)
      continue;

    if (directive == "define") {
      if (tokens.size() >= 3 && tokens[2].kind == LexToken::Kind::Identifier) {
        std::string name = tokens[2].text;
        PreprocMacro macro;
        macro.knownValue = false;
        macro.value = 0;
        if (tokens.size() >= 4) {
          int parsed = 0;
          if (tokens[3].kind == LexToken::Kind::Identifier &&
              parseIntToken(tokens[3].text, parsed)) {
            macro.knownValue = true;
            macro.value = parsed;
          } else if (tokens[3].kind == LexToken::Kind::Punct &&
                     parseIntToken(tokens[3].text, parsed)) {
            macro.knownValue = true;
            macro.value = parsed;
          }
        }
        macros[name] = macro;
      }
      continue;
    }

    if (directive == "undef") {
      if (tokens.size() >= 3 && tokens[2].kind == LexToken::Kind::Identifier) {
        macros.erase(tokens[2].text);
      }
      continue;
    }
  }

  return lineActive;
}

using PreprocBranchSig = std::vector<std::pair<int, int>>;

static bool preprocBranchSigsOverlap(const PreprocBranchSig &a,
                                     const PreprocBranchSig &b) {
  size_t i = 0;
  size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i].first < b[j].first) {
      i++;
      continue;
    }
    if (b[j].first < a[i].first) {
      j++;
      continue;
    }
    if (a[i].second != b[j].second)
      return false;
    i++;
    j++;
  }
  return true;
}

static std::vector<PreprocBranchSig>
computePreprocessorBranchSigs(const std::string &text) {
  struct Frame {
    int id = 0;
    int branchIndex = 0;
    int nextBranchIndex = 1;
  };

  std::vector<Frame> stack;
  int nextId = 1;

  std::istringstream stream(text);
  std::string lineText;
  bool inBlockComment = false;
  std::vector<PreprocBranchSig> sigs;

  while (std::getline(stream, lineText)) {
    PreprocBranchSig sig;
    sig.reserve(stack.size());
    for (const auto &frame : stack) {
      sig.push_back({frame.id, frame.branchIndex});
    }
    sigs.push_back(sig);

    bool maskBlock = inBlockComment;
    const auto mask = buildCodeMaskForLine(lineText, maskBlock);
    inBlockComment = maskBlock;
    if (!isPreprocessorDirectiveLine(lineText, mask))
      continue;

    const auto rawTokens = lexLineTokens(lineText);
    std::vector<LexToken> tokens;
    tokens.reserve(rawTokens.size());
    for (const auto &token : rawTokens) {
      if (token.start < mask.size() && mask[token.start])
        tokens.push_back(token);
    }
    if (tokens.size() < 2)
      continue;
    if (tokens[0].kind != LexToken::Kind::Punct || tokens[0].text != "#")
      continue;
    if (tokens[1].kind != LexToken::Kind::Identifier)
      continue;
    const std::string directive = tokens[1].text;

    if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
      Frame frame;
      frame.id = nextId++;
      frame.branchIndex = 0;
      frame.nextBranchIndex = 1;
      stack.push_back(frame);
      continue;
    }
    if (directive == "elif") {
      if (stack.empty())
        continue;
      Frame &frame = stack.back();
      frame.branchIndex = frame.nextBranchIndex;
      frame.nextBranchIndex++;
      continue;
    }
    if (directive == "else") {
      if (stack.empty())
        continue;
      Frame &frame = stack.back();
      frame.branchIndex = frame.nextBranchIndex;
      frame.nextBranchIndex++;
      continue;
    }
    if (directive == "endif") {
      if (stack.empty())
        continue;
      stack.pop_back();
      continue;
    }
  }
  return sigs;
}

static std::string normalizeTypeToken(std::string value);
static bool isVectorType(const std::string &type, int &dimensionOut);
static bool isScalarType(const std::string &type);
static bool isMatrixType(const std::string &type, std::string &scalarOut,
                         int &rowsOut, int &colsOut);
static std::string makeVectorOrScalarType(const std::string &scalar, int dim);
static std::string makeMatrixType(const std::string &scalar, int rows,
                                  int cols);

enum class BuiltinElemKind { Unknown, Bool, Int, UInt, Half, Float };

struct BuiltinTypeInfo {
  enum class ShapeKind { Unknown, Scalar, Vector, Matrix };
  ShapeKind shape = ShapeKind::Unknown;
  BuiltinElemKind elem = BuiltinElemKind::Unknown;
  int dim = 0;
  int rows = 0;
  int cols = 0;
};

static BuiltinElemKind parseBuiltinElemKind(const std::string &t) {
  if (t == "bool")
    return BuiltinElemKind::Bool;
  if (t == "int")
    return BuiltinElemKind::Int;
  if (t == "uint")
    return BuiltinElemKind::UInt;
  if (t == "half")
    return BuiltinElemKind::Half;
  if (t == "float")
    return BuiltinElemKind::Float;
  return BuiltinElemKind::Unknown;
}

static std::string builtinElemKindToString(BuiltinElemKind k) {
  if (k == BuiltinElemKind::Bool)
    return "bool";
  if (k == BuiltinElemKind::Int)
    return "int";
  if (k == BuiltinElemKind::UInt)
    return "uint";
  if (k == BuiltinElemKind::Half)
    return "half";
  if (k == BuiltinElemKind::Float)
    return "float";
  return "";
}

static BuiltinTypeInfo parseBuiltinTypeInfo(std::string type) {
  type = normalizeTypeToken(type);
  BuiltinTypeInfo out;
  if (type.empty())
    return out;

  int dim = 0;
  if (isScalarType(type)) {
    out.shape = BuiltinTypeInfo::ShapeKind::Scalar;
    out.elem = parseBuiltinElemKind(type);
    out.dim = 1;
    return out;
  }
  if (isVectorType(type, dim)) {
    std::string base = type.substr(0, type.size() - 1);
    out.shape = BuiltinTypeInfo::ShapeKind::Vector;
    out.elem = parseBuiltinElemKind(base);
    out.dim = dim;
    return out;
  }
  std::string scalar;
  int rows = 0;
  int cols = 0;
  if (isMatrixType(type, scalar, rows, cols)) {
    out.shape = BuiltinTypeInfo::ShapeKind::Matrix;
    out.elem = parseBuiltinElemKind(scalar);
    out.rows = rows;
    out.cols = cols;
    return out;
  }
  return out;
}

static std::string builtinTypeInfoToString(const BuiltinTypeInfo &t) {
  const std::string base = builtinElemKindToString(t.elem);
  if (base.empty())
    return "";
  if (t.shape == BuiltinTypeInfo::ShapeKind::Scalar)
    return base;
  if (t.shape == BuiltinTypeInfo::ShapeKind::Vector)
    return makeVectorOrScalarType(base, t.dim);
  if (t.shape == BuiltinTypeInfo::ShapeKind::Matrix)
    return makeMatrixType(base, t.rows, t.cols);
  return "";
}

static bool isBuiltinNumericElem(BuiltinElemKind k) {
  return k == BuiltinElemKind::Int || k == BuiltinElemKind::UInt ||
         k == BuiltinElemKind::Half || k == BuiltinElemKind::Float;
}

static BuiltinElemKind promoteBuiltinNumericElem(BuiltinElemKind a,
                                                 BuiltinElemKind b,
                                                 bool &signednessMixOut) {
  signednessMixOut = false;
  if (a == BuiltinElemKind::Unknown || b == BuiltinElemKind::Unknown)
    return BuiltinElemKind::Unknown;
  if (!isBuiltinNumericElem(a) || !isBuiltinNumericElem(b))
    return BuiltinElemKind::Unknown;
  if (a == BuiltinElemKind::Float || b == BuiltinElemKind::Float)
    return BuiltinElemKind::Float;
  if (a == BuiltinElemKind::Half || b == BuiltinElemKind::Half)
    return BuiltinElemKind::Half;
  if ((a == BuiltinElemKind::Int && b == BuiltinElemKind::UInt) ||
      (a == BuiltinElemKind::UInt && b == BuiltinElemKind::Int)) {
    signednessMixOut = true;
    return BuiltinElemKind::Unknown;
  }
  return a;
}

static bool isBuiltinUnarySameType(const std::string &name) {
  return name == "normalize" || name == "saturate" || name == "abs" ||
         name == "sin" || name == "cos" || name == "tan" || name == "asin" ||
         name == "acos" || name == "atan" || name == "exp" || name == "exp2" ||
         name == "floor" || name == "ceil" || name == "frac" ||
         name == "fmod" || name == "rsqrt" || name == "sqrt" || name == "sign";
}

struct BuiltinResolveResult {
  bool ok = false;
  bool warnMixedSignedness = false;
  bool indeterminate = false;
  BuiltinTypeInfo ret;
};

static BuiltinResolveResult
resolveBuiltinCall(const std::string &name,
                   const std::vector<BuiltinTypeInfo> &args) {
  BuiltinResolveResult r;
  if (args.empty())
    return r;
  for (const auto &a : args) {
    if (a.shape == BuiltinTypeInfo::ShapeKind::Unknown ||
        a.elem == BuiltinElemKind::Unknown) {
      r.indeterminate = true;
      return r;
    }
  }

  auto unifyElem = [&](BuiltinElemKind &outElem) -> bool {
    outElem = args[0].elem;
    for (size_t i = 1; i < args.size(); i++) {
      bool mix = false;
      BuiltinElemKind promoted =
          promoteBuiltinNumericElem(outElem, args[i].elem, mix);
      if (mix) {
        r.warnMixedSignedness = true;
        return false;
      }
      if (promoted == BuiltinElemKind::Unknown)
        return false;
      outElem = promoted;
    }
    return true;
  };
  auto exactShapeEq = [&](const BuiltinTypeInfo &a,
                          const BuiltinTypeInfo &b) -> bool {
    if (a.shape != b.shape)
      return false;
    if (a.shape == BuiltinTypeInfo::ShapeKind::Vector)
      return a.dim == b.dim;
    if (a.shape == BuiltinTypeInfo::ShapeKind::Matrix)
      return a.rows == b.rows && a.cols == b.cols;
    return true;
  };
  auto scalarOrExactMatch = [&](const BuiltinTypeInfo &a,
                                const BuiltinTypeInfo &ref) -> bool {
    if (a.shape == BuiltinTypeInfo::ShapeKind::Scalar)
      return true;
    return exactShapeEq(a, ref);
  };

  if (name == "length" || name == "distance") {
    if (args.size() < 1)
      return r;
    if (!isBuiltinNumericElem(args[0].elem))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (name == "length") {
      outElem = args[0].elem;
    } else {
      if (args.size() < 2)
        return r;
      if (!isBuiltinNumericElem(args[1].elem))
        return r;
      if (!exactShapeEq(args[0], args[1]))
        return r;
      if (!unifyElem(outElem))
        return r;
    }
    r.ok = true;
    r.ret.shape = BuiltinTypeInfo::ShapeKind::Scalar;
    r.ret.elem = outElem;
    r.ret.dim = 1;
    return r;
  }

  if (name == "dot") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    bool okShape = false;
    if (args[0].shape == BuiltinTypeInfo::ShapeKind::Vector &&
        args[1].shape == BuiltinTypeInfo::ShapeKind::Vector &&
        args[0].dim == args[1].dim) {
      okShape = true;
    }
    if (args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar &&
        args[1].shape == BuiltinTypeInfo::ShapeKind::Scalar) {
      okShape = true;
    }
    if (!okShape)
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret.shape = BuiltinTypeInfo::ShapeKind::Scalar;
    r.ret.elem = outElem;
    r.ret.dim = 1;
    return r;
  }

  if (name == "cross") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    if (args[0].shape != BuiltinTypeInfo::ShapeKind::Vector ||
        args[1].shape != BuiltinTypeInfo::ShapeKind::Vector)
      return r;
    if (args[0].dim != 3 || args[1].dim != 3)
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret.shape = BuiltinTypeInfo::ShapeKind::Vector;
    r.ret.elem = outElem;
    r.ret.dim = 3;
    return r;
  }

  if (isBuiltinUnarySameType(name)) {
    if (args.size() < 1)
      return r;
    if (!isBuiltinNumericElem(args[0].elem))
      return r;
    r.ok = true;
    r.ret = args[0];
    return r;
  }

  if (name == "asfloat" || name == "asint" || name == "asuint") {
    if (args.size() != 1)
      return r;
    if (args[0].elem != BuiltinElemKind::Float &&
        args[0].elem != BuiltinElemKind::Half &&
        args[0].elem != BuiltinElemKind::Int &&
        args[0].elem != BuiltinElemKind::UInt) {
      return r;
    }
    r.ok = true;
    r.ret = args[0];
    if (name == "asfloat")
      r.ret.elem = BuiltinElemKind::Float;
    else if (name == "asint")
      r.ret.elem = BuiltinElemKind::Int;
    else if (name == "asuint")
      r.ret.elem = BuiltinElemKind::UInt;
    return r;
  }

  if (name == "countbits" || name == "firstbithigh" || name == "firstbitlow" ||
      name == "reversebits") {
    if (args.size() != 1)
      return r;
    if (args[0].elem != BuiltinElemKind::Int &&
        args[0].elem != BuiltinElemKind::UInt) {
      return r;
    }
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = BuiltinElemKind::Int;
    return r;
  }

  if (name == "f16tof32") {
    if (args.size() != 1)
      return r;
    if (args[0].elem != BuiltinElemKind::UInt)
      return r;
    if (args[0].shape == BuiltinTypeInfo::ShapeKind::Matrix)
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = BuiltinElemKind::Float;
    return r;
  }

  if (name == "f32tof16") {
    if (args.size() != 1)
      return r;
    if (!isBuiltinNumericElem(args[0].elem))
      return r;
    if (args[0].shape == BuiltinTypeInfo::ShapeKind::Matrix)
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = BuiltinElemKind::UInt;
    return r;
  }

  if (name == "rcp") {
    if (args.size() != 1)
      return r;
    if (!isBuiltinNumericElem(args[0].elem))
      return r;
    r.ok = true;
    r.ret = args[0];
    if (r.ret.elem != BuiltinElemKind::Half &&
        r.ret.elem != BuiltinElemKind::Float) {
      r.ret.elem = BuiltinElemKind::Float;
    }
    return r;
  }

  if (name == "mad") {
    if (args.size() != 3)
      return r;
    for (const auto &a : args) {
      if (!isBuiltinNumericElem(a.elem))
        return r;
    }
    BuiltinTypeInfo ref = args[0];
    if (args[1].shape != BuiltinTypeInfo::ShapeKind::Scalar)
      ref = args[1];
    if (args[2].shape != BuiltinTypeInfo::ShapeKind::Scalar)
      ref = args[2];
    if (!scalarOrExactMatch(args[0], ref) ||
        !scalarOrExactMatch(args[1], ref) || !scalarOrExactMatch(args[2], ref))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = ref;
    r.ret.elem = outElem;
    return r;
  }

  if (name == "pow") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    if (!exactShapeEq(args[0], args[1]) &&
        !(args[0].shape == BuiltinTypeInfo::ShapeKind::Vector &&
          args[1].shape == BuiltinTypeInfo::ShapeKind::Scalar) &&
        !(args[1].shape == BuiltinTypeInfo::ShapeKind::Vector &&
          args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret =
        args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar ? args[1] : args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "atan2") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    if (!exactShapeEq(args[0], args[1]) &&
        !(args[0].shape == BuiltinTypeInfo::ShapeKind::Vector &&
          args[1].shape == BuiltinTypeInfo::ShapeKind::Scalar) &&
        !(args[1].shape == BuiltinTypeInfo::ShapeKind::Vector &&
          args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret =
        args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar ? args[1] : args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "reflect") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    if (!exactShapeEq(args[0], args[1]))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "refract") {
    if (args.size() < 3)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem) ||
        !isBuiltinNumericElem(args[2].elem))
      return r;
    if (!exactShapeEq(args[0], args[1]))
      return r;
    if (args[2].shape != BuiltinTypeInfo::ShapeKind::Scalar)
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "clamp") {
    if (args.size() < 3)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem) ||
        !isBuiltinNumericElem(args[2].elem))
      return r;
    if (!scalarOrExactMatch(args[1], args[0]))
      return r;
    if (!scalarOrExactMatch(args[2], args[0]))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "lerp") {
    if (args.size() < 3)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem) ||
        !isBuiltinNumericElem(args[2].elem))
      return r;
    if (!exactShapeEq(args[0], args[1]))
      return r;
    if (!(args[2].shape == BuiltinTypeInfo::ShapeKind::Scalar ||
          exactShapeEq(args[2], args[0])))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "step") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    if (!scalarOrExactMatch(args[0], args[1]))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[1];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "smoothstep") {
    if (args.size() < 3)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem) ||
        !isBuiltinNumericElem(args[2].elem))
      return r;
    if (!scalarOrExactMatch(args[0], args[2]))
      return r;
    if (!scalarOrExactMatch(args[1], args[2]))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[2];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "min" || name == "max") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    BuiltinTypeInfo::ShapeKind outShape = args[0].shape;
    if (args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar)
      outShape = args[1].shape;
    else if (args[1].shape == BuiltinTypeInfo::ShapeKind::Scalar)
      outShape = args[0].shape;
    else if (!exactShapeEq(args[0], args[1]))
      return r;

    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = outShape == BuiltinTypeInfo::ShapeKind::Scalar ? args[0] : args[0];
    r.ret.shape = outShape;
    r.ret.elem = outElem;
    return r;
  }

  if (name == "mul" && args.size() >= 2) {
    const auto &a = args[0];
    const auto &b = args[1];
    if (!isBuiltinNumericElem(a.elem) || !isBuiltinNumericElem(b.elem))
      return r;
    bool mix = false;
    BuiltinElemKind outElem = promoteBuiltinNumericElem(a.elem, b.elem, mix);
    if (mix) {
      r.warnMixedSignedness = true;
      return r;
    }
    if (outElem == BuiltinElemKind::Unknown)
      return r;

    if (a.shape == BuiltinTypeInfo::ShapeKind::Matrix &&
        b.shape == BuiltinTypeInfo::ShapeKind::Matrix) {
      if (a.cols != b.rows)
        return r;
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Matrix;
      r.ret.elem = outElem;
      r.ret.rows = a.rows;
      r.ret.cols = b.cols;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Vector &&
        b.shape == BuiltinTypeInfo::ShapeKind::Matrix) {
      if (a.dim != b.rows)
        return r;
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Vector;
      r.ret.elem = outElem;
      r.ret.dim = b.cols;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Matrix &&
        b.shape == BuiltinTypeInfo::ShapeKind::Vector) {
      if (a.cols != b.dim)
        return r;
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Vector;
      r.ret.elem = outElem;
      r.ret.dim = a.rows;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Matrix &&
        b.shape == BuiltinTypeInfo::ShapeKind::Scalar) {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Matrix;
      r.ret.elem = outElem;
      r.ret.rows = a.rows;
      r.ret.cols = a.cols;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Scalar &&
        b.shape == BuiltinTypeInfo::ShapeKind::Matrix) {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Matrix;
      r.ret.elem = outElem;
      r.ret.rows = b.rows;
      r.ret.cols = b.cols;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Vector &&
        b.shape == BuiltinTypeInfo::ShapeKind::Scalar) {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Vector;
      r.ret.elem = outElem;
      r.ret.dim = a.dim;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Scalar &&
        b.shape == BuiltinTypeInfo::ShapeKind::Vector) {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Vector;
      r.ret.elem = outElem;
      r.ret.dim = b.dim;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Scalar &&
        b.shape == BuiltinTypeInfo::ShapeKind::Scalar) {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Scalar;
      r.ret.elem = outElem;
      r.ret.dim = 1;
      return r;
    }
  }

  return r;
}

static std::string normalizeTypeToken(std::string value) {
  while (!value.empty() && isWhitespace(value.front()))
    value.erase(value.begin());
  while (!value.empty() && isWhitespace(value.back()))
    value.pop_back();
  return value;
}

static bool isVectorType(const std::string &type, int &dimensionOut) {
  if (type.size() < 2)
    return false;
  size_t digitPos = type.size() - 1;
  if (!std::isdigit(static_cast<unsigned char>(type[digitPos])))
    return false;
  int dim = type[digitPos] - '0';
  if (dim < 2 || dim > 4)
    return false;
  std::string base = type.substr(0, digitPos);
  if (base == "float" || base == "half" || base == "int" || base == "uint" ||
      base == "bool") {
    dimensionOut = dim;
    return true;
  }
  return false;
}

static bool isScalarType(const std::string &type) {
  return type == "float" || type == "half" || type == "int" || type == "uint" ||
         type == "bool";
}

static bool isNumericScalarType(const std::string &type) {
  return type == "float" || type == "half" || type == "int" || type == "uint";
}

static bool isMatrixType(const std::string &type, std::string &scalarOut,
                         int &rowsOut, int &colsOut) {
  scalarOut.clear();
  rowsOut = 0;
  colsOut = 0;
  if (type.size() < 4)
    return false;
  size_t xPos = type.find('x');
  if (xPos == std::string::npos)
    return false;
  if (xPos == 0 || xPos + 1 >= type.size())
    return false;
  if (!std::isdigit(static_cast<unsigned char>(type[xPos - 1])) ||
      !std::isdigit(static_cast<unsigned char>(type[xPos + 1])))
    return false;
  int rows = type[xPos - 1] - '0';
  int cols = type[xPos + 1] - '0';
  if (rows < 1 || rows > 4 || cols < 1 || cols > 4)
    return false;
  std::string base = type.substr(0, xPos - 1);
  if (base != "float" && base != "half" && base != "int" && base != "uint")
    return false;
  if (xPos + 2 != type.size())
    return false;
  scalarOut = base;
  rowsOut = rows;
  colsOut = cols;
  return true;
}

static bool isMatrixType(const std::string &type, int &rowsOut, int &colsOut) {
  std::string scalar;
  return isMatrixType(type, scalar, rowsOut, colsOut);
}

static bool isNumericMatrixType(const std::string &type) {
  std::string scalar;
  int r = 0;
  int c = 0;
  if (!isMatrixType(type, scalar, r, c))
    return false;
  return isNumericScalarType(scalar);
}

static std::string makeMatrixType(const std::string &scalar, int rows,
                                  int cols) {
  if (scalar.empty() || rows <= 0 || cols <= 0)
    return "";
  return scalar + std::to_string(rows) + "x" + std::to_string(cols);
}

static std::string inferLiteralType(const std::string &token) {
  if (token == "true" || token == "false")
    return "bool";
  if (token.empty())
    return "";
  auto parseHexIntegerTokenSuffix = [](const std::string &value,
                                       char &suffixOut,
                                       bool &invalidSuffixOut) -> bool {
    suffixOut = 0;
    invalidSuffixOut = false;
    if (value.empty())
      return false;
    size_t start = 0;
    if (value[0] == '+' || value[0] == '-')
      start = 1;
    if (start + 2 >= value.size())
      return false;
    if (value[start] != '0' ||
        (value[start + 1] != 'x' && value[start + 1] != 'X'))
      return false;
    size_t i = start + 2;
    const size_t digitsStart = i;
    while (i < value.size() &&
           std::isxdigit(static_cast<unsigned char>(value[i]))) {
      i++;
    }
    if (i == digitsStart)
      return false;
    if (i == value.size())
      return true;
    if (i + 1 == value.size() &&
        std::isalpha(static_cast<unsigned char>(value[i]))) {
      suffixOut = value[i];
      invalidSuffixOut = !(suffixOut == 'u' || suffixOut == 'U');
      return true;
    }
    return false;
  };
  {
    char suffix = 0;
    bool invalid = false;
    if (parseHexIntegerTokenSuffix(token, suffix, invalid)) {
      if (suffix == 'u' || suffix == 'U')
        return "uint";
      return "int";
    }
  }
  bool hasDigit = false;
  bool hasDot = false;
  size_t start = 0;
  if (token[0] == '+' || token[0] == '-')
    start = 1;
  for (size_t i = start; i < token.size(); i++) {
    unsigned char ch = static_cast<unsigned char>(token[i]);
    if (std::isdigit(ch)) {
      hasDigit = true;
      continue;
    }
    if (token[i] == '.') {
      hasDot = true;
      continue;
    }
    return "";
  }
  if (!hasDigit)
    return "";
  return hasDot ? "float" : "int";
}

static bool isSignedDigitsToken(const std::string &token) {
  if (token.empty())
    return false;
  size_t start = 0;
  if (token[0] == '+' || token[0] == '-')
    start = 1;
  if (start >= token.size())
    return false;
  for (size_t i = start; i < token.size(); i++) {
    if (!std::isdigit(static_cast<unsigned char>(token[i])))
      return false;
  }
  return true;
}

static bool isSignedDigitsWithOptionalSuffixToken(const std::string &token,
                                                  char &suffixOut) {
  suffixOut = 0;
  if (token.empty())
    return false;
  size_t start = 0;
  if (token[0] == '+' || token[0] == '-')
    start = 1;
  if (start >= token.size())
    return false;
  size_t i = start;
  while (i < token.size() &&
         std::isdigit(static_cast<unsigned char>(token[i]))) {
    i++;
  }
  if (i == start)
    return false;
  if (i == token.size())
    return true;
  if (i + 1 == token.size()) {
    char suf = token[i];
    if (suf == 'h' || suf == 'f' || suf == 'F' || suf == 'u' || suf == 'U') {
      suffixOut = suf;
      return true;
    }
  }
  return false;
}

static bool isSignedDigitsWithSingleAlphaSuffixToken(const std::string &token,
                                                     char &suffixOut) {
  suffixOut = 0;
  if (token.empty())
    return false;
  size_t start = 0;
  if (token[0] == '+' || token[0] == '-')
    start = 1;
  if (start >= token.size())
    return false;
  size_t i = start;
  while (i < token.size() &&
         std::isdigit(static_cast<unsigned char>(token[i]))) {
    i++;
  }
  if (i == start)
    return false;
  if (i + 1 != token.size())
    return false;
  unsigned char suf = static_cast<unsigned char>(token[i]);
  if (!std::isalpha(suf))
    return false;
  suffixOut = token[i];
  return true;
}

static size_t numericLiteralTokenSpan(const std::vector<LexToken> &tokens,
                                      size_t index) {
  if (index >= tokens.size())
    return 0;
  if (tokens[index].kind != LexToken::Kind::Identifier)
    return 0;
  {
    char suffix = 0;
    bool invalid = false;
    const std::string &value = tokens[index].text;
    size_t start = 0;
    if (!value.empty() && (value[0] == '+' || value[0] == '-'))
      start = 1;
    if (start + 2 < value.size() && value[start] == '0' &&
        (value[start + 1] == 'x' || value[start + 1] == 'X')) {
      size_t i = start + 2;
      const size_t digitsStart = i;
      while (i < value.size() &&
             std::isxdigit(static_cast<unsigned char>(value[i]))) {
        i++;
      }
      if (i > digitsStart) {
        if (i == value.size())
          return 1;
        if (i + 1 == value.size() &&
            std::isalpha(static_cast<unsigned char>(value[i]))) {
          suffix = value[i];
          invalid = !(suffix == 'u' || suffix == 'U');
          return invalid ? 0 : 1;
        }
      }
    }
  }
  char suffix0 = 0;
  if (!isSignedDigitsWithOptionalSuffixToken(tokens[index].text, suffix0))
    return 0;
  if (suffix0 != 0)
    return 1;
  if (index + 2 < tokens.size() &&
      tokens[index + 1].kind == LexToken::Kind::Punct &&
      tokens[index + 1].text == "." &&
      tokens[index + 2].kind == LexToken::Kind::Identifier) {
    char suffix1 = 0;
    if (isSignedDigitsWithOptionalSuffixToken(tokens[index + 2].text, suffix1))
      return 3;
  }
  return 1;
}

static std::string
inferNumericLiteralTypeFromTokens(const std::vector<LexToken> &tokens,
                                  size_t index) {
  if (index >= tokens.size())
    return "";
  if (tokens[index].kind != LexToken::Kind::Identifier)
    return "";
  {
    const std::string &value = tokens[index].text;
    size_t start = 0;
    if (!value.empty() && (value[0] == '+' || value[0] == '-'))
      start = 1;
    if (start + 2 < value.size() && value[start] == '0' &&
        (value[start + 1] == 'x' || value[start + 1] == 'X')) {
      size_t i = start + 2;
      const size_t digitsStart = i;
      while (i < value.size() &&
             std::isxdigit(static_cast<unsigned char>(value[i]))) {
        i++;
      }
      if (i > digitsStart) {
        if (i == value.size())
          return "int";
        if (i + 1 == value.size() && (value[i] == 'u' || value[i] == 'U'))
          return "uint";
        return "int";
      }
    }
  }
  char suffix0 = 0;
  if (!isSignedDigitsWithOptionalSuffixToken(tokens[index].text, suffix0))
    return "";
  if (index + 2 < tokens.size() &&
      tokens[index + 1].kind == LexToken::Kind::Punct &&
      tokens[index + 1].text == "." &&
      tokens[index + 2].kind == LexToken::Kind::Identifier && true) {
    char suffix1 = 0;
    if (!isSignedDigitsWithOptionalSuffixToken(tokens[index + 2].text,
                                               suffix1)) {
      char badSuffix = 0;
      if (isSignedDigitsWithSingleAlphaSuffixToken(tokens[index + 2].text,
                                                   badSuffix))
        return "float";
      return "int";
    }
    if (suffix1 == 'h')
      return "half";
    return "float";
  }
  if (suffix0 == 'h')
    return "half";
  if (suffix0 == 'f' || suffix0 == 'F')
    return "float";
  if (suffix0 == 'u' || suffix0 == 'U')
    return "uint";
  return "int";
}

static bool parseVectorOrScalarType(const std::string &type,
                                    std::string &scalarOut, int &dimensionOut) {
  int dim = 0;
  if (isVectorType(type, dim)) {
    dimensionOut = dim;
    scalarOut = type.substr(0, type.size() - 1);
    return true;
  }
  if (isScalarType(type)) {
    dimensionOut = 1;
    scalarOut = type;
    return true;
  }
  return false;
}

static bool isNarrowingPrecisionAssignment(const std::string &lhsType,
                                           const std::string &rhsType) {
  std::string lhsScalar;
  std::string rhsScalar;
  int lhsDim = 0;
  int rhsDim = 0;
  if (parseVectorOrScalarType(lhsType, lhsScalar, lhsDim) &&
      parseVectorOrScalarType(rhsType, rhsScalar, rhsDim) && lhsDim == rhsDim) {
    return lhsScalar == "half" &&
           (rhsScalar == "float" || rhsScalar == "double");
  }
  std::string lhsMatScalar;
  std::string rhsMatScalar;
  int lhsRows = 0;
  int lhsCols = 0;
  int rhsRows = 0;
  int rhsCols = 0;
  if (isMatrixType(lhsType, lhsMatScalar, lhsRows, lhsCols) &&
      isMatrixType(rhsType, rhsMatScalar, rhsRows, rhsCols) &&
      lhsRows == rhsRows && lhsCols == rhsCols) {
    return lhsMatScalar == "half" &&
           (rhsMatScalar == "float" || rhsMatScalar == "double");
  }
  return false;
}

static bool isHalfFamilyType(const std::string &type) {
  std::string scalar;
  int dim = 0;
  if (parseVectorOrScalarType(type, scalar, dim))
    return scalar == "half";
  std::string matScalar;
  int rows = 0;
  int cols = 0;
  if (isMatrixType(type, matScalar, rows, cols))
    return matScalar == "half";
  return false;
}

static std::string
inferNarrowingFallbackRhsTypeFromTokens(const std::vector<LexToken> &tokens,
                                        size_t startIndex, size_t endIndex) {
  endIndex = std::min(endIndex, tokens.size());
  bool sawFloatLiteral = false;
  bool sawHalfLiteral = false;
  for (size_t i = startIndex; i < endIndex; i++) {
    const std::string literal = inferLiteralType(tokens[i].text);
    if (literal == "float")
      sawFloatLiteral = true;
    else if (literal == "half")
      sawHalfLiteral = true;
    char suffix = 0;
    if (isSignedDigitsWithOptionalSuffixToken(tokens[i].text, suffix)) {
      if (suffix == 'f' || suffix == 'F')
        sawFloatLiteral = true;
      else if (suffix == 'h')
        sawHalfLiteral = true;
    }
    if (sawFloatLiteral)
      return "float";
  }
  if (sawHalfLiteral)
    return "half";
  return "";
}

static std::string makeVectorOrScalarType(const std::string &scalar, int dim) {
  if (scalar.empty())
    return "";
  if (dim <= 1)
    return scalar;
  return scalar + std::to_string(dim);
}

static bool isSwizzleToken(const std::string &token) {
  if (token.empty() || token.size() > 4)
    return false;
  for (char ch : token) {
    if (ch == 'x' || ch == 'y' || ch == 'z' || ch == 'w' || ch == 'r' ||
        ch == 'g' || ch == 'b' || ch == 'a')
      continue;
    return false;
  }
  return true;
}

static std::string applySwizzleType(const std::string &baseType,
                                    const std::string &swizzle) {
  if (!isSwizzleToken(swizzle))
    return "";
  std::string scalar;
  int dim = 0;
  if (!parseVectorOrScalarType(baseType, scalar, dim)) {
    scalar = "float";
  }
  return makeVectorOrScalarType(scalar, static_cast<int>(swizzle.size()));
}

static std::string applyIndexAccessType(const std::string &baseType) {
  const std::string normalized = normalizeTypeToken(baseType);
  if (normalized.empty())
    return normalized;

  std::string matrixScalar;
  int rows = 0;
  int cols = 0;
  if (isMatrixType(normalized, matrixScalar, rows, cols)) {
    return makeVectorOrScalarType(matrixScalar, cols);
  }

  int vecDim = 0;
  if (isVectorType(normalized, vecDim)) {
    return normalized.substr(0, normalized.size() - 1);
  }

  if (!normalized.empty() && normalized.back() == ']') {
    int depth = 0;
    for (size_t pos = normalized.size(); pos-- > 0;) {
      const char ch = normalized[pos];
      if (ch == ']') {
        depth++;
        continue;
      }
      if (ch == '[') {
        depth--;
        if (depth == 0)
          return normalizeTypeToken(normalized.substr(0, pos));
      }
    }
  }

  return normalized;
}

static bool readFileToString(const std::string &path, std::string &out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  out = buffer.str();
  return true;
}

static bool parseStructMembersFromText(
    const std::string &text, const std::string &structName,
    std::unordered_map<std::string, std::string> &membersOut) {
  std::istringstream stream(text);
  std::string lineText;
  bool inStruct = false;
  int braceDepth = 0;
  bool inBlockComment = false;
  while (std::getline(stream, lineText)) {
    bool maskBlock = inBlockComment;
    const auto mask = buildCodeMaskForLine(lineText, maskBlock);
    inBlockComment = maskBlock;
    const auto rawTokens = lexLineTokens(lineText);
    std::vector<LexToken> tokens;
    tokens.reserve(rawTokens.size());
    for (const auto &token : rawTokens) {
      if (token.start < mask.size() && mask[token.start])
        tokens.push_back(token);
    }
    if (tokens.empty())
      continue;

    if (!inStruct) {
      std::string trimmed = trimLeftCopy(lineText);
      if (trimmed.rfind("struct", 0) != 0)
        continue;
      bool sawName = false;
      for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Identifier &&
            tokens[i].text == "struct" &&
            tokens[i + 1].kind == LexToken::Kind::Identifier &&
            tokens[i + 1].text == structName) {
          sawName = true;
          break;
        }
      }
      if (!sawName)
        continue;
      inStruct = true;
      for (const auto &t : tokens) {
        if (t.kind != LexToken::Kind::Punct)
          continue;
        if (t.text == "{")
          braceDepth++;
        else if (t.text == "}")
          braceDepth = braceDepth > 0 ? braceDepth - 1 : 0;
      }
      continue;
    }

    for (const auto &t : tokens) {
      if (t.kind != LexToken::Kind::Punct)
        continue;
      if (t.text == "{")
        braceDepth++;
      else if (t.text == "}")
        braceDepth = braceDepth > 0 ? braceDepth - 1 : 0;
    }

    if (inStruct && braceDepth == 0)
      break;

    if (!inStruct || braceDepth != 1)
      continue;

    size_t typeIndex = std::string::npos;
    for (size_t i = 0; i < tokens.size(); i++) {
      if (tokens[i].kind != LexToken::Kind::Identifier)
        continue;
      if (isQualifierToken(tokens[i].text))
        continue;
      typeIndex = i;
      break;
    }
    if (typeIndex == std::string::npos)
      continue;
    if (typeIndex + 1 >= tokens.size())
      continue;
    if (tokens[typeIndex + 1].kind != LexToken::Kind::Identifier)
      continue;
    std::string memberName = tokens[typeIndex + 1].text;
    size_t stop = tokens.size();
    for (size_t i = typeIndex + 2; i < tokens.size(); i++) {
      if (tokens[i].kind == LexToken::Kind::Punct &&
          (tokens[i].text == ":" || tokens[i].text == ";" ||
           tokens[i].text == "["))
        break;
      if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == ";") {
        stop = i;
        break;
      }
    }
    std::string memberType = tokens[typeIndex].text;
    if (!memberName.empty() && !memberType.empty())
      membersOut.emplace(memberName, memberType);
  }
  return !membersOut.empty();
}

static bool hasStructDeclarationInText(const std::string &text,
                                       const std::string &structName) {
  if (structName.empty())
    return false;
  std::istringstream stream(text);
  std::string lineText;
  bool inBlockComment = false;
  while (std::getline(stream, lineText)) {
    bool maskBlock = inBlockComment;
    const auto mask = buildCodeMaskForLine(lineText, maskBlock);
    inBlockComment = maskBlock;
    const auto rawTokens = lexLineTokens(lineText);
    std::vector<LexToken> tokens;
    tokens.reserve(rawTokens.size());
    for (const auto &token : rawTokens) {
      if (token.start < mask.size() && mask[token.start])
        tokens.push_back(token);
    }
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
      if (tokens[i].kind == LexToken::Kind::Identifier &&
          tokens[i].text == "struct" &&
          tokens[i + 1].kind == LexToken::Kind::Identifier &&
          tokens[i + 1].text == structName) {
        return true;
      }
    }
  }
  return false;
}

struct StructTypeCache {
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      membersByStruct;
  std::unordered_map<std::string, bool> attempted;
};

struct SymbolTypeCache {
  std::unordered_map<std::string, std::string> typeBySymbol;
  std::unordered_map<std::string, bool> attempted;
  std::unordered_map<std::string, bool> attemptedInText;
};

static std::string
parseSymbolTypeFromLineTokens(const std::vector<LexToken> &tokens,
                              const std::string &symbol) {
  for (size_t i = 0; i < tokens.size(); i++) {
    if (tokens[i].kind != LexToken::Kind::Identifier ||
        tokens[i].text != symbol)
      continue;
    if (i > 0) {
      const auto &prev = tokens[i - 1];
      if (prev.kind == LexToken::Kind::Punct &&
          (prev.text == "." || prev.text == "->" || prev.text == "::"))
        continue;
    }
    bool hasAssignBefore = false;
    for (size_t j = 0; j < i; j++) {
      if (tokens[j].kind == LexToken::Kind::Punct && tokens[j].text == "=") {
        hasAssignBefore = true;
        break;
      }
    }
    if (hasAssignBefore)
      continue;
    if (i == 0)
      continue;
    const auto &typeToken = tokens[i - 1];
    if (typeToken.kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(typeToken.text))
      continue;
    if (typeToken.text == "return" || typeToken.text == "if" ||
        typeToken.text == "for" || typeToken.text == "while" ||
        typeToken.text == "switch")
      continue;
    return typeToken.text;
  }
  return "";
}

static std::string resolveSymbolTypeInText(const std::string &text,
                                           const std::string &symbol) {
  std::istringstream stream(text);
  std::string lineText;
  bool inBlockComment = false;
  while (std::getline(stream, lineText)) {
    bool maskBlock = inBlockComment;
    const auto mask = buildCodeMaskForLine(lineText, maskBlock);
    inBlockComment = maskBlock;
    const auto rawTokens = lexLineTokens(lineText);
    std::vector<LexToken> tokens;
    tokens.reserve(rawTokens.size());
    for (const auto &token : rawTokens) {
      if (token.start < mask.size() && mask[token.start])
        tokens.push_back(token);
    }
    const std::string type = parseSymbolTypeFromLineTokens(tokens, symbol);
    if (!type.empty())
      return type;
  }
  return "";
}

static std::string getLineByIndex(const std::string &text, int lineIndex) {
  if (lineIndex < 0)
    return "";
  std::istringstream stream(text);
  std::string lineText;
  int i = 0;
  while (std::getline(stream, lineText)) {
    if (i == lineIndex)
      return lineText;
    i++;
  }
  return "";
}

static std::string resolveSymbolTypeByWorkspaceScan(
    const std::string &symbol, const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &scanExtensions, SymbolTypeCache &cache,
    std::unordered_map<std::string, std::string> *fileTextCache) {
  std::string indexed;
  if (workspaceIndexGetSymbolType(symbol, indexed) && !indexed.empty()) {
    cache.typeBySymbol.emplace(symbol, indexed);
    return indexed;
  }
  auto cached = cache.typeBySymbol.find(symbol);
  if (cached != cache.typeBySymbol.end())
    return cached->second;
  bool attempted = false;
  auto attemptIt = cache.attempted.find(symbol);
  if (attemptIt != cache.attempted.end())
    attempted = attemptIt->second;
  if (attempted)
    return "";
  cache.attempted[symbol] = true;

  DefinitionLocation location;
  if (!findDefinitionByWorkspaceScan(symbol, scanRoots, scanExtensions,
                                     location)) {
    cache.typeBySymbol.emplace(symbol, "");
    return "";
  }

  std::string path = uriToPath(location.uri);
  if (path.empty())
    path = location.uri;
  std::string fileText;
  if (fileTextCache) {
    auto it = fileTextCache->find(path);
    if (it == fileTextCache->end()) {
      std::string loadedText;
      if (!readFileToString(path, loadedText)) {
        cache.typeBySymbol.emplace(symbol, "");
        return "";
      }
      it = fileTextCache->emplace(path, std::move(loadedText)).first;
    }
    fileText = it->second;
  } else {
    if (!readFileToString(path, fileText)) {
      cache.typeBySymbol.emplace(symbol, "");
      return "";
    }
  }
  const std::string lineText = getLineByIndex(fileText, location.line);
  if (lineText.empty()) {
    cache.typeBySymbol.emplace(symbol, "");
    return "";
  }

  const auto tokens = lexLineTokens(lineText);
  std::string type = parseSymbolTypeFromLineTokens(tokens, symbol);
  cache.typeBySymbol.emplace(symbol, type);
  return type;
}

static std::string resolveStructMemberType(
    const std::string &structName, const std::string &memberName,
    const std::string &currentText, const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &scanExtensions, StructTypeCache &cache) {
  std::string indexed;
  if (workspaceIndexGetStructMemberType(structName, memberName, indexed) &&
      !indexed.empty()) {
    return indexed;
  }
  auto cached = cache.membersByStruct.find(structName);
  if (cached != cache.membersByStruct.end()) {
    auto it = cached->second.find(memberName);
    if (it != cached->second.end())
      return it->second;
    return "";
  }

  bool attempted = false;
  auto attemptIt = cache.attempted.find(structName);
  if (attemptIt != cache.attempted.end())
    attempted = attemptIt->second;
  if (attempted)
    return "";
  cache.attempted[structName] = true;

  std::unordered_map<std::string, std::string> members;
  parseStructMembersFromText(currentText, structName, members);
  if (members.empty()) {
    DefinitionLocation location;
    if (findStructDefinitionByWorkspaceScan(structName, scanRoots,
                                            scanExtensions, location)) {
      std::string path = uriToPath(location.uri);
      if (path.empty())
        path = location.uri;
      std::string otherText;
      if (readFileToString(path, otherText)) {
        parseStructMembersFromText(otherText, structName, members);
      }
    }
  }

  cache.membersByStruct.emplace(structName, members);
  auto newCached = cache.membersByStruct.find(structName);
  if (newCached == cache.membersByStruct.end())
    return "";
  auto it = newCached->second.find(memberName);
  if (it != newCached->second.end())
    return it->second;
  return "";
}

static std::string inferExpressionTypeFromTokens(
    const std::vector<LexToken> &tokens, size_t startIndex,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentText, const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &scanExtensions,
    StructTypeCache &structCache, SymbolTypeCache &symbolCache,
    std::unordered_map<std::string, std::string> *fileTextCache);
static std::string inferExpressionTypeFromTokensRange(
    const std::vector<LexToken> &tokens, size_t startIndex, size_t endIndex,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentText, const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &scanExtensions,
    StructTypeCache &structCache, SymbolTypeCache &symbolCache,
    std::unordered_map<std::string, std::string> *fileTextCache);

struct ExprParser {
  const std::vector<LexToken> &tokens;
  size_t endIndex;
  size_t i;
  const std::unordered_map<std::string, std::string> &locals;
  const std::string &currentText;
  const std::vector<std::string> &scanRoots;
  const std::vector<std::string> &scanExtensions;
  StructTypeCache &structCache;
  SymbolTypeCache &symbolCache;
  std::unordered_map<std::string, std::string> *fileTextCache;

  const LexToken *peek() const {
    if (i >= endIndex)
      return nullptr;
    return &tokens[i];
  }

  const LexToken *consume() {
    if (i >= endIndex)
      return nullptr;
    return &tokens[i++];
  }

  bool matchPunct(const std::string &text) {
    const LexToken *t = peek();
    if (!t || t->kind != LexToken::Kind::Punct || t->text != text)
      return false;
    consume();
    return true;
  }

  std::string parseExpression() { return parseConditional(); }

  std::string parseConditional() {
    std::string base = parseOr();
    const LexToken *t = peek();
    if (!t || t->kind != LexToken::Kind::Punct || t->text != "?")
      return base;
    consume();
    std::string trueType = parseExpression();
    matchPunct(":");
    std::string falseType = parseExpression();
    return mergeArithmeticType(trueType, falseType);
  }

  std::string parseOr() {
    std::string left = parseAnd();
    bool saw = false;
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "||")
        break;
      saw = true;
      consume();
      std::string right = parseAnd();
      if (left.empty())
        left = right;
    }
    return saw ? "bool" : left;
  }

  std::string parseAnd() {
    std::string left = parseBitwiseOr();
    bool saw = false;
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "&&")
        break;
      saw = true;
      consume();
      std::string right = parseBitwiseOr();
      if (left.empty())
        left = right;
    }
    return saw ? "bool" : left;
  }

  std::string parseComparison() {
    std::string left = parseShift();
    bool saw = false;
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct)
        break;
      const std::string &op = t->text;
      if (op != "==" && op != "!=" && op != "<" && op != "<=" && op != ">" &&
          op != ">=")
        break;
      saw = true;
      consume();
      std::string right = parseShift();
      if (left.empty())
        left = right;
    }
    return saw ? "bool" : left;
  }

  static std::string mergeArithmeticType(const std::string &leftType,
                                         const std::string &rightType) {
    if (leftType.empty())
      return rightType;
    if (rightType.empty())
      return leftType;

    std::string leftMatScalar;
    std::string rightMatScalar;
    int leftRows = 0;
    int leftCols = 0;
    int rightRows = 0;
    int rightCols = 0;
    bool leftIsMat = isMatrixType(leftType, leftMatScalar, leftRows, leftCols);
    bool rightIsMat =
        isMatrixType(rightType, rightMatScalar, rightRows, rightCols);
    if (leftIsMat || rightIsMat) {
      if (leftIsMat && rightIsMat) {
        if (leftRows != rightRows || leftCols != rightCols)
          return leftType;
        std::string outScalar =
            leftMatScalar == rightMatScalar ? leftMatScalar : "float";
        return makeMatrixType(outScalar, leftRows, leftCols);
      }
      if (leftIsMat) {
        if (isNumericScalarType(rightType))
          return makeMatrixType(leftMatScalar, leftRows, leftCols);
        return leftType;
      }
      if (isNumericScalarType(leftType))
        return makeMatrixType(rightMatScalar, rightRows, rightCols);
      return rightType;
    }

    std::string leftScalar;
    std::string rightScalar;
    int leftDim = 0;
    int rightDim = 0;
    bool leftOk = parseVectorOrScalarType(leftType, leftScalar, leftDim);
    bool rightOk = parseVectorOrScalarType(rightType, rightScalar, rightDim);
    if (!leftOk || !rightOk) {
      if (leftType == rightType)
        return leftType;
      return leftType;
    }

    int outDim = std::max(leftDim, rightDim);
    std::string outScalar = leftScalar == rightScalar ? leftScalar : "float";
    return makeVectorOrScalarType(outScalar, outDim);
  }

  static std::string mergeBitwiseType(const std::string &leftType,
                                      const std::string &rightType) {
    if (leftType.empty())
      return rightType;
    if (rightType.empty())
      return leftType;
    std::string leftScalar;
    std::string rightScalar;
    int leftDim = 0;
    int rightDim = 0;
    bool leftOk = parseVectorOrScalarType(leftType, leftScalar, leftDim);
    bool rightOk = parseVectorOrScalarType(rightType, rightScalar, rightDim);
    if (!leftOk || !rightOk)
      return leftType;
    if ((leftScalar != "int" && leftScalar != "uint") ||
        (rightScalar != "int" && rightScalar != "uint")) {
      return leftType;
    }
    int outDim = 1;
    if (leftDim == rightDim)
      outDim = leftDim;
    else if (leftDim == 1)
      outDim = rightDim;
    else if (rightDim == 1)
      outDim = leftDim;
    else
      outDim = leftDim;
    std::string outScalar =
        (leftScalar == "uint" || rightScalar == "uint") ? "uint" : "int";
    return makeVectorOrScalarType(outScalar, outDim);
  }

  static std::string mergeMultiplyType(const std::string &leftType,
                                       const std::string &rightType) {
    if (leftType.empty())
      return rightType;
    if (rightType.empty())
      return leftType;

    std::string leftMatScalar;
    std::string rightMatScalar;
    int leftRows = 0;
    int leftCols = 0;
    int rightRows = 0;
    int rightCols = 0;
    bool leftIsMat = isMatrixType(leftType, leftMatScalar, leftRows, leftCols);
    bool rightIsMat =
        isMatrixType(rightType, rightMatScalar, rightRows, rightCols);

    std::string leftScalar;
    std::string rightScalar;
    int leftDim = 0;
    int rightDim = 0;
    bool leftIsVec =
        parseVectorOrScalarType(leftType, leftScalar, leftDim) && leftDim > 1;
    bool rightIsVec =
        parseVectorOrScalarType(rightType, rightScalar, rightDim) &&
        rightDim > 1;

    if (leftIsMat && rightIsMat) {
      if (leftCols != rightRows)
        return leftType;
      std::string outScalar =
          leftMatScalar == rightMatScalar ? leftMatScalar : "float";
      return makeMatrixType(outScalar, leftRows, rightCols);
    }

    if (leftIsVec && rightIsMat) {
      if (leftDim != rightRows)
        return makeVectorOrScalarType("float", rightCols);
      std::string outScalar =
          leftScalar == rightMatScalar ? leftScalar : "float";
      return makeVectorOrScalarType(outScalar, rightCols);
    }

    if (leftIsMat && rightIsVec) {
      if (leftCols != rightDim)
        return makeVectorOrScalarType("float", leftRows);
      std::string outScalar =
          leftMatScalar == rightScalar ? leftMatScalar : "float";
      return makeVectorOrScalarType(outScalar, leftRows);
    }

    if (leftIsMat && isNumericScalarType(rightType)) {
      return makeMatrixType(leftMatScalar, leftRows, leftCols);
    }
    if (rightIsMat && isNumericScalarType(leftType)) {
      return makeMatrixType(rightMatScalar, rightRows, rightCols);
    }

    return mergeArithmeticType(leftType, rightType);
  }

  std::string parseShift() {
    std::string left = parseAddSub();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct ||
          (t->text != "<<" && t->text != ">>"))
        break;
      consume();
      std::string right = parseAddSub();
      if (left.empty())
        left = right;
      else if (!right.empty())
        left = mergeBitwiseType(left, right);
    }
    return left;
  }

  std::string parseBitwiseAnd() {
    std::string left = parseComparison();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "&")
        break;
      consume();
      std::string right = parseComparison();
      left = mergeBitwiseType(left, right);
    }
    return left;
  }

  std::string parseBitwiseXor() {
    std::string left = parseBitwiseAnd();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "^")
        break;
      consume();
      std::string right = parseBitwiseAnd();
      left = mergeBitwiseType(left, right);
    }
    return left;
  }

  std::string parseBitwiseOr() {
    std::string left = parseBitwiseXor();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "|")
        break;
      consume();
      std::string right = parseBitwiseXor();
      left = mergeBitwiseType(left, right);
    }
    return left;
  }

  std::string parseAddSub() {
    std::string left = parseMulDiv();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct ||
          (t->text != "+" && t->text != "-"))
        break;
      consume();
      std::string right = parseMulDiv();
      left = mergeArithmeticType(left, right);
    }
    return left;
  }

  std::string parseMulDiv() {
    std::string left = parseUnary();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct ||
          (t->text != "*" && t->text != "/" && t->text != "%"))
        break;
      const std::string op = t->text;
      consume();
      std::string right = parseUnary();
      if (op == "*")
        left = mergeMultiplyType(left, right);
      else
        left = mergeArithmeticType(left, right);
    }
    return left;
  }

  std::string parseUnary() {
    const LexToken *t = peek();
    if (t && t->kind == LexToken::Kind::Punct &&
        (t->text == "!" || t->text == "-" || t->text == "+")) {
      consume();
      return parseUnary();
    }
    return parsePostfix(parsePrimary());
  }

  static void skipBalanced(const std::vector<LexToken> &tokens, size_t &i,
                           size_t endIndex, const std::string &open,
                           const std::string &close) {
    int depth = 0;
    while (i < endIndex) {
      const auto &t = tokens[i];
      if (t.kind == LexToken::Kind::Punct) {
        if (t.text == open)
          depth++;
        else if (t.text == close) {
          depth = depth > 0 ? depth - 1 : 0;
          if (depth == 0) {
            i++;
            break;
          }
        }
      }
      i++;
    }
  }

  static std::string
  inferBuiltinReturnType(const std::string &name,
                         const std::vector<std::string> &args) {
    std::vector<BuiltinTypeInfo> infos;
    infos.reserve(args.size());
    for (const auto &a : args) {
      infos.push_back(parseBuiltinTypeInfo(a));
    }
    BuiltinResolveResult rr = resolveBuiltinCall(name, infos);
    if (!rr.ok)
      return "";
    if (rr.warnMixedSignedness)
      return "";
    return builtinTypeInfoToString(rr.ret);
  }

  std::vector<std::string> parseCallArgumentTypes() {
    std::vector<std::string> args;
    if (!matchPunct("("))
      return args;
    if (matchPunct(")"))
      return args;

    while (i < endIndex) {
      size_t argStart = i;
      int parenDepth = 0;
      int bracketDepth = 0;
      while (i < endIndex) {
        const auto &t = tokens[i];
        if (t.kind == LexToken::Kind::Punct) {
          if (t.text == "(")
            parenDepth++;
          else if (t.text == ")") {
            if (parenDepth == 0)
              break;
            parenDepth--;
          } else if (t.text == "[")
            bracketDepth++;
          else if (t.text == "]")
            bracketDepth = bracketDepth > 0 ? bracketDepth - 1 : 0;
          else if (t.text == "," && parenDepth == 0 && bracketDepth == 0)
            break;
        }
        i++;
      }
      size_t argEnd = i;
      args.push_back(inferExpressionTypeFromTokensRange(
          tokens, argStart, argEnd, locals, currentText, scanRoots,
          scanExtensions, structCache, symbolCache, fileTextCache));
      if (i < endIndex && tokens[i].kind == LexToken::Kind::Punct &&
          tokens[i].text == ",") {
        i++;
        continue;
      }
      break;
    }

    matchPunct(")");
    return args;
  }

  std::string parsePrimary() {
    const LexToken *t = peek();
    if (!t)
      return "";
    if (t->kind == LexToken::Kind::Punct && t->text == "(") {
      if (i + 2 < endIndex &&
          tokens[i + 1].kind == LexToken::Kind::Identifier &&
          tokens[i + 2].kind == LexToken::Kind::Punct &&
          tokens[i + 2].text == ")") {
        const std::string castType = tokens[i + 1].text;
        int dim = 0;
        int rows = 0;
        int cols = 0;
        bool isBuiltinCastType = isVectorType(castType, dim) ||
                                 isScalarType(castType) ||
                                 isMatrixType(castType, rows, cols);
        bool isStructCastType =
            hasStructDeclarationInText(currentText, castType);
        if (isBuiltinCastType || isStructCastType) {
          consume();
          consume();
          consume();
          parseUnary();
          return castType;
        }
      }
      consume();
      std::string inner = parseExpression();
      matchPunct(")");
      return inner;
    }
    if (t->kind == LexToken::Kind::Identifier) {
      std::string word = t->text;
      consume();

      int dim = 0;
      int rows = 0;
      int cols = 0;
      if (isVectorType(word, dim) || isScalarType(word) ||
          isMatrixType(word, rows, cols)) {
        const LexToken *next = peek();
        if (next && next->kind == LexToken::Kind::Punct && next->text == "(") {
          skipBalanced(tokens, i, endIndex, "(", ")");
        }
        return word;
      }

      std::string numeric = inferNumericLiteralTypeFromTokens(tokens, i - 1);
      if (!numeric.empty())
        return numeric;
      std::string literal = inferLiteralType(word);
      if (!literal.empty())
        return literal;

      const LexToken *next = peek();
      if (next && next->kind == LexToken::Kind::Punct && next->text == "(") {
        size_t callStart = i;
        std::vector<std::string> args = parseCallArgumentTypes();
        std::string builtin = inferBuiltinReturnType(word, args);
        if (!builtin.empty())
          return builtin;
        i = callStart;
        skipBalanced(tokens, i, endIndex, "(", ")");
        auto cached = symbolCache.typeBySymbol.find(word);
        if (cached != symbolCache.typeBySymbol.end())
          return cached->second;
        std::string indexed;
        if (workspaceIndexGetSymbolType(word, indexed) && !indexed.empty()) {
          symbolCache.typeBySymbol.emplace(word, indexed);
          return indexed;
        }
        bool attempted = false;
        auto attemptIt = symbolCache.attemptedInText.find(word);
        if (attemptIt != symbolCache.attemptedInText.end())
          attempted = attemptIt->second;
        if (attempted)
          return "";
        symbolCache.attemptedInText[word] = true;
        std::string inText = resolveSymbolTypeInText(currentText, word);
        if (!inText.empty()) {
          symbolCache.typeBySymbol.emplace(word, inText);
          return inText;
        }
        return "";
      }

      auto it = locals.find(word);
      if (it != locals.end())
        return it->second;
      std::string inText = resolveSymbolTypeInText(currentText, word);
      if (!inText.empty())
        return inText;
      return resolveSymbolTypeByWorkspaceScan(word, scanRoots, scanExtensions,
                                              symbolCache, fileTextCache);
    }
    if (t->kind == LexToken::Kind::Punct) {
      return "";
    }
    std::string literal = inferLiteralType(t->text);
    consume();
    return literal;
  }

  std::string parsePostfix(std::string baseType) {
    while (true) {
      const LexToken *t = peek();
      if (!t)
        break;
      if (t->kind == LexToken::Kind::Punct && t->text == ".") {
        consume();
        const LexToken *name = peek();
        if (!name || name->kind != LexToken::Kind::Identifier)
          break;
        std::string member = name->text;
        consume();
        const LexToken *after = peek();
        if (after && after->kind == LexToken::Kind::Punct &&
            after->text == "(") {
          parseCallArgumentTypes();
          HlslBuiltinMethodRule methodRule;
          if (lookupHlslBuiltinMethodRule(member, baseType, methodRule) &&
              !methodRule.returnType.empty() &&
              normalizeTypeToken(methodRule.returnType) != "void") {
            baseType = normalizeTypeToken(methodRule.returnType);
            continue;
          }
          continue;
        }
        if (isSwizzleToken(member)) {
          std::string swizzled = applySwizzleType(baseType, member);
          if (!swizzled.empty())
            baseType = swizzled;
          continue;
        }
        if (!baseType.empty()) {
          std::string memberType =
              resolveStructMemberType(baseType, member, currentText, scanRoots,
                                      scanExtensions, structCache);
          if (!memberType.empty()) {
            baseType = memberType;
            continue;
          }
        }
        continue;
      }
      if (t->kind == LexToken::Kind::Punct && t->text == "[") {
        consume();
        skipBalanced(tokens, i, endIndex, "[", "]");
        baseType = applyIndexAccessType(baseType);
        continue;
      }
      break;
    }
    return baseType;
  }
};

static std::string inferExpressionTypeFromTokens(
    const std::vector<LexToken> &tokens, size_t startIndex,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentText, const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &scanExtensions,
    StructTypeCache &structCache, SymbolTypeCache &symbolCache,
    std::unordered_map<std::string, std::string> *fileTextCache) {
  size_t endIndex = tokens.size();
  int parenDepth = 0;
  int bracketDepth = 0;
  for (size_t i = startIndex; i < tokens.size(); i++) {
    if (tokens[i].kind != LexToken::Kind::Punct)
      continue;
    const std::string &p = tokens[i].text;
    if (p == "(") {
      parenDepth++;
      continue;
    }
    if (p == ")") {
      if (parenDepth > 0)
        parenDepth--;
      continue;
    }
    if (p == "[") {
      bracketDepth++;
      continue;
    }
    if (p == "]") {
      if (bracketDepth > 0)
        bracketDepth--;
      continue;
    }
    if (parenDepth == 0 && bracketDepth == 0 && (p == ";" || p == ",")) {
      endIndex = i;
      break;
    }
  }
  return inferExpressionTypeFromTokensRange(
      tokens, startIndex, endIndex, locals, currentText, scanRoots,
      scanExtensions, structCache, symbolCache, fileTextCache);
}

static std::string inferExpressionTypeFromTokensRange(
    const std::vector<LexToken> &tokens, size_t startIndex, size_t endIndex,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentText, const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &scanExtensions,
    StructTypeCache &structCache, SymbolTypeCache &symbolCache,
    std::unordered_map<std::string, std::string> *fileTextCache) {
  endIndex = std::min(endIndex, tokens.size());
  ExprParser parser{tokens,      endIndex,     startIndex,     locals,
                    currentText, scanRoots,    scanExtensions, structCache,
                    symbolCache, fileTextCache};
  return parser.parseExpression();
}

static void collectBracketDiagnostics(const std::string &text, Json &diags) {
  struct Entry {
    char bracket;
    int line;
    int character;
  };
  std::vector<Entry> stack;
  bool inString = false;
  bool inLineComment = false;
  bool inBlockComment = false;
  int line = 0;
  int character = 0;

  auto push = [&](char bracket) {
    stack.push_back(Entry{bracket, line, character});
  };
  auto mismatch = [&](char found) {
    std::string message = "Unmatched closing bracket: ";
    message.push_back(found);
    diags.a.push_back(makeDiagnostic(text, line, character, character + 1, 1,
                                     "nsf", message));
  };

  for (size_t i = 0; i < text.size(); i++) {
    char ch = text[i];
    char next = (i + 1 < text.size()) ? text[i + 1] : '\0';

    if (ch == '\n') {
      line++;
      character = 0;
      inLineComment = false;
      continue;
    }

    if (inString) {
      if (ch == '"' && (i == 0 || text[i - 1] != '\\'))
        inString = false;
      character++;
      continue;
    }

    if (inLineComment) {
      character++;
      continue;
    }

    if (inBlockComment) {
      if (ch == '*' && next == '/') {
        inBlockComment = false;
        i++;
        character += 2;
        continue;
      }
      character++;
      continue;
    }

    if (ch == '"') {
      inString = true;
      character++;
      continue;
    }
    if (ch == '/' && next == '/') {
      inLineComment = true;
      i++;
      character += 2;
      continue;
    }
    if (ch == '/' && next == '*') {
      inBlockComment = true;
      i++;
      character += 2;
      continue;
    }

    if (ch == '(' || ch == '{' || ch == '[') {
      push(ch);
      character++;
      continue;
    }
    if (ch == ')' || ch == '}' || ch == ']') {
      if (stack.empty()) {
        mismatch(ch);
        character++;
        continue;
      }
      char expected = stack.back().bracket;
      bool ok = (expected == '(' && ch == ')') ||
                (expected == '{' && ch == '}') ||
                (expected == '[' && ch == ']');
      if (!ok) {
        mismatch(ch);
        character++;
        continue;
      }
      stack.pop_back();
      character++;
      continue;
    }
    character++;
  }

  for (const auto &entry : stack) {
    std::string message = "Unterminated bracket: ";
    message.push_back(entry.bracket);
    diags.a.push_back(makeDiagnostic(text, entry.line, entry.character,
                                     entry.character + 1, 1, "nsf", message));
  }
}

static void collectReturnAndTypeDiagnostics(
    const std::string &uri, const std::string &text,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines, Json &diags,
    int timeBudgetMs, size_t maxDiagnostics, bool &timedOut,
    bool indeterminateEnabled, int indeterminateSeverity,
    size_t indeterminateMaxItems, size_t &indeterminateCount) {
  const auto diagnosticsStart = std::chrono::steady_clock::now();
  const auto diagnosticsBudget =
      std::chrono::milliseconds(std::max(30, timeBudgetMs));
  std::istringstream stream(text);
  std::string lineText;
  int lineIndex = 0;
  bool inBlockComment = false;
  bool inString = false;
  const auto lineActive = computeActiveLineMask(text, defines);
  const auto branchSigs = computePreprocessorBranchSigs(text);

  bool inFunction = false;
  std::string pendingReturnType;
  std::string pendingFunctionName;
  int pendingFunctionLine = -1;
  int pendingFunctionStart = -1;
  bool pendingSignature = false;
  std::unordered_map<std::string, std::string> pendingParams;
  std::vector<std::string> pendingParamTypesOrdered;

  std::string functionReturnType;
  std::string functionName;
  int functionNameLine = -1;
  int functionNameStart = -1;
  int functionNameEnd = -1;
  int functionBraceDepth = 0;
  bool sawReturn = false;
  bool sawTopLevelReturn = false;
  bool sawPotentialMissingReturn = false;
  bool sawPotentialUnreachable = false;
  struct LocalDeclEntry {
    std::string type;
    PreprocBranchSig sig;
  };
  std::unordered_map<std::string, std::vector<LocalDeclEntry>> localsByName;
  std::unordered_map<std::string, std::string> localsVisibleTypes;
  std::unordered_set<std::string> localsVisibleNames;
  std::string pendingMultilineLocalName;
  int pendingMultilineLocalDepth = -1;
  int pendingMultilineLocalLine = -1;
  int pendingMultilineLocalStart = -1;
  int pendingMultilineLocalEnd = -1;
  std::unordered_set<std::string> paramNames;
  std::unordered_set<std::string> globalSymbols;
  std::unordered_set<std::string> globalFunctionSignatures;
  std::unordered_map<std::string, bool> resolvedSymbolCache;
  int lastIfLineAtDepth1 = -100000;
  bool lastIfHadElseAtDepth1 = false;
  bool conditionalReturnSeen = false;
  bool unconditionalReturnSeen = false;
  bool inUiMetaBlock = false;
  std::string pendingUiVarName;
  int pendingUiVarLine = -1;
  int pendingUiVarStart = -1;
  int pendingUiVarEnd = -1;
  int typeBlockBraceDepth = 0;
  bool pendingTypeBlockOpen = false;
  auto emitIndeterminate =
      [&](int line, int startByte, int endByte, const std::string &code,
          const std::string &reasonCode, const std::string &message) {
        if (!indeterminateEnabled)
          return;
        if (indeterminateCount >= indeterminateMaxItems)
          return;
        if (diags.a.size() >= maxDiagnostics)
          return;
        diags.a.push_back(makeDiagnosticWithCodeAndReason(
            text, line, startByte, endByte, indeterminateSeverity, "nsf",
            message, code, reasonCode));
        indeterminateCount++;
      };
  const bool builtinRegistryAvailable = isHlslBuiltinRegistryAvailable();
  const std::string builtinRegistryError = getHlslBuiltinRegistryError();
  if (!builtinRegistryAvailable) {
    emitIndeterminate(
        0, 0, 0, "NSF_INDET_BUILTIN_REGISTRY_UNAVAILABLE",
        IndeterminateReason::DiagnosticsBuiltinRegistryUnavailable,
        "Indeterminate builtin analysis: builtin registry unavailable. " +
            builtinRegistryError);
  }

  auto isAbsolutePath = [](const std::string &path) {
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':')
      return true;
    return path.rfind("\\\\", 0) == 0 || path.rfind("/", 0) == 0;
  };
  auto joinPath = [](const std::string &base, const std::string &child) {
    if (base.empty())
      return child;
    char sep = '\\';
    if (base.back() == '/' || base.back() == '\\')
      return base + child;
    return base + sep + child;
  };
  auto addUnique = [](std::vector<std::string> &items,
                      const std::string &value) {
    for (const auto &item : items) {
      if (item == value)
        return;
    }
    items.push_back(value);
  };

  std::vector<std::string> scanRoots;
  std::string docPath = uriToPath(uri);
  if (!docPath.empty()) {
    size_t lastSlash = docPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
      scanRoots.push_back(docPath.substr(0, lastSlash));
    }
  }
  for (const auto &inc : includePaths) {
    if (inc.empty())
      continue;
    if (isAbsolutePath(inc)) {
      addUnique(scanRoots, inc);
      continue;
    }
    for (const auto &folder : workspaceFolders) {
      if (!folder.empty())
        addUnique(scanRoots, joinPath(folder, inc));
    }
  }
  for (const auto &folder : workspaceFolders) {
    if (!folder.empty())
      addUnique(scanRoots, folder);
  }
  std::vector<std::string> scanExtensions = shaderExtensions;
  addUnique(scanExtensions, ".hlsli");
  addUnique(scanExtensions, ".h");
  StructTypeCache structCache;
  SymbolTypeCache symbolCache;
  enum class FunctionCandidateConfidence {
    AstIndexed,
    MacroDerived,
    TextFallback
  };
  struct UserFunctionCandidate {
    std::string label;
    std::vector<std::string> paramTypes;
    DefinitionLocation loc;
    FunctionCandidateConfidence confidence =
        FunctionCandidateConfidence::AstIndexed;
  };
  std::unordered_map<std::string, std::vector<UserFunctionCandidate>>
      userFunctionCache;
  std::unordered_map<std::string, std::string> fileTextCache;
  std::unordered_map<std::string, bool> fileExistsCache;
  std::vector<std::string> includeGraphFiles;

  auto formatLocationShort = [&](const DefinitionLocation &loc) {
    std::string path = uriToPath(loc.uri);
    if (path.empty())
      path = loc.uri;
    std::ostringstream oss;
    oss << path << ":" << (loc.line + 1);
    return oss.str();
  };

  auto parseParamTypeFromDecl = [&](const std::string &param) -> std::string {
    auto tokens = lexLineTokens(param);
    for (const auto &t : tokens) {
      if (t.kind != LexToken::Kind::Identifier)
        continue;
      if (isQualifierToken(t.text))
        continue;
      if (t.text == "in" || t.text == "out" || t.text == "inout")
        continue;
      return normalizeTypeToken(t.text);
    }
    return "";
  };

  enum class ScalarCategory { FloatLike, IntLike, Other };
  auto scalarCategory = [&](const std::string &scalar) {
    if (scalar == "float" || scalar == "half" || scalar == "double")
      return ScalarCategory::FloatLike;
    if (scalar == "int" || scalar == "uint")
      return ScalarCategory::IntLike;
    return ScalarCategory::Other;
  };

  auto normalizeObjectType = [](const std::string &type) {
    std::string out = normalizeTypeToken(type);
    std::string lower;
    lower.reserve(out.size());
    for (char ch : out)
      lower.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    if (lower == "texture")
      return std::string("texture2d");
    if (lower == "texture2d")
      return std::string("texture2d");
    if (lower == "sampler" || lower == "samplerstate")
      return std::string("sampler");
    return lower;
  };

  auto compatibleValueType = [&](const std::string &expected,
                                 const std::string &actual) -> bool {
    if (expected.empty() || actual.empty())
      return true;
    if (expected == actual)
      return true;
    std::string expectedObjectType = normalizeObjectType(expected);
    std::string actualObjectType = normalizeObjectType(actual);
    if (!expectedObjectType.empty() && expectedObjectType == actualObjectType)
      return true;
    std::string eScalar;
    std::string aScalar;
    int eDim = 0;
    int aDim = 0;
    if (parseVectorOrScalarType(expected, eScalar, eDim) &&
        parseVectorOrScalarType(actual, aScalar, aDim) && eDim == aDim) {
      auto ec = scalarCategory(eScalar);
      auto ac = scalarCategory(aScalar);
      if (ec == ScalarCategory::Other || ac == ScalarCategory::Other)
        return false;
      return ec == ac;
    }
    return false;
  };

  auto getUserFunctionCandidates = [&](const std::string &name)
      -> const std::vector<UserFunctionCandidate> & {
    auto it = userFunctionCache.find(name);
    if (it != userFunctionCache.end())
      return it->second;

    std::vector<UserFunctionCandidate> out;
    std::vector<IndexedDefinition> defs;
    if (workspaceIndexFindDefinitions(name, defs, 64)) {
      for (const auto &d : defs) {
        if (d.kind != 12)
          continue;
        std::string path = uriToPath(d.uri);
        if (path.empty())
          continue;
        auto textIt = fileTextCache.find(path);
        if (textIt == fileTextCache.end()) {
          std::string loadedText;
          if (!readFileToString(path, loadedText))
            continue;
          textIt = fileTextCache.emplace(path, std::move(loadedText)).first;
        }
        const std::string &defText = textIt->second;
        std::string label;
        std::vector<std::string> params;
        const bool fullSig = queryFullAstFunctionSignature(
            d.uri, defText, 0, name, d.line, d.start, label, params);
        bool fastSig = false;
        if (!fullSig) {
          fastSig = queryFastAstFunctionSignature(
              d.uri, defText, 0, name, d.line, d.start, label, params);
        }
        if ((!fullSig && !fastSig &&
             !extractFunctionSignatureAt(defText, d.line, d.start, name, label,
                                         params)) ||
            label.empty()) {
          continue;
        }
        std::vector<std::string> paramTypes;
        paramTypes.reserve(params.size());
        for (const auto &p : params)
          paramTypes.push_back(parseParamTypeFromDecl(p));
        out.push_back(UserFunctionCandidate{
            label, paramTypes,
            DefinitionLocation{d.uri, d.line, d.start, d.end},
            FunctionCandidateConfidence::AstIndexed});
        if (out.size() >= 16)
          break;
      }
    }
    if (out.empty()) {
      std::vector<MacroGeneratedFunctionInfo> macroCandidates;
      if (collectMacroGeneratedFunctions(uri, text, workspaceFolders,
                                         includePaths, scanExtensions, name,
                                         macroCandidates, 16)) {
        for (const auto &candidate : macroCandidates) {
          out.push_back(UserFunctionCandidate{
              candidate.label, candidate.parameterTypes, candidate.definition,
              FunctionCandidateConfidence::MacroDerived});
        }
      }
    }
    if (out.empty()) {
      size_t lineStart = 0;
      int lineNumber = 0;
      const std::string needle = name + "(";
      while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos)
          lineEnd = text.size();
        std::string line = text.substr(lineStart, lineEnd - lineStart);
        size_t namePos = line.find(needle);
        while (namePos != std::string::npos) {
          const auto lineTokens = lexLineTokens(line);
          bool definitionLike = false;
          for (size_t ti = 0; ti < lineTokens.size(); ti++) {
            if (lineTokens[ti].kind != LexToken::Kind::Identifier ||
                lineTokens[ti].text != name ||
                lineTokens[ti].start != namePos) {
              continue;
            }
            if (ti == 0)
              break;
            if (lineTokens[ti - 1].kind != LexToken::Kind::Identifier)
              break;
            if (isQualifierToken(lineTokens[ti - 1].text))
              break;
            if (ti >= 2 && lineTokens[ti - 2].kind == LexToken::Kind::Punct &&
                (lineTokens[ti - 2].text == "." ||
                 lineTokens[ti - 2].text == "->" ||
                 lineTokens[ti - 2].text == "::")) {
              break;
            }
            definitionLike = true;
            break;
          }
          if (!definitionLike) {
            namePos = line.find(needle, namePos + 1);
            continue;
          }
          std::string label;
          std::vector<std::string> params;
          if (extractFunctionSignatureAt(text, lineNumber,
                                         static_cast<int>(namePos), name, label,
                                         params) &&
              !label.empty()) {
            std::vector<std::string> paramTypes;
            paramTypes.reserve(params.size());
            for (const auto &p : params)
              paramTypes.push_back(parseParamTypeFromDecl(p));
            out.push_back(UserFunctionCandidate{
                label, paramTypes,
                DefinitionLocation{uri, lineNumber, static_cast<int>(namePos),
                                   static_cast<int>(namePos + name.size())},
                FunctionCandidateConfidence::TextFallback});
            break;
          }
          namePos = line.find(needle, namePos + 1);
        }
        if (lineEnd == text.size())
          break;
        lineStart = lineEnd + 1;
        lineNumber++;
      }
    }

    auto inserted = userFunctionCache.emplace(name, std::move(out));
    return inserted.first->second;
  };

  {
    auto startTime = std::chrono::steady_clock::now();
    const auto timeBudget = std::chrono::milliseconds(
        std::max(20, std::min(250, timeBudgetMs / 2)));
    const size_t fileBudget = 512;
    size_t loadedFiles = 0;

    std::unordered_set<std::string> visitedUris;
    std::vector<std::string> stackUris;
    stackUris.push_back(uri);
    visitedUris.insert(uri);

    while (!stackUris.empty()) {
      std::string currentUri = stackUris.back();
      stackUris.pop_back();
      auto elapsed = std::chrono::steady_clock::now() - startTime;
      if (elapsed > timeBudget || loadedFiles >= fileBudget)
        break;

      std::string currentText;
      if (currentUri == uri) {
        currentText = text;
      } else {
        std::string currentPath = uriToPath(currentUri);
        if (currentPath.empty())
          continue;
        auto textIt = fileTextCache.find(currentPath);
        if (textIt == fileTextCache.end()) {
          std::string loadedText;
          if (!readFileToString(currentPath, loadedText))
            continue;
          textIt =
              fileTextCache.emplace(currentPath, std::move(loadedText)).first;
        }
        currentText = textIt->second;
      }
      loadedFiles++;

      std::vector<std::string> includePathList;
      if (!queryFullAstIncludes(currentUri, currentText, 0, includePathList)) {
        includePathList.clear();
      }
      for (const auto &includePath : includePathList) {
        auto candidates =
            resolveIncludeCandidates(currentUri, includePath, workspaceFolders,
                                     includePaths, scanExtensions);
        for (const auto &candidate : candidates) {
          auto existsIt = fileExistsCache.find(candidate);
          bool exists = false;
          if (existsIt == fileExistsCache.end()) {
            struct _stat statBuffer;
            exists = (_stat(candidate.c_str(), &statBuffer) == 0);
            fileExistsCache.emplace(candidate, exists);
          } else {
            exists = existsIt->second;
          }
          if (!exists)
            continue;
          addUnique(includeGraphFiles, candidate);
          std::string nextUri = pathToUri(candidate);
          if (!nextUri.empty() && visitedUris.insert(nextUri).second) {
            stackUris.push_back(nextUri);
          }
          break;
        }
      }
    }
  }

  auto collectMacroNames = [&](const std::string &sourceText,
                               std::unordered_set<std::string> &out) {
    std::istringstream stream(sourceText);
    std::string lineText;
    bool inBlockComment = false;
    while (std::getline(stream, lineText)) {
      bool maskBlock = inBlockComment;
      const auto mask = buildCodeMaskForLine(lineText, maskBlock);
      inBlockComment = maskBlock;
      if (!isPreprocessorDirectiveLine(lineText, mask))
        continue;
      const auto rawTokens = lexLineTokens(lineText);
      std::vector<LexToken> tokens;
      tokens.reserve(rawTokens.size());
      for (const auto &token : rawTokens) {
        if (token.start < mask.size() && mask[token.start])
          tokens.push_back(token);
      }
      if (tokens.size() < 3)
        continue;
      if (tokens[0].kind != LexToken::Kind::Punct || tokens[0].text != "#")
        continue;
      if (tokens[1].kind != LexToken::Kind::Identifier ||
          tokens[1].text != "define")
        continue;
      if (tokens[2].kind != LexToken::Kind::Identifier)
        continue;
      out.insert(tokens[2].text);
    }
  };

  std::unordered_set<std::string> macroNames;
  collectMacroNames(text, macroNames);
  for (const auto &path : includeGraphFiles) {
    auto textIt = fileTextCache.find(path);
    if (textIt == fileTextCache.end()) {
      std::string loadedText;
      if (!readFileToString(path, loadedText))
        continue;
      textIt = fileTextCache.emplace(path, std::move(loadedText)).first;
    }
    collectMacroNames(textIt->second, macroNames);
  }
  const bool workspaceIndexReadySnapshot = workspaceIndexIsReady();

  auto isBuiltinOrKeyword = [&](const std::string &word) {
    if (word.empty())
      return true;
    if (isQualifierToken(word))
      return true;
    if (isHlslKeyword(word))
      return true;
    int dim = 0;
    if (isVectorType(word, dim))
      return true;
    if (isScalarType(word))
      return true;
    int rows = 0;
    int cols = 0;
    if (isMatrixType(word, rows, cols))
      return true;
    if (isHlslBuiltinFunction(word))
      return true;
    if (macroNames.find(word) != macroNames.end())
      return true;
    return false;
  };

  auto isKnownSymbol = [&](const std::string &word) {
    if (localsVisibleNames.find(word) != localsVisibleNames.end())
      return true;
    if (paramNames.find(word) != paramNames.end())
      return true;
    if (globalSymbols.find(word) != globalSymbols.end())
      return true;
    return false;
  };

  auto isExternallyResolvable = [&](const std::string &word) {
    auto it = resolvedSymbolCache.find(word);
    if (it != resolvedSymbolCache.end())
      return it->second;
    DefinitionLocation location;
    bool ok = false;
    if (!includeGraphFiles.empty()) {
      ok = findDefinitionByWorkspaceScan(word, includeGraphFiles,
                                         scanExtensions, location, false);
    }
    if (!ok) {
      const bool allowWorkspaceScanWhenIndexReady =
          word.rfind("u_", 0) == 0 || word.rfind("t_", 0) == 0 ||
          word.rfind("s_", 0) == 0 || word.rfind("g_", 0) == 0;
      if (workspaceIndexReadySnapshot) {
        ok = workspaceIndexFindDefinition(word, location);
        if (!ok && allowWorkspaceScanWhenIndexReady) {
          ok = findDefinitionByWorkspaceScan(word, scanRoots, scanExtensions,
                                             location);
        }
      } else {
        ok = findDefinitionByWorkspaceScan(word, scanRoots, scanExtensions,
                                           location);
      }
    }
    resolvedSymbolCache.emplace(word, ok);
    return ok;
  };

  auto rebuildVisibleLocals = [&](const PreprocBranchSig &sig) {
    localsVisibleTypes.clear();
    localsVisibleNames.clear();
    for (const auto &entry : localsByName) {
      const std::string &name = entry.first;
      const auto &decls = entry.second;
      bool any = false;
      std::string soleType;
      bool typeSet = false;
      bool ambiguous = false;
      for (const auto &decl : decls) {
        if (!preprocBranchSigsOverlap(decl.sig, sig))
          continue;
        any = true;
        std::string t = normalizeTypeToken(decl.type);
        if (!typeSet) {
          soleType = t;
          typeSet = true;
        } else if (t != soleType) {
          ambiguous = true;
        }
      }
      if (!any)
        continue;
      localsVisibleNames.insert(name);
      if (typeSet && !ambiguous && !soleType.empty()) {
        localsVisibleTypes.emplace(name, soleType);
      }
    }
  };

  bool unreachableActive = false;
  int unreachableDepth = 0;

  auto scanForBraces = [&](const std::string &line) {
    for (size_t i = 0; i < line.size(); i++) {
      char ch = line[i];
      char next = (i + 1 < line.size()) ? line[i + 1] : '\0';
      if (inBlockComment) {
        if (ch == '*' && next == '/') {
          inBlockComment = false;
          i++;
        }
        continue;
      }
      if (inString) {
        if (ch == '"' && (i == 0 || line[i - 1] != '\\'))
          inString = false;
        continue;
      }
      if (ch == '"') {
        inString = true;
        continue;
      }
      if (ch == '/' && next == '/') {
        break;
      }
      if (ch == '/' && next == '*') {
        inBlockComment = true;
        i++;
        continue;
      }
      if (ch == '{') {
        if (inFunction) {
          functionBraceDepth++;
        } else if (pendingSignature) {
          std::string signatureKey = pendingFunctionName + "(";
          for (size_t i = 0; i < pendingParamTypesOrdered.size(); i++) {
            if (i > 0)
              signatureKey.push_back(',');
            signatureKey += normalizeTypeToken(pendingParamTypesOrdered[i]);
          }
          signatureKey.push_back(')');
          if (!globalFunctionSignatures.insert(signatureKey).second) {
            diags.a.push_back(makeDiagnostic(
                text, pendingFunctionLine, pendingFunctionStart,
                pendingFunctionStart +
                    static_cast<int>(pendingFunctionName.size()),
                2, "nsf",
                "Duplicate global declaration: " + pendingFunctionName + "."));
          }
          inFunction = true;
          functionReturnType = pendingReturnType;
          functionName = pendingFunctionName;
          functionNameLine = pendingFunctionLine;
          functionNameStart = pendingFunctionStart;
          functionNameEnd = pendingFunctionStart +
                            static_cast<int>(pendingFunctionName.size());
          functionBraceDepth = 1;
          sawReturn = false;
          sawTopLevelReturn = false;
          sawPotentialMissingReturn = false;
          sawPotentialUnreachable = false;
          lastIfLineAtDepth1 = -100000;
          lastIfHadElseAtDepth1 = false;
          conditionalReturnSeen = false;
          unconditionalReturnSeen = false;
          localsByName.clear();
          localsVisibleTypes.clear();
          localsVisibleNames.clear();
          paramNames.clear();
          for (const auto &entry : pendingParams) {
            paramNames.insert(entry.first);
            localsByName[entry.first].push_back(
                LocalDeclEntry{entry.second, PreprocBranchSig{}});
          }
          rebuildVisibleLocals(PreprocBranchSig{});
          pendingParams.clear();
          pendingParamTypesOrdered.clear();
          pendingSignature = false;
          unreachableActive = false;
          unreachableDepth = 0;
        } else if (pendingTypeBlockOpen || typeBlockBraceDepth > 0) {
          typeBlockBraceDepth++;
          pendingTypeBlockOpen = false;
        }
        continue;
      }
      if (ch == '}') {
        if (inFunction) {
          if (functionBraceDepth == 1 && !pendingMultilineLocalName.empty() &&
              pendingMultilineLocalDepth == 1 &&
              pendingMultilineLocalLine >= 0 &&
              pendingMultilineLocalStart >= 0 &&
              pendingMultilineLocalEnd >= pendingMultilineLocalStart) {
            diags.a.push_back(makeDiagnostic(
                text, pendingMultilineLocalLine, pendingMultilineLocalStart,
                pendingMultilineLocalEnd, 1, "nsf", "Missing semicolon."));
          }
          functionBraceDepth =
              functionBraceDepth > 0 ? functionBraceDepth - 1 : 0;
          if (functionBraceDepth == 0) {
            if (normalizeTypeToken(functionReturnType) != "void" &&
                !sawReturn) {
              diags.a.push_back(makeDiagnostic(
                  text, functionNameLine, functionNameStart, functionNameEnd, 1,
                  "nsf", "Missing return statement."));
            }
            if (normalizeTypeToken(functionReturnType) != "void" && sawReturn &&
                sawPotentialMissingReturn) {
              diags.a.push_back(makeDiagnostic(
                  text, functionNameLine, functionNameStart, functionNameEnd, 2,
                  "nsf", "Potential missing return on some paths."));
            }
            inFunction = false;
            functionReturnType.clear();
            functionName.clear();
            localsByName.clear();
            localsVisibleTypes.clear();
            localsVisibleNames.clear();
            paramNames.clear();
            pendingMultilineLocalName.clear();
            pendingMultilineLocalDepth = -1;
            pendingMultilineLocalLine = -1;
            pendingMultilineLocalStart = -1;
            pendingMultilineLocalEnd = -1;
            unreachableActive = false;
            unreachableDepth = 0;
          }
        } else if (typeBlockBraceDepth > 0) {
          typeBlockBraceDepth--;
          if (typeBlockBraceDepth == 0)
            pendingTypeBlockOpen = false;
        }
        continue;
      }
    }
  };

  const PreprocBranchSig emptySig;
  auto collectSignatureParamsFromTokens =
      [&](const std::vector<LexToken> &sigTokens, size_t startIndex,
          size_t endExclusive, int diagLine) {
        if (startIndex >= endExclusive || endExclusive > sigTokens.size())
          return;
        size_t segmentStart = startIndex;
        for (size_t i = startIndex; i <= endExclusive; i++) {
          bool atEnd = (i == endExclusive);
          bool atComma =
              (!atEnd && sigTokens[i].kind == LexToken::Kind::Punct &&
               sigTokens[i].text == ",");
          if (!atEnd && !atComma)
            continue;
          size_t segmentEnd = i;
          std::string paramType;
          std::string paramName;
          for (size_t j = segmentStart; j < segmentEnd; j++) {
            if (sigTokens[j].kind == LexToken::Kind::Punct &&
                (sigTokens[j].text == ":" || sigTokens[j].text == "="))
              break;
            if (sigTokens[j].kind != LexToken::Kind::Identifier)
              continue;
            if (isQualifierToken(sigTokens[j].text))
              continue;
            if (paramType.empty()) {
              paramType = sigTokens[j].text;
              continue;
            }
            if (paramName.empty())
              paramName = sigTokens[j].text;
          }
          if (!paramType.empty() && !paramName.empty()) {
            if (pendingParams.find(paramName) != pendingParams.end()) {
              int start = 0;
              int end = 0;
              if (!atEnd && i < sigTokens.size()) {
                start = static_cast<int>(sigTokens[i].start);
                end = static_cast<int>(sigTokens[i].end);
              } else if (segmentEnd > segmentStart &&
                         segmentEnd - 1 < sigTokens.size()) {
                start = static_cast<int>(sigTokens[segmentEnd - 1].start);
                end = static_cast<int>(sigTokens[segmentEnd - 1].end);
              }
              diags.a.push_back(makeDiagnostic(
                  text, diagLine, start, end, 2, "nsf",
                  "Duplicate parameter declaration: " + paramName + "."));
            } else {
              pendingParams.emplace(paramName, paramType);
              pendingParamTypesOrdered.push_back(paramType);
            }
          }
          segmentStart = i + 1;
        }
      };

  while (std::getline(stream, lineText)) {
    if (diags.a.size() >= maxDiagnostics)
      return;
    if (std::chrono::steady_clock::now() - diagnosticsStart >
        diagnosticsBudget) {
      timedOut = true;
      return;
    }
    bool maskBlock = inBlockComment;
    const auto mask = buildCodeMaskForLine(lineText, maskBlock);
    inBlockComment = maskBlock;
    const auto rawTokens = lexLineTokens(lineText);
    std::vector<LexToken> tokens;
    tokens.reserve(rawTokens.size());
    for (const auto &token : rawTokens) {
      if (token.start < mask.size() && mask[token.start])
        tokens.push_back(token);
    }

    if (lineIndex < static_cast<int>(lineActive.size()) &&
        !lineActive[lineIndex]) {
      lineIndex++;
      continue;
    }

    if (isPreprocessorDirectiveLine(lineText, mask)) {
      scanForBraces(lineText);
      lineIndex++;
      continue;
    }

    const PreprocBranchSig &currentSig =
        (lineIndex < static_cast<int>(branchSigs.size()) ? branchSigs[lineIndex]
                                                         : emptySig);

    bool lineStartsTypeBlock = false;
    if (!inFunction && !tokens.empty() &&
        tokens[0].kind == LexToken::Kind::Identifier &&
        (tokens[0].text == "struct" || tokens[0].text == "cbuffer")) {
      bool hasSemicolon = false;
      for (const auto &token : tokens) {
        if (token.kind == LexToken::Kind::Punct && token.text == ";") {
          hasSemicolon = true;
          break;
        }
      }
      lineStartsTypeBlock = !hasSemicolon;
    }
    if (lineStartsTypeBlock)
      pendingTypeBlockOpen = true;

    if (!inFunction && typeBlockBraceDepth == 0 && !pendingTypeBlockOpen &&
        pendingUiVarName.empty() && tokens.size() >= 2) {
      size_t typeIndex = std::string::npos;
      for (size_t t = 0; t < tokens.size(); t++) {
        if (tokens[t].kind != LexToken::Kind::Identifier)
          continue;
        if (isQualifierToken(tokens[t].text))
          continue;
        if (tokens[t].text == "return" || tokens[t].text == "if" ||
            tokens[t].text == "for" || tokens[t].text == "while" ||
            tokens[t].text == "switch" || tokens[t].text == "struct" ||
            tokens[t].text == "cbuffer")
          break;
        typeIndex = t;
        break;
      }
      if (typeIndex != std::string::npos) {
        size_t nameIndex = std::string::npos;
        for (size_t t = typeIndex + 1; t < tokens.size(); t++) {
          if (tokens[t].kind != LexToken::Kind::Identifier)
            continue;
          if (isQualifierToken(tokens[t].text))
            continue;
          nameIndex = t;
          break;
        }
        if (nameIndex != std::string::npos) {
          bool disqualify = false;
          for (const auto &t : tokens) {
            if (t.kind != LexToken::Kind::Punct)
              continue;
            if (t.text == ";" || t.text == "=" || t.text == "(" ||
                t.text == "{" || t.text == "}" || t.text == "<" ||
                t.text == ">") {
              disqualify = true;
              break;
            }
          }
          if (!disqualify) {
            pendingUiVarName = tokens[nameIndex].text;
            pendingUiVarLine = lineIndex;
            pendingUiVarStart = static_cast<int>(tokens[nameIndex].start);
            pendingUiVarEnd = static_cast<int>(tokens[nameIndex].end);
          }
        }
      }
    }

    if (!pendingUiVarName.empty() && !inUiMetaBlock) {
      if (tokens.size() == 1 && tokens[0].kind == LexToken::Kind::Punct &&
          tokens[0].text == "<") {
        if (!globalSymbols.insert(pendingUiVarName).second) {
          diags.a.push_back(makeDiagnostic(
              text, pendingUiVarLine, pendingUiVarStart, pendingUiVarEnd, 2,
              "nsf",
              "Duplicate global declaration: " + pendingUiVarName + "."));
        }
        inUiMetaBlock = true;
        lineIndex++;
        continue;
      }
      if (!tokens.empty()) {
        pendingUiVarName.clear();
        pendingUiVarLine = -1;
        pendingUiVarStart = -1;
        pendingUiVarEnd = -1;
      }
    }

    if (inUiMetaBlock) {
      if (!tokens.empty() && tokens[0].kind == LexToken::Kind::Punct &&
          tokens[0].text == ">") {
        inUiMetaBlock = false;
        pendingUiVarName.clear();
        pendingUiVarLine = -1;
        pendingUiVarStart = -1;
        pendingUiVarEnd = -1;
      } else {
        lineIndex++;
        continue;
      }
    }

    if (!inFunction && typeBlockBraceDepth == 0 && !pendingTypeBlockOpen &&
        tokens.size() >= 2) {
      size_t typeIndex = std::string::npos;
      for (size_t t = 0; t < tokens.size(); t++) {
        if (tokens[t].kind != LexToken::Kind::Identifier)
          continue;
        if (isQualifierToken(tokens[t].text))
          continue;
        if (tokens[t].text == "return" || tokens[t].text == "if" ||
            tokens[t].text == "for" || tokens[t].text == "while" ||
            tokens[t].text == "switch" || tokens[t].text == "struct" ||
            tokens[t].text == "cbuffer")
          break;
        typeIndex = t;
        break;
      }
      if (typeIndex != std::string::npos && typeIndex + 1 < tokens.size()) {
        if (tokens[typeIndex + 1].kind == LexToken::Kind::Identifier) {
          bool hasSemi = false;
          for (const auto &t : tokens) {
            if (t.kind == LexToken::Kind::Punct && t.text == ";") {
              hasSemi = true;
              break;
            }
          }
          if (hasSemi) {
            const std::string name = tokens[typeIndex + 1].text;
            if (name.rfind("SasUi", 0) == 0) {
              lineIndex++;
              continue;
            }
            if (!globalSymbols.insert(name).second) {
              diags.a.push_back(makeDiagnostic(
                  text, lineIndex,
                  static_cast<int>(tokens[typeIndex + 1].start),
                  static_cast<int>(tokens[typeIndex + 1].end), 2, "nsf",
                  "Duplicate global declaration: " + name + "."));
            }
          }
        }
      }
    }

    bool signatureStartedThisLine = false;
    if (!inFunction && !pendingSignature) {
      size_t typeIndex = std::string::npos;
      for (size_t t = 0; t < tokens.size(); t++) {
        if (tokens[t].kind != LexToken::Kind::Identifier)
          continue;
        if (isQualifierToken(tokens[t].text))
          continue;
        if (tokens[t].text == "return" || tokens[t].text == "if" ||
            tokens[t].text == "for" || tokens[t].text == "while" ||
            tokens[t].text == "switch" || tokens[t].text == "struct" ||
            tokens[t].text == "cbuffer")
          break;
        typeIndex = t;
        break;
      }
      if (typeIndex != std::string::npos && typeIndex + 2 < tokens.size()) {
        if (tokens[typeIndex + 1].kind == LexToken::Kind::Identifier &&
            tokens[typeIndex + 2].kind == LexToken::Kind::Punct &&
            tokens[typeIndex + 2].text == "(") {
          pendingReturnType = tokens[typeIndex].text;
          pendingFunctionName = tokens[typeIndex + 1].text;
          pendingFunctionLine = lineIndex;
          pendingFunctionStart = static_cast<int>(tokens[typeIndex + 1].start);
          pendingSignature = true;
          signatureStartedThisLine = true;
          pendingParams.clear();
          pendingParamTypesOrdered.clear();

          int parenDepth = 0;
          size_t openIndex = typeIndex + 2;
          size_t closeIndex = std::string::npos;
          for (size_t i = openIndex; i < tokens.size(); i++) {
            if (tokens[i].kind != LexToken::Kind::Punct)
              continue;
            if (tokens[i].text == "(") {
              parenDepth++;
              continue;
            }
            if (tokens[i].text == ")") {
              parenDepth--;
              if (parenDepth == 0) {
                closeIndex = i;
                break;
              }
            }
          }
          size_t paramEndExclusive =
              (closeIndex != std::string::npos ? closeIndex : tokens.size());
          collectSignatureParamsFromTokens(tokens, openIndex + 1,
                                           paramEndExclusive, lineIndex);
        }
      }
    }

    if (!inFunction && pendingSignature && !signatureStartedThisLine) {
      int parenDepth = 0;
      size_t closeIndex = std::string::npos;
      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind != LexToken::Kind::Punct)
          continue;
        if (tokens[i].text == "(") {
          parenDepth++;
          continue;
        }
        if (tokens[i].text == ")") {
          if (parenDepth == 0) {
            closeIndex = i;
            break;
          }
          parenDepth--;
        }
      }
      size_t paramEndExclusive =
          (closeIndex != std::string::npos ? closeIndex : tokens.size());
      collectSignatureParamsFromTokens(tokens, 0, paramEndExclusive, lineIndex);
    }

    if (inFunction) {
      rebuildVisibleLocals(currentSig);
      size_t declarationEqTokenIndex = std::string::npos;
      if (!pendingMultilineLocalName.empty()) {
        bool hasSemi = false;
        for (const auto &t : tokens) {
          if (t.kind == LexToken::Kind::Punct && t.text == ";") {
            hasSemi = true;
            break;
          }
        }
        if (hasSemi && pendingMultilineLocalDepth == functionBraceDepth) {
          pendingMultilineLocalName.clear();
          pendingMultilineLocalDepth = -1;
          pendingMultilineLocalLine = -1;
          pendingMultilineLocalStart = -1;
          pendingMultilineLocalEnd = -1;
        }
      }

      if (functionBraceDepth == 1) {
        if (!tokens.empty() && tokens[0].kind == LexToken::Kind::Identifier) {
          if (tokens[0].text == "if") {
            lastIfLineAtDepth1 = lineIndex;
            lastIfHadElseAtDepth1 = false;
          } else if (tokens[0].text == "else") {
            lastIfHadElseAtDepth1 = true;
          }
        }
      }

      if (unreachableActive && functionBraceDepth == unreachableDepth) {
        for (const auto &token : tokens) {
          if (token.kind == LexToken::Kind::Punct &&
              (token.text == ";" || token.text == "}"))
            continue;
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, static_cast<int>(token.start),
              static_cast<int>(token.end), 2, "nsf", "Unreachable code."));
          unreachableActive = false;
          break;
        }
      }

      for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        const std::string kw = tokens[i].text;
        if (kw != "if" && kw != "for" && kw != "while" && kw != "switch")
          continue;
        if (tokens[i + 1].kind != LexToken::Kind::Punct ||
            tokens[i + 1].text != "(") {
          diags.a.push_back(
              makeDiagnostic(text, lineIndex, static_cast<int>(tokens[i].start),
                             static_cast<int>(tokens[i].end), 1, "nsf",
                             "Missing parentheses after " + kw + "."));
        }
      }

      bool handledSharedLocalDeclarations = false;
      bool lineHasSemicolon = false;
      for (const auto &t : tokens) {
        if (t.kind == LexToken::Kind::Punct && t.text == ";") {
          lineHasSemicolon = true;
          break;
        }
      }
      if (lineHasSemicolon) {
        const auto declaredNames = extractDeclaredNamesFromLine(lineText);
        if (!declaredNames.empty()) {
          handledSharedLocalDeclarations = true;
          auto findNameToken =
              [&](const std::string &name) -> const LexToken * {
            for (size_t ti = 0; ti < tokens.size(); ti++) {
              if (tokens[ti].kind != LexToken::Kind::Identifier ||
                  tokens[ti].text != name) {
                continue;
              }
              if (ti > 0 && tokens[ti - 1].kind == LexToken::Kind::Punct &&
                  (tokens[ti - 1].text == "." || tokens[ti - 1].text == "->" ||
                   tokens[ti - 1].text == "::")) {
                continue;
              }
              return &tokens[ti];
            }
            return nullptr;
          };
          bool localsChanged = false;
          for (const auto &name : declaredNames) {
            std::string localType;
            if (!findTypeOfIdentifierInDeclarationLineShared(lineText, name,
                                                             localType)) {
              continue;
            }
            const LexToken *nameToken = findNameToken(name);
            int nameStart = nameToken ? static_cast<int>(nameToken->start) : 0;
            int nameEnd = nameToken ? static_cast<int>(nameToken->end) : 0;
            if (paramNames.find(name) != paramNames.end()) {
              diags.a.push_back(
                  makeDiagnostic(text, lineIndex, nameStart, nameEnd, 2, "nsf",
                                 "Local shadows parameter: " + name + "."));
            } else {
              bool duplicate = false;
              auto it = localsByName.find(name);
              if (it != localsByName.end()) {
                for (const auto &decl : it->second) {
                  if (preprocBranchSigsOverlap(decl.sig, currentSig)) {
                    duplicate = true;
                    break;
                  }
                }
              }
              if (duplicate) {
                diags.a.push_back(makeDiagnostic(
                    text, lineIndex, nameStart, nameEnd, 2, "nsf",
                    "Duplicate local declaration: " + name + "."));
              }
            }
            localsByName[name].push_back(
                LocalDeclEntry{normalizeTypeToken(localType), currentSig});
            localsChanged = true;
          }
          if (localsChanged)
            rebuildVisibleLocals(currentSig);
        }
      }

      size_t typeIndex = std::string::npos;
      for (size_t t = 0; t < tokens.size(); t++) {
        if (tokens[t].kind != LexToken::Kind::Identifier)
          continue;
        if (isQualifierToken(tokens[t].text))
          continue;
        if (tokens[t].text == "return" || tokens[t].text == "if" ||
            tokens[t].text == "for" || tokens[t].text == "while" ||
            tokens[t].text == "switch")
          break;
        typeIndex = t;
        break;
      }
      if (!handledSharedLocalDeclarations && typeIndex != std::string::npos &&
          typeIndex + 1 < tokens.size()) {
        if (tokens[typeIndex + 1].kind == LexToken::Kind::Identifier) {
          bool hasSemi = false;
          bool hasEq = false;
          for (const auto &t : tokens) {
            if (t.kind != LexToken::Kind::Punct)
              continue;
            if (t.text == ";") {
              hasSemi = true;
              break;
            }
            if (t.text == "=")
              hasEq = true;
          }
          if (hasSemi) {
            const std::string name = tokens[typeIndex + 1].text;
            if (paramNames.find(name) != paramNames.end()) {
              diags.a.push_back(makeDiagnostic(
                  text, lineIndex,
                  static_cast<int>(tokens[typeIndex + 1].start),
                  static_cast<int>(tokens[typeIndex + 1].end), 2, "nsf",
                  "Local shadows parameter: " + name + "."));
            } else {
              bool duplicate = false;
              auto it = localsByName.find(name);
              if (it != localsByName.end()) {
                for (const auto &decl : it->second) {
                  if (preprocBranchSigsOverlap(decl.sig, currentSig)) {
                    duplicate = true;
                    break;
                  }
                }
              }
              if (duplicate) {
                diags.a.push_back(makeDiagnostic(
                    text, lineIndex,
                    static_cast<int>(tokens[typeIndex + 1].start),
                    static_cast<int>(tokens[typeIndex + 1].end), 2, "nsf",
                    "Duplicate local declaration: " + name + "."));
              }
            }
            localsByName[name].push_back(
                LocalDeclEntry{tokens[typeIndex].text, currentSig});
            rebuildVisibleLocals(currentSig);

            for (size_t k = typeIndex + 2; k + 1 < tokens.size(); k++) {
              if (tokens[k].kind == LexToken::Kind::Punct &&
                  tokens[k].text == "=") {
                declarationEqTokenIndex = k;
                std::string lhsType =
                    normalizeTypeToken(tokens[typeIndex].text);
                TypeEvalResult rhsEval;
                rhsEval.type = normalizeTypeToken(inferExpressionTypeFromTokens(
                    tokens, k + 1, localsVisibleTypes, text, scanRoots,
                    scanExtensions, structCache, symbolCache, &fileTextCache));
                rhsEval.confidence = TypeEvalConfidence::L2;
                if (rhsEval.type.empty() && isHalfFamilyType(lhsType)) {
                  rhsEval.type = inferNarrowingFallbackRhsTypeFromTokens(
                      tokens, k + 1, tokens.size());
                  if (!rhsEval.type.empty()) {
                    rhsEval.confidence = TypeEvalConfidence::L3;
                    rhsEval.reasonCode =
                        IndeterminateReason::DiagnosticsHeavyRulesSkipped;
                  } else {
                    rhsEval.reasonCode =
                        IndeterminateReason::DiagnosticsRhsTypeEmpty;
                    emitIndeterminate(
                        lineIndex, static_cast<int>(tokens[k].start),
                        static_cast<int>(tokens[k].end),
                        "NSF_INDET_RHS_TYPE_EMPTY", rhsEval.reasonCode,
                        "Indeterminate assignment type: rhs type unavailable.");
                  }
                }
                const std::string rhsType = rhsEval.type;
                if (!lhsType.empty() && !rhsType.empty() &&
                    lhsType != rhsType &&
                    !(isNumericScalarType(lhsType) &&
                      isNumericScalarType(rhsType) && lhsType != "half")) {
                  int severity =
                      isNarrowingPrecisionAssignment(lhsType, rhsType) ? 2 : 1;
                  diags.a.push_back(makeDiagnostic(
                      text, lineIndex, static_cast<int>(tokens[k].start),
                      static_cast<int>(tokens[k].end), severity, "nsf",
                      "Assignment type mismatch: " + lhsType + " = " + rhsType +
                          "."));
                }
                break;
              }
            }
          } else if (hasEq) {
            const std::string name = tokens[typeIndex + 1].text;
            if (paramNames.find(name) != paramNames.end()) {
              diags.a.push_back(makeDiagnostic(
                  text, lineIndex,
                  static_cast<int>(tokens[typeIndex + 1].start),
                  static_cast<int>(tokens[typeIndex + 1].end), 2, "nsf",
                  "Local shadows parameter: " + name + "."));
            } else {
              bool duplicate = false;
              auto it = localsByName.find(name);
              if (it != localsByName.end()) {
                for (const auto &decl : it->second) {
                  if (preprocBranchSigsOverlap(decl.sig, currentSig)) {
                    duplicate = true;
                    break;
                  }
                }
              }
              if (duplicate) {
                diags.a.push_back(makeDiagnostic(
                    text, lineIndex,
                    static_cast<int>(tokens[typeIndex + 1].start),
                    static_cast<int>(tokens[typeIndex + 1].end), 2, "nsf",
                    "Duplicate local declaration: " + name + "."));
              }
            }
            localsByName[name].push_back(
                LocalDeclEntry{tokens[typeIndex].text, currentSig});
            rebuildVisibleLocals(currentSig);
            pendingMultilineLocalName = name;
            pendingMultilineLocalDepth = functionBraceDepth;
            pendingMultilineLocalLine = lineIndex;
            pendingMultilineLocalStart =
                static_cast<int>(tokens[typeIndex + 1].start);
            pendingMultilineLocalEnd =
                static_cast<int>(tokens[typeIndex + 1].end);
          }
        }
      }

      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind != LexToken::Kind::Identifier ||
            tokens[i].text != "return")
          continue;
        sawReturn = true;
        if (functionBraceDepth == 1) {
          if (lastIfLineAtDepth1 == lineIndex - 1 && !lastIfHadElseAtDepth1) {
            conditionalReturnSeen = true;
          } else {
            unconditionalReturnSeen = true;
          }
          sawTopLevelReturn = true;
        }
        bool hasValue = true;
        size_t exprStart = i + 1;
        if (exprStart >= tokens.size()) {
          hasValue = false;
        } else if (tokens[exprStart].kind == LexToken::Kind::Punct &&
                   tokens[exprStart].text == ";") {
          hasValue = false;
        }

        const std::string normalizedReturnType =
            normalizeTypeToken(functionReturnType);
        if (normalizedReturnType == "void") {
          if (hasValue) {
            diags.a.push_back(makeDiagnostic(
                text, lineIndex, static_cast<int>(tokens[i].start),
                static_cast<int>(tokens[i].end), 1, "nsf",
                "Return value in void function."));
          }
          bool hasSemi = false;
          for (const auto &t : tokens) {
            if (t.kind == LexToken::Kind::Punct && t.text == ";") {
              hasSemi = true;
              break;
            }
          }
          if (!hasSemi) {
            diags.a.push_back(makeDiagnostic(text, lineIndex,
                                             static_cast<int>(tokens[i].start),
                                             static_cast<int>(tokens[i].end), 1,
                                             "nsf", "Missing semicolon."));
          }
          unreachableActive = true;
          unreachableDepth = functionBraceDepth;
          continue;
        }

        if (!hasValue) {
          diags.a.push_back(makeDiagnostic(text, lineIndex,
                                           static_cast<int>(tokens[i].start),
                                           static_cast<int>(tokens[i].end), 1,
                                           "nsf", "Missing return value."));
          continue;
        }

        std::string exprType = inferExpressionTypeFromTokens(
            tokens, exprStart, localsVisibleTypes, text, scanRoots,
            scanExtensions, structCache, symbolCache, &fileTextCache);
        exprType = normalizeTypeToken(exprType);
        if (!exprType.empty() && exprType != normalizedReturnType) {
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end), 1, "nsf",
              "Return type mismatch: expected " + normalizedReturnType +
                  " but got " + exprType + "."));
        }
        bool hasSemi = false;
        for (const auto &t : tokens) {
          if (t.kind == LexToken::Kind::Punct && t.text == ";") {
            hasSemi = true;
            break;
          }
        }
        if (!hasSemi) {
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end), 1, "nsf", "Missing semicolon."));
        }
        unreachableActive = true;
        unreachableDepth = functionBraceDepth;
      }

      for (size_t i = 0; i + 2 < tokens.size(); i++) {
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        if (tokens[i + 1].kind != LexToken::Kind::Punct ||
            tokens[i + 1].text != "=")
          continue;
        if (declarationEqTokenIndex != std::string::npos &&
            i + 1 == declarationEqTokenIndex)
          continue;
        if (tokens[i + 2].kind == LexToken::Kind::Punct &&
            tokens[i + 2].text == "=")
          continue;
        if (i > 0 && tokens[i - 1].kind == LexToken::Kind::Punct &&
            (tokens[i - 1].text == "." || tokens[i - 1].text == "->" ||
             tokens[i - 1].text == "::"))
          continue;
        auto it = localsVisibleTypes.find(tokens[i].text);
        if (it == localsVisibleTypes.end())
          continue;
        std::string lhsType = normalizeTypeToken(it->second);
        TypeEvalResult rhsEval;
        rhsEval.type = normalizeTypeToken(inferExpressionTypeFromTokens(
            tokens, i + 2, localsVisibleTypes, text, scanRoots, scanExtensions,
            structCache, symbolCache, &fileTextCache));
        rhsEval.confidence = TypeEvalConfidence::L2;
        if (rhsEval.type.empty() && isHalfFamilyType(lhsType)) {
          rhsEval.type = inferNarrowingFallbackRhsTypeFromTokens(tokens, i + 2,
                                                                 tokens.size());
          if (!rhsEval.type.empty()) {
            rhsEval.confidence = TypeEvalConfidence::L3;
            rhsEval.reasonCode =
                IndeterminateReason::DiagnosticsHeavyRulesSkipped;
          } else {
            rhsEval.reasonCode = IndeterminateReason::DiagnosticsRhsTypeEmpty;
            emitIndeterminate(
                lineIndex, static_cast<int>(tokens[i + 1].start),
                static_cast<int>(tokens[i + 1].end), "NSF_INDET_RHS_TYPE_EMPTY",
                rhsEval.reasonCode,
                "Indeterminate assignment type: rhs type unavailable.");
          }
        }
        const std::string rhsType = rhsEval.type;
        if (!lhsType.empty() && !rhsType.empty() && lhsType != rhsType &&
            !(isNumericScalarType(lhsType) && isNumericScalarType(rhsType) &&
              lhsType != "half")) {
          int severity =
              isNarrowingPrecisionAssignment(lhsType, rhsType) ? 2 : 1;
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, static_cast<int>(tokens[i + 1].start),
              static_cast<int>(tokens[i + 1].end), severity, "nsf",
              "Assignment type mismatch: " + lhsType + " = " + rhsType + "."));
        }
        bool hasSemi = false;
        for (const auto &t : tokens) {
          if (t.kind == LexToken::Kind::Punct && t.text == ";") {
            hasSemi = true;
            break;
          }
        }
        if (!hasSemi) {
          if (!(tokens[i].text == pendingMultilineLocalName &&
                pendingMultilineLocalDepth == functionBraceDepth)) {
            diags.a.push_back(makeDiagnostic(
                text, lineIndex, static_cast<int>(tokens[i + 1].start),
                static_cast<int>(tokens[i + 1].end), 1, "nsf",
                "Missing semicolon."));
          }
        }
      }

      for (size_t i = 0; i + 2 < tokens.size(); i++) {
        auto inferOperandType = [&](size_t index) -> std::string {
          if (index >= tokens.size())
            return "";

          auto skipBalancedAt = [&](size_t start, const std::string &open,
                                    const std::string &close) -> size_t {
            if (start >= tokens.size() ||
                tokens[start].kind != LexToken::Kind::Punct ||
                tokens[start].text != open) {
              return start;
            }
            int depth = 0;
            size_t cursor = start;
            while (cursor < tokens.size()) {
              if (tokens[cursor].kind == LexToken::Kind::Punct) {
                if (tokens[cursor].text == open)
                  depth++;
                else if (tokens[cursor].text == close) {
                  depth--;
                  if (depth == 0)
                    return cursor + 1;
                }
              }
              cursor++;
            }
            return tokens.size();
          };

          auto applyPostfixType = [&](size_t cursor,
                                      std::string baseType) -> std::string {
            std::string current = normalizeTypeToken(baseType);
            while (cursor < tokens.size()) {
              const LexToken &tok = tokens[cursor];
              if (tok.kind == LexToken::Kind::Punct && tok.text == "[") {
                cursor = skipBalancedAt(cursor, "[", "]");
                current = applyIndexAccessType(current);
                continue;
              }
              if (tok.kind == LexToken::Kind::Punct && tok.text == "." &&
                  cursor + 1 < tokens.size() &&
                  tokens[cursor + 1].kind == LexToken::Kind::Identifier) {
                cursor += 2;
                if (isSwizzleToken(tokens[cursor - 1].text)) {
                  const std::string swizzled =
                      applySwizzleType(current, tokens[cursor - 1].text);
                  if (!swizzled.empty())
                    current = swizzled;
                }
                continue;
              }
              break;
            }
            return current;
          };

          if (tokens[index].kind == LexToken::Kind::Identifier) {
            int dim = 0;
            int rows = 0;
            int cols = 0;
            if (isVectorType(tokens[index].text, dim) ||
                isScalarType(tokens[index].text) ||
                isMatrixType(tokens[index].text, rows, cols))
              return applyPostfixType(index + 1, tokens[index].text);
            auto it = localsVisibleTypes.find(tokens[index].text);
            if (it != localsVisibleTypes.end())
              return applyPostfixType(index + 1, it->second);
            std::string numeric =
                inferNumericLiteralTypeFromTokens(tokens, index);
            if (!numeric.empty())
              return applyPostfixType(index + 1, numeric);
            return applyPostfixType(index + 1,
                                    inferLiteralType(tokens[index].text));
          }
          return applyPostfixType(
              index + 1,
              normalizeTypeToken(inferLiteralType(tokens[index].text)));
        };

        std::string leftType = normalizeTypeToken(inferOperandType(i));
        if (leftType.empty())
          continue;
        if (tokens[i + 1].kind != LexToken::Kind::Punct)
          continue;
        const std::string op = tokens[i + 1].text;
        if (op != "+" && op != "-" && op != "*" && op != "/" && op != "==" &&
            op != "!=" && op != "<" && op != ">" && op != "<=" && op != ">=" &&
            op != "&&" && op != "||")
          continue;
        std::string rightType = normalizeTypeToken(inferOperandType(i + 2));
        if (rightType.empty())
          continue;

        if ((leftType == "bool" || rightType == "bool") &&
            (op == "+" || op == "-" || op == "*" || op == "/")) {
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, static_cast<int>(tokens[i + 1].start),
              static_cast<int>(tokens[i + 1].end), 1, "nsf",
              "Binary operator type mismatch: " + leftType + " " + op + " " +
                  rightType + "."));
          continue;
        }

        if ((op == "&&" || op == "||") &&
            (leftType != "bool" || rightType != "bool")) {
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, static_cast<int>(tokens[i + 1].start),
              static_cast<int>(tokens[i + 1].end), 1, "nsf",
              "Binary operator type mismatch: " + leftType + " " + op + " " +
                  rightType + "."));
          continue;
        }

        int leftDim = 0;
        int rightDim = 0;
        bool leftVec = isVectorType(leftType, leftDim);
        bool rightVec = isVectorType(rightType, rightDim);
        int leftRows = 0;
        int leftCols = 0;
        int rightRows = 0;
        int rightCols = 0;
        bool leftMat = isMatrixType(leftType, leftRows, leftCols);
        bool rightMat = isMatrixType(rightType, rightRows, rightCols);
        if (leftMat || rightMat) {
          bool ok = false;
          if (op == "+" || op == "-") {
            ok = (leftMat && rightMat && leftRows == rightRows &&
                  leftCols == rightCols) ||
                 (leftMat && isNumericScalarType(rightType)) ||
                 (rightMat && isNumericScalarType(leftType));
          } else if (op == "*") {
            ok = (leftMat && rightMat && leftCols == rightRows) ||
                 (leftMat && isNumericScalarType(rightType)) ||
                 (rightMat && isNumericScalarType(leftType)) ||
                 (leftVec && rightMat && leftDim == rightRows) ||
                 (leftMat && rightVec && leftCols == rightDim);
          } else if (op == "/") {
            ok = (leftMat && isNumericScalarType(rightType));
          }
          if (!ok) {
            diags.a.push_back(makeDiagnostic(
                text, lineIndex, static_cast<int>(tokens[i + 1].start),
                static_cast<int>(tokens[i + 1].end), 1, "nsf",
                "Binary operator type mismatch: " + leftType + " " + op + " " +
                    rightType + "."));
          }
          continue;
        }
        if (leftVec && rightVec && leftDim != rightDim) {
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, static_cast<int>(tokens[i + 1].start),
              static_cast<int>(tokens[i + 1].end), 1, "nsf",
              "Binary operator type mismatch: " + leftType + " " + op + " " +
                  rightType + "."));
          continue;
        }

        if ((op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" ||
             op == ">=")) {
          if (leftVec || rightVec) {
            if (!(leftVec && rightVec && leftDim == rightDim)) {
              diags.a.push_back(makeDiagnostic(
                  text, lineIndex, static_cast<int>(tokens[i + 1].start),
                  static_cast<int>(tokens[i + 1].end), 1, "nsf",
                  "Binary operator type mismatch: " + leftType + " " + op +
                      " " + rightType + "."));
              continue;
            }
          } else if (!(isNumericScalarType(leftType) &&
                       isNumericScalarType(rightType)) &&
                     !(leftType == "bool" && rightType == "bool")) {
            diags.a.push_back(makeDiagnostic(
                text, lineIndex, static_cast<int>(tokens[i + 1].start),
                static_cast<int>(tokens[i + 1].end), 1, "nsf",
                "Binary operator type mismatch: " + leftType + " " + op + " " +
                    rightType + "."));
            continue;
          }
        }

        if (leftVec && isNumericScalarType(rightType))
          continue;
        if (rightVec && isNumericScalarType(leftType))
          continue;
      }

      int builtinAttributeDepth = 0;
      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "[") {
          builtinAttributeDepth++;
          continue;
        }
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "]") {
          if (builtinAttributeDepth > 0)
            builtinAttributeDepth--;
          continue;
        }
        if (builtinAttributeDepth > 0)
          continue;
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        if (i + 1 >= tokens.size() ||
            tokens[i + 1].kind != LexToken::Kind::Punct ||
            tokens[i + 1].text != "(")
          continue;
        if (i > 0 && tokens[i - 1].kind == LexToken::Kind::Punct &&
            (tokens[i - 1].text == "." || tokens[i - 1].text == "->" ||
             tokens[i - 1].text == "::"))
          continue;
        const std::string name = tokens[i].text;
        int dim = 0;
        int rows = 0;
        int cols = 0;
        if (!isHlslBuiltinFunction(name))
          continue;
        if (isLikelyTypeConstructorCallName(name) || isVectorType(name, dim) ||
            isScalarType(name) || isMatrixType(name, rows, cols))
          continue;

        std::vector<std::pair<size_t, size_t>> argRanges;
        size_t closeParenIndex = std::string::npos;
        if (!collectCallArgumentTokenRanges(tokens, i + 1, argRanges,
                                            closeParenIndex)) {
          continue;
        }
        std::vector<std::string> argTypes;
        for (const auto &range : argRanges) {
          argTypes.push_back(
              normalizeTypeToken(inferExpressionTypeFromTokensRange(
                  tokens, range.first, range.second, localsVisibleTypes, text,
                  scanRoots, scanExtensions, structCache, symbolCache,
                  &fileTextCache)));
        }
        if (closeParenIndex == std::string::npos) {
          continue;
        }

        if (!isHlslBuiltinTypeCheckedFunction(name)) {
          emitIndeterminate(
              lineIndex, static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end), "NSF_INDET_BUILTIN_UNMODELED",
              IndeterminateReason::DiagnosticsBuiltinUnmodeled,
              "Indeterminate builtin call: type rules not implemented. Name: " +
                  name + ". Args: " + formatTypeList(argTypes) + ".");
          continue;
        }

        std::vector<BuiltinTypeInfo> infos;
        infos.reserve(argTypes.size());
        for (const auto &t : argTypes) {
          infos.push_back(parseBuiltinTypeInfo(t));
        }
        BuiltinResolveResult rr = resolveBuiltinCall(name, infos);
        if (rr.indeterminate) {
          emitIndeterminate(
              lineIndex, static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end),
              "NSF_INDET_BUILTIN_ARG_TYPE_UNKNOWN",
              IndeterminateReason::DiagnosticsBuiltinArgTypeUnknown,
              "Indeterminate builtin call: arg types unavailable. Name: " +
                  name + ". Args: " + formatTypeList(argTypes) + ".");
        } else if (rr.warnMixedSignedness) {
          diags.a.push_back(
              makeDiagnostic(text, lineIndex, static_cast<int>(tokens[i].start),
                             static_cast<int>(tokens[i].end), 2, "nsf",
                             "Builtin call mixed integer signedness: " + name +
                                 ". Args: " + formatTypeList(argTypes) + "."));
        } else if (!rr.ok) {
          diags.a.push_back(
              makeDiagnostic(text, lineIndex, static_cast<int>(tokens[i].start),
                             static_cast<int>(tokens[i].end), 1, "nsf",
                             "Builtin call type mismatch: " + name +
                                 ". Args: " + formatTypeList(argTypes) + "."));
        }
      }

      int methodCallAttributeDepth = 0;
      for (size_t i = 0; i + 3 < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "[") {
          methodCallAttributeDepth++;
          continue;
        }
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "]") {
          if (methodCallAttributeDepth > 0)
            methodCallAttributeDepth--;
          continue;
        }
        if (methodCallAttributeDepth > 0)
          continue;
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        if (tokens[i + 1].kind != LexToken::Kind::Punct ||
            tokens[i + 1].text != ".")
          continue;
        if (tokens[i + 2].kind != LexToken::Kind::Identifier)
          continue;
        if (tokens[i + 3].kind != LexToken::Kind::Punct ||
            tokens[i + 3].text != "(")
          continue;

        const std::string baseName = tokens[i].text;
        auto baseIt = localsVisibleTypes.find(baseName);
        if (baseIt == localsVisibleTypes.end())
          continue;
        const std::string baseType = normalizeTypeToken(baseIt->second);
        if (!isTypeModelTextureLike(baseType))
          continue;

        const std::string member = tokens[i + 2].text;
        std::vector<std::pair<size_t, size_t>> argRanges;
        size_t closeParenIndex = std::string::npos;
        if (!collectCallArgumentTokenRanges(tokens, i + 3, argRanges,
                                            closeParenIndex)) {
          continue;
        }
        std::vector<std::string> argTypes;
        for (const auto &range : argRanges) {
          argTypes.push_back(
              normalizeTypeToken(inferExpressionTypeFromTokensRange(
                  tokens, range.first, range.second, localsVisibleTypes, text,
                  scanRoots, scanExtensions, structCache, symbolCache,
                  &fileTextCache)));
        }
        if (closeParenIndex == std::string::npos)
          continue;

        int dim = getTypeModelCoordDim(baseType);
        if (dim < 1)
          dim = 2;

        auto isSamplerLike = [&](const std::string &t) -> bool {
          TypeDesc desc = parseTypeDesc(t);
          if (desc.kind == TypeDescKind::Object &&
              (desc.objectKind == ObjectTypeKind::Sampler ||
               desc.objectKind == ObjectTypeKind::SamplerState))
            return true;
          return isTypeModelSamplerLike(t);
        };

        auto isFloatFamilyCoord = [&](const std::string &t, int d) -> bool {
          TypeDesc desc = parseTypeDesc(t);
          if (d <= 1) {
            return desc.kind == TypeDescKind::Scalar &&
                   (desc.base == "float" || desc.base == "half" ||
                    desc.base == "double");
          }
          return desc.kind == TypeDescKind::Vector && desc.rows == d &&
                 (desc.base == "float" || desc.base == "half" ||
                  desc.base == "double");
        };

        auto isIntFamilyCoord = [&](const std::string &t, int d) -> bool {
          TypeDesc desc = parseTypeDesc(t);
          if (d <= 1) {
            return desc.kind == TypeDescKind::Scalar &&
                   (desc.base == "int" || desc.base == "uint");
          }
          return desc.kind == TypeDescKind::Vector && desc.rows == d &&
                 (desc.base == "int" || desc.base == "uint");
        };

        auto anyArgUnknown = [&]() -> bool {
          for (const auto &t : argTypes) {
            if (t.empty())
              return true;
          }
          return false;
        };

        auto emitUnmodeled = [&]() {
          emitIndeterminate(
              lineIndex, static_cast<int>(tokens[i + 2].start),
              static_cast<int>(tokens[i + 2].end),
              "NSF_INDET_BUILTIN_METHOD_UNMODELED",
              IndeterminateReason::DiagnosticsBuiltinMethodUnmodeled,
              "Indeterminate built-in method call: type rules not implemented. "
              "Base: " +
                  baseType + ". Method: " + member +
                  ". Args: " + formatTypeList(argTypes) + ".");
        };

        auto emitArgUnknown = [&]() {
          emitIndeterminate(
              lineIndex, static_cast<int>(tokens[i + 2].start),
              static_cast<int>(tokens[i + 2].end),
              "NSF_INDET_BUILTIN_METHOD_ARG_TYPE_UNKNOWN",
              IndeterminateReason::DiagnosticsBuiltinMethodArgTypeUnknown,
              "Indeterminate built-in method call: arg types unavailable. "
              "Base: " +
                  baseType + ". Method: " + member +
                  ". Args: " + formatTypeList(argTypes) + ".");
        };

        auto emitMismatch = [&]() {
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, static_cast<int>(tokens[i + 2].start),
              static_cast<int>(tokens[i + 2].end), 1, "nsf",
              "Built-in method call type mismatch: " + member + ". Base: " +
                  baseType + ". Args: " + formatTypeList(argTypes) + "."));
        };

        HlslBuiltinMethodRule methodRule;
        if (!lookupHlslBuiltinMethodRule(member, baseType, methodRule)) {
          emitUnmodeled();
          continue;
        }
        if (anyArgUnknown()) {
          emitArgUnknown();
          continue;
        }
        if (argTypes.size() < static_cast<size_t>(methodRule.minArgs) ||
            argTypes.size() > static_cast<size_t>(methodRule.maxArgs)) {
          emitMismatch();
          continue;
        }

        if (member == "Sample" || member == "SampleLevel" ||
            member == "SampleBias" || member == "SampleGrad" ||
            member == "SampleCmp" || member == "SampleCmpLevelZero" ||
            member == "Gather" || member == "GatherRed" ||
            member == "GatherGreen" || member == "GatherBlue" ||
            member == "GatherAlpha") {
          if (!isSamplerLike(argTypes[0]) ||
              !isFloatFamilyCoord(argTypes[1], dim)) {
            emitMismatch();
            continue;
          }
          if (member == "SampleLevel" || member == "SampleBias") {
            if (!isFloatFamilyCoord(argTypes[2], 1)) {
              emitMismatch();
              continue;
            }
          } else if (member == "SampleGrad") {
            if (!isFloatFamilyCoord(argTypes[2], dim) ||
                !isFloatFamilyCoord(argTypes[3], dim)) {
              emitMismatch();
              continue;
            }
          } else if (member == "SampleCmp" || member == "SampleCmpLevelZero") {
            if (!isFloatFamilyCoord(argTypes[2], 1)) {
              emitMismatch();
              continue;
            }
          }
          continue;
        }

        if (member == "Load") {
          if (!isIntFamilyCoord(argTypes[0], dim + 1)) {
            emitMismatch();
            continue;
          }
          continue;
        }

        if (member == "GetDimensions") {
          continue;
        }

        emitUnmodeled();
      }

      int userCallAttributeDepth = 0;
      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "[") {
          userCallAttributeDepth++;
          continue;
        }
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "]") {
          if (userCallAttributeDepth > 0)
            userCallAttributeDepth--;
          continue;
        }
        if (userCallAttributeDepth > 0)
          continue;
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        if (i + 1 >= tokens.size() ||
            tokens[i + 1].kind != LexToken::Kind::Punct ||
            tokens[i + 1].text != "(")
          continue;
        if (i > 0 && tokens[i - 1].kind == LexToken::Kind::Punct &&
            (tokens[i - 1].text == "." || tokens[i - 1].text == "->" ||
             tokens[i - 1].text == "::"))
          continue;
        const std::string name = tokens[i].text;
        if (isBuiltinOrKeyword(name))
          continue;
        if (isHlslBuiltinTypeCheckedFunction(name))
          continue;
        int dim = 0;
        int rows = 0;
        int cols = 0;
        if (isLikelyTypeConstructorCallName(name) || isVectorType(name, dim) ||
            isScalarType(name) || isMatrixType(name, rows, cols))
          continue;
        std::vector<std::pair<size_t, size_t>> argRanges;
        size_t closeParenIndex = std::string::npos;
        if (!collectCallArgumentTokenRanges(tokens, i + 1, argRanges,
                                            closeParenIndex)) {
          continue;
        }
        std::vector<std::string> argTypes;
        for (const auto &range : argRanges) {
          argTypes.push_back(
              normalizeTypeToken(inferExpressionTypeFromTokensRange(
                  tokens, range.first, range.second, localsVisibleTypes, text,
                  scanRoots, scanExtensions, structCache, symbolCache,
                  &fileTextCache)));
        }
        if (closeParenIndex == std::string::npos)
          continue;

        const auto &candidates = getUserFunctionCandidates(name);
        if (candidates.empty())
          continue;

        std::vector<CandidateSignature> resolverCandidates;
        resolverCandidates.reserve(candidates.size());
        for (const auto &cand : candidates) {
          CandidateSignature resolverCandidate;
          resolverCandidate.name = name;
          resolverCandidate.displayLabel = cand.label;
          resolverCandidate.displayParams = cand.paramTypes;
          resolverCandidate.sourceUri = cand.loc.uri;
          resolverCandidate.sourceLine = cand.loc.line;
          resolverCandidate.visibilityCondition = "";
          resolverCandidate.params.reserve(cand.paramTypes.size());
          for (const auto &paramType : cand.paramTypes) {
            ParamDesc paramDesc;
            paramDesc.type = parseTypeDesc(paramType);
            resolverCandidate.params.push_back(std::move(paramDesc));
          }
          resolverCandidates.push_back(std::move(resolverCandidate));
        }
        std::vector<TypeDesc> resolverArgTypes;
        resolverArgTypes.reserve(argTypes.size());
        for (const auto &argType : argTypes)
          resolverArgTypes.push_back(parseTypeDesc(argType));
        ResolveCallContext resolveContext;
        resolveContext.defines = defines;
        resolveContext.allowNarrowing = false;
        resolveContext.enableVisibilityFiltering = false;
        ResolveCallResult resolveResult = resolveCallCandidates(
            resolverCandidates, resolverArgTypes, resolveContext);
        if (resolveResult.status == ResolveCallStatus::Resolved ||
            resolveResult.status == ResolveCallStatus::Ambiguous) {
          continue;
        }

        bool anyArityMatch = false;
        bool anyPerfect = false;
        int bestMismatches = 1000000;
        int bestCompared = -1;
        int bestScore = -1;
        size_t bestIndex = 0;
        for (size_t ci = 0; ci < candidates.size(); ci++) {
          const auto &cand = candidates[ci];
          if (cand.paramTypes.size() != argTypes.size())
            continue;
          anyArityMatch = true;
          int mismatches = 0;
          int compared = 0;
          int score = 0;
          for (size_t k = 0; k < argTypes.size() && k < cand.paramTypes.size();
               k++) {
            const std::string &expected = cand.paramTypes[k];
            const std::string &actual = argTypes[k];
            if (expected.empty() || actual.empty())
              continue;
            compared++;
            if (compatibleValueType(expected, actual)) {
              if (expected == actual)
                score += 3;
              else
                score += 1;
            } else {
              mismatches++;
            }
          }
          if (mismatches == 0) {
            anyPerfect = true;
            break;
          }
          if (mismatches < bestMismatches ||
              (mismatches == bestMismatches && compared > bestCompared) ||
              (mismatches == bestMismatches && compared == bestCompared &&
               score > bestScore)) {
            bestMismatches = mismatches;
            bestCompared = compared;
            bestScore = score;
            bestIndex = ci;
          }
        }

        if (anyPerfect)
          continue;

        auto buildDisplayTypes = [&](const std::vector<std::string> &types) {
          std::vector<std::string> out;
          out.reserve(types.size());
          for (const auto &t : types)
            out.push_back(t.empty() ? "?" : t);
          return out;
        };

        auto emitLowConfidenceFunctionIndeterminate =
            [&](const UserFunctionCandidate &best, const std::string &detail) {
              emitIndeterminate(
                  lineIndex, static_cast<int>(tokens[i].start),
                  static_cast<int>(tokens[i].end),
                  "NSF_INDET_FUNCTION_SIGNATURE_LOW_CONFIDENCE",
                  IndeterminateReason::
                      DiagnosticsFunctionSignatureLowConfidence,
                  "Indeterminate function call analysis: low-confidence "
                  "signature source for " +
                      name + ". " + detail + " Candidate at " +
                      formatLocationShort(best.loc) + ".");
            };

        if (anyArityMatch) {
          if (bestCompared <= 0)
            continue;
          const auto &best = candidates[bestIndex];
          if (best.confidence == FunctionCandidateConfidence::TextFallback &&
              best.loc.line == lineIndex) {
            std::string detail =
                "Expected " +
                formatTypeList(buildDisplayTypes(best.paramTypes)) + ", got " +
                formatTypeList(buildDisplayTypes(argTypes)) + ".";
            emitLowConfidenceFunctionIndeterminate(best, detail);
            continue;
          }
          std::string message =
              "Function call argument mismatch: " + name + ". Expected: " +
              formatTypeList(buildDisplayTypes(best.paramTypes)) +
              ". Got: " + formatTypeList(buildDisplayTypes(argTypes)) +
              ". Defined at: " + formatLocationShort(best.loc) + ".";
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end), 1, "nsf", message));
          continue;
        }

        const auto &best = candidates.front();
        if (best.confidence == FunctionCandidateConfidence::TextFallback &&
            best.loc.line == lineIndex) {
          std::ostringstream expectedCount;
          expectedCount << best.paramTypes.size();
          std::string detail = "Expected " + expectedCount.str() +
                               " argument(s), got " +
                               std::to_string(argTypes.size()) + ".";
          emitLowConfidenceFunctionIndeterminate(best, detail);
          continue;
        }
        std::ostringstream expectedCount;
        expectedCount << best.paramTypes.size();
        std::string message = "Function call argument count mismatch: " + name +
                              ". Expected " + expectedCount.str() +
                              " but got " + std::to_string(argTypes.size()) +
                              ". Defined at: " + formatLocationShort(best.loc) +
                              ".";
        diags.a.push_back(
            makeDiagnostic(text, lineIndex, static_cast<int>(tokens[i].start),
                           static_cast<int>(tokens[i].end), 1, "nsf", message));
      }

      int attributeDepth = 0;
      for (size_t i = 0; i < tokens.size(); i++) {
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "[") {
          attributeDepth++;
          continue;
        }
        if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "]") {
          if (attributeDepth > 0)
            attributeDepth--;
          continue;
        }
        if (attributeDepth > 0)
          continue;
        if (tokens[i].kind != LexToken::Kind::Identifier)
          continue;
        size_t span = numericLiteralTokenSpan(tokens, i);
        if (span == 3) {
          char suffix = 0;
          if (isSignedDigitsWithOptionalSuffixToken(tokens[i + 2].text,
                                                    suffix) &&
              (suffix == 'u' || suffix == 'U')) {
            diags.a.push_back(makeDiagnostic(
                text, lineIndex, static_cast<int>(tokens[i + 2].start),
                static_cast<int>(tokens[i + 2].end), 2, "nsf",
                std::string("Invalid numeric literal suffix: ") + suffix +
                    "."));
          }
          i += span - 1;
          continue;
        }
        if (span > 0) {
          i += span - 1;
          continue;
        }
        {
          const std::string &value = tokens[i].text;
          size_t start = 0;
          if (!value.empty() && (value[0] == '+' || value[0] == '-'))
            start = 1;
          if (start + 2 < value.size() && value[start] == '0' &&
              (value[start + 1] == 'x' || value[start + 1] == 'X')) {
            size_t k = start + 2;
            const size_t digitsStart = k;
            while (k < value.size() &&
                   std::isxdigit(static_cast<unsigned char>(value[k]))) {
              k++;
            }
            if (k > digitsStart) {
              if (k + 1 == value.size() &&
                  std::isalpha(static_cast<unsigned char>(value[k])) &&
                  !(value[k] == 'u' || value[k] == 'U')) {
                diags.a.push_back(makeDiagnostic(
                    text, lineIndex, static_cast<int>(tokens[i].start),
                    static_cast<int>(tokens[i].end), 2, "nsf",
                    std::string("Invalid numeric literal suffix: ") + value[k] +
                        "."));
              }
              continue;
            }
          }
        }
        char suffix = 0;
        if (isSignedDigitsWithSingleAlphaSuffixToken(tokens[i].text, suffix) &&
            !(suffix == 'h' || suffix == 'f' || suffix == 'F' ||
              suffix == 'u' || suffix == 'U')) {
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, static_cast<int>(tokens[i].start),
              static_cast<int>(tokens[i].end), 2, "nsf",
              std::string("Invalid numeric literal suffix: ") + suffix + "."));
          continue;
        }
        const std::string word = tokens[i].text;
        if (isBuiltinOrKeyword(word))
          continue;
        if (word.rfind("SasUi", 0) == 0)
          continue;
        if (isHlslSystemSemantic(word))
          continue;
        if (i > 0 && tokens[i - 1].kind == LexToken::Kind::Punct &&
            (tokens[i - 1].text == "." || tokens[i - 1].text == "->" ||
             tokens[i - 1].text == "::" || tokens[i - 1].text == ":" ||
             tokens[i - 1].text == "#"))
          continue;
        if (i + 1 < tokens.size() &&
            tokens[i + 1].kind == LexToken::Kind::Punct &&
            tokens[i + 1].text == ":")
          continue;
        if (i + 1 < tokens.size() &&
            tokens[i + 1].kind == LexToken::Kind::Punct &&
            tokens[i + 1].text == "(")
          continue;
        if (isKnownSymbol(word))
          continue;
        if (isExternallyResolvable(word))
          continue;
        diags.a.push_back(
            makeDiagnostic(text, lineIndex, static_cast<int>(tokens[i].start),
                           static_cast<int>(tokens[i].end), 2, "nsf",
                           "Undefined identifier: " + word + "."));
      }
    }

    if (inFunction && normalizeTypeToken(functionReturnType) != "void" &&
        sawReturn) {
      if (conditionalReturnSeen && !unconditionalReturnSeen) {
        sawPotentialMissingReturn = true;
      }
    }

    scanForBraces(lineText);
    lineIndex++;
  }
}

static void collectPreprocessorDiagnostics(const std::string &text,
                                           Json &diags) {
  struct ConditionalEntry {
    int line = 0;
    int start = 0;
    int end = 0;
    std::string directive;
  };

  std::istringstream stream(text);
  std::string lineText;
  int lineIndex = 0;
  bool inBlockComment = false;
  bool inString = false;
  std::vector<ConditionalEntry> stack;

  auto pushEntry = [&](int line, int start, int end, const std::string &name) {
    stack.push_back(ConditionalEntry{line, start, end, name});
  };

  while (std::getline(stream, lineText)) {
    for (size_t i = 0; i < lineText.size(); i++) {
      char ch = lineText[i];
      char next = (i + 1 < lineText.size()) ? lineText[i + 1] : '\0';

      if (inBlockComment) {
        if (ch == '*' && next == '/') {
          inBlockComment = false;
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
        break;
      }

      if (ch == '/' && next == '*') {
        inBlockComment = true;
        i++;
        continue;
      }

      if (std::isspace(static_cast<unsigned char>(ch))) {
        continue;
      }

      if (ch != '#') {
        break;
      }

      size_t directiveStart = i;
      size_t j = i + 1;
      while (j < lineText.size() &&
             std::isspace(static_cast<unsigned char>(lineText[j]))) {
        j++;
      }
      size_t wordStart = j;
      while (j < lineText.size() && isIdentifierChar(lineText[j]))
        j++;
      if (j == wordStart)
        break;
      std::string directive = lineText.substr(wordStart, j - wordStart);
      int spanStart = static_cast<int>(directiveStart);
      int spanEnd = static_cast<int>(j);

      if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
        pushEntry(lineIndex, spanStart, spanEnd, directive);
      } else if (directive == "else" || directive == "elif") {
        if (stack.empty()) {
          diags.a.push_back(makeDiagnostic(
              text, lineIndex, spanStart, spanEnd, 2, "nsf",
              "Unmatched preprocessor directive: #" + directive + "."));
        }
      } else if (directive == "endif") {
        if (stack.empty()) {
          diags.a.push_back(makeDiagnostic(text, lineIndex, spanStart, spanEnd,
                                           2, "nsf",
                                           "Unmatched preprocessor directive: "
                                           "#endif."));
        } else {
          stack.pop_back();
        }
      }
      break;
    }

    lineIndex++;
  }

  for (const auto &entry : stack) {
    diags.a.push_back(makeDiagnostic(
        text, entry.line, entry.start, entry.end, 2, "nsf",
        "Unterminated preprocessor conditional: #" + entry.directive + "."));
  }
}

static bool hasDiagnosticErrorOrWarning(const Json &diagnostics) {
  for (const auto &diag : diagnostics.a) {
    const Json *severity = getObjectValue(diag, "severity");
    if (!severity || severity->type != Json::Type::Number)
      continue;
    const int value = static_cast<int>(getNumberValue(*severity));
    if (value == 1 || value == 2)
      return true;
  }
  return false;
}

static bool isIndeterminateDiagnostic(const Json &diag) {
  const Json *code = getObjectValue(diag, "code");
  if (!code || code->type != Json::Type::String)
    return false;
  const std::string value = getStringValue(*code);
  return value.rfind("NSF_INDET_", 0) == 0;
}

static void
fillIndeterminateMetricsFromDiagnostics(const Json &diagnostics,
                                        DiagnosticsBuildResult &result) {
  uint64_t total = 0;
  uint64_t rhsTypeEmpty = 0;
  uint64_t budgetTimeout = 0;
  uint64_t heavyRulesSkipped = 0;
  for (const auto &diag : diagnostics.a) {
    if (!isIndeterminateDiagnostic(diag))
      continue;
    total++;
    const Json *data = getObjectValue(diag, "data");
    if (!data || data->type != Json::Type::Object)
      continue;
    const Json *reasonCode = getObjectValue(*data, "reasonCode");
    if (!reasonCode || reasonCode->type != Json::Type::String)
      continue;
    const std::string reason = getStringValue(*reasonCode);
    if (reason == IndeterminateReason::DiagnosticsRhsTypeEmpty)
      rhsTypeEmpty++;
    else if (reason == IndeterminateReason::DiagnosticsBudgetTimeout)
      budgetTimeout++;
    else if (reason == IndeterminateReason::DiagnosticsHeavyRulesSkipped)
      heavyRulesSkipped++;
  }
  result.indeterminateTotal = total;
  result.indeterminateReasonRhsTypeEmpty = rhsTypeEmpty;
  result.indeterminateReasonBudgetTimeout = budgetTimeout;
  result.indeterminateReasonHeavyRulesSkipped = heavyRulesSkipped;
}

DiagnosticsBuildResult
buildDiagnosticsWithOptions(const std::string &uri, const std::string &text,
                            const std::vector<std::string> &workspaceFolders,
                            const std::vector<std::string> &includePaths,
                            const std::vector<std::string> &shaderExtensions,
                            const std::unordered_map<std::string, int> &defines,
                            const DiagnosticsBuildOptions &options) {
  DiagnosticsBuildResult result;
  result.diagnostics = makeArray();
  const auto startTime = std::chrono::steady_clock::now();
  const size_t maxDiagnostics =
      static_cast<size_t>(std::max(20, options.maxItems));
  const auto timeBudget =
      std::chrono::milliseconds(std::max(30, options.timeBudgetMs));
  auto budgetExpired = [&]() {
    return std::chrono::steady_clock::now() - startTime > timeBudget;
  };
  size_t indeterminateCount = 0;
  const size_t indeterminateMaxItems =
      static_cast<size_t>(std::max(0, options.indeterminateMaxItems));
  if (options.semanticCacheEnabled) {
    SemanticCacheKey key;
    key.includePaths = includePaths;
    key.shaderExtensions = shaderExtensions;
    key.workspaceFolders = workspaceFolders;
    key.definesFingerprint = makeDefinesFingerprint(defines);
    key.unitPath = uriToPath(uri);
    auto snapshot = semanticCacheGetSnapshot(key, uri, options.documentEpoch);
    if (!snapshot) {
      SemanticSnapshot created;
      created.uri = uri;
      created.documentEpoch = options.documentEpoch;
      created.includeGraphUrisOrdered.push_back(uri);
      semanticCacheUpsertSnapshot(key, created);
    }
  }

  collectBracketDiagnostics(text, result.diagnostics);
  if (result.diagnostics.a.size() >= maxDiagnostics) {
    result.truncated = true;
  }
  if (!result.truncated) {
    collectPreprocessorDiagnostics(text, result.diagnostics);
    if (result.diagnostics.a.size() >= maxDiagnostics) {
      result.truncated = true;
    }
  }
  if (!result.truncated && options.enableExpensiveRules && !budgetExpired()) {
    collectReturnAndTypeDiagnostics(
        uri, text, workspaceFolders, includePaths, shaderExtensions, defines,
        result.diagnostics, options.timeBudgetMs, maxDiagnostics,
        result.timedOut, options.indeterminateEnabled,
        options.indeterminateSeverity, indeterminateMaxItems,
        indeterminateCount);
    if (result.diagnostics.a.size() >= maxDiagnostics) {
      result.truncated = true;
    }
  } else if (!options.enableExpensiveRules) {
    result.heavyRulesSkipped = true;
  } else if (budgetExpired()) {
    result.timedOut = true;
    result.heavyRulesSkipped = true;
  }

  std::istringstream stream(text);
  std::string lineText;
  int lineIndex = 0;
  bool inBlockComment = false;
  std::unordered_map<std::string, bool> includeCandidateExistsCache;
  const auto lineActive = computeActiveLineMask(text, defines);
  while (std::getline(stream, lineText)) {
    if (result.diagnostics.a.size() >= maxDiagnostics) {
      result.truncated = true;
      break;
    }
    if (budgetExpired()) {
      result.timedOut = true;
      result.truncated = true;
      break;
    }
    size_t includePos =
        findIncludeDirectiveOutsideComments(lineText, inBlockComment);
    if (includePos == std::string::npos) {
      lineIndex++;
      continue;
    }
    if (lineIndex < static_cast<int>(lineActive.size()) &&
        !lineActive[lineIndex]) {
      lineIndex++;
      continue;
    }

    std::string includePath;
    int spanStart = -1;
    int spanEnd = -1;
    if (!findIncludePathSpan(lineText, includePos, spanStart, spanEnd)) {
      result.diagnostics.a.push_back(makeDiagnostic(
          text, lineIndex, static_cast<int>(includePos),
          static_cast<int>(includePos + std::string("#include").size()), 2,
          "nsf", "Invalid #include syntax."));
      lineIndex++;
      continue;
    }
    includePath = lineText.substr(static_cast<size_t>(spanStart),
                                  static_cast<size_t>(spanEnd - spanStart));

    auto candidates = resolveIncludeCandidates(
        uri, includePath, workspaceFolders, includePaths, shaderExtensions);
    bool found = false;
    for (const auto &candidate : candidates) {
      auto cacheIt = includeCandidateExistsCache.find(candidate);
      bool exists = false;
      if (cacheIt == includeCandidateExistsCache.end()) {
        struct _stat statBuffer;
        exists = (_stat(candidate.c_str(), &statBuffer) == 0);
        includeCandidateExistsCache.emplace(candidate, exists);
      } else {
        exists = cacheIt->second;
      }
      if (exists) {
        found = true;
        break;
      }
    }
    if (!found) {
      result.diagnostics.a.push_back(
          makeDiagnostic(text, lineIndex, spanStart, spanEnd, 2, "nsf",
                         "Cannot resolve include: " + includePath));
    }
    lineIndex++;
  }

  int blockLine = -1;
  int blockChar = -1;
  if (result.diagnostics.a.size() < maxDiagnostics &&
      hasUnterminatedBlockComment(text, blockLine, blockChar)) {
    result.diagnostics.a.push_back(
        makeDiagnostic(text, blockLine, blockChar, blockChar + 2, 1, "nsf",
                       "Unterminated block comment."));
  } else if (result.diagnostics.a.size() >= maxDiagnostics) {
    result.truncated = true;
  }
  if (result.diagnostics.a.size() > maxDiagnostics) {
    result.diagnostics.a.resize(maxDiagnostics);
    result.truncated = true;
  }
  if (options.indeterminateEnabled &&
      result.diagnostics.a.size() < maxDiagnostics) {
    if (result.timedOut && indeterminateCount < indeterminateMaxItems) {
      result.diagnostics.a.push_back(makeDiagnosticWithCodeAndReason(
          text, 0, 0, 0, options.indeterminateSeverity, "nsf",
          "Indeterminate diagnostics: time budget exhausted.",
          "NSF_INDET_BUDGET_TIMEOUT",
          IndeterminateReason::DiagnosticsBudgetTimeout));
      indeterminateCount++;
    }
    if (result.heavyRulesSkipped &&
        indeterminateCount < indeterminateMaxItems) {
      result.diagnostics.a.push_back(makeDiagnosticWithCodeAndReason(
          text, 0, 0, 0, options.indeterminateSeverity, "nsf",
          "Indeterminate diagnostics: heavy rules skipped.",
          "NSF_INDET_HEAVY_RULES_SKIPPED",
          IndeterminateReason::DiagnosticsHeavyRulesSkipped));
      indeterminateCount++;
    }
  }
  if (options.indeterminateEnabled && options.indeterminateSuppressWhenErrors &&
      hasDiagnosticErrorOrWarning(result.diagnostics)) {
    std::vector<Json> filtered;
    filtered.reserve(result.diagnostics.a.size());
    for (const auto &diag : result.diagnostics.a) {
      if (isIndeterminateDiagnostic(diag))
        continue;
      filtered.push_back(diag);
    }
    result.diagnostics.a = std::move(filtered);
  }
  fillIndeterminateMetricsFromDiagnostics(result.diagnostics, result);
  result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - startTime)
                         .count();
  return result;
}

Json buildDiagnostics(const std::string &uri, const std::string &text,
                      const std::vector<std::string> &workspaceFolders,
                      const std::vector<std::string> &includePaths,
                      const std::vector<std::string> &shaderExtensions,
                      const std::unordered_map<std::string, int> &defines) {
  DiagnosticsBuildOptions options;
  return buildDiagnosticsWithOptions(uri, text, workspaceFolders, includePaths,
                                     shaderExtensions, defines, options)
      .diagnostics;
}

SemanticCacheMetricsSnapshot takeSemanticCacheMetricsSnapshot() {
  const SemanticCacheManagerStats stats = semanticCacheConsumeStats();
  SemanticCacheMetricsSnapshot snapshot;
  snapshot.snapshotHit = stats.snapshotHit;
  snapshot.snapshotMiss = stats.snapshotMiss;
  return snapshot;
}
