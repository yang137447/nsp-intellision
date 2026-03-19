#include "visibility_evaluator.hpp"

#include <cctype>

namespace {
struct Parser {
  const std::string &input;
  const std::unordered_map<std::string, int> &defines;
  size_t pos = 0;
  bool malformed = false;

  void skipSpaces() {
    while (pos < input.size() &&
           std::isspace(static_cast<unsigned char>(input[pos]))) {
      pos++;
    }
  }

  bool consumeChar(char ch) {
    skipSpaces();
    if (pos < input.size() && input[pos] == ch) {
      pos++;
      return true;
    }
    return false;
  }

  bool consumeToken(const char *token) {
    skipSpaces();
    size_t i = 0;
    while (token[i] != '\0') {
      if (pos + i >= input.size() || input[pos + i] != token[i])
        return false;
      i++;
    }
    pos += i;
    return true;
  }

  std::string parseIdentifier() {
    skipSpaces();
    if (pos >= input.size())
      return "";
    if (!(std::isalpha(static_cast<unsigned char>(input[pos])) ||
          input[pos] == '_')) {
      return "";
    }
    size_t start = pos;
    pos++;
    while (pos < input.size() &&
           (std::isalnum(static_cast<unsigned char>(input[pos])) ||
            input[pos] == '_')) {
      pos++;
    }
    return input.substr(start, pos - start);
  }

  VisibilityEvalResult parsePrimary() {
    skipSpaces();
    if (consumeChar('(')) {
      VisibilityEvalResult nested = parseOrExpr();
      if (!consumeChar(')')) {
        malformed = true;
        return VisibilityEvalResult::Unknown;
      }
      return nested;
    }
    if (consumeToken("defined")) {
      if (!consumeChar('(')) {
        malformed = true;
        return VisibilityEvalResult::Unknown;
      }
      std::string name = parseIdentifier();
      if (name.empty() || !consumeChar(')')) {
        malformed = true;
        return VisibilityEvalResult::Unknown;
      }
      return defines.find(name) != defines.end() ? VisibilityEvalResult::Visible
                                                 : VisibilityEvalResult::Hidden;
    }
    std::string ident = parseIdentifier();
    if (!ident.empty()) {
      auto it = defines.find(ident);
      if (it == defines.end())
        return VisibilityEvalResult::Hidden;
      return it->second == 0 ? VisibilityEvalResult::Hidden
                             : VisibilityEvalResult::Visible;
    }
    malformed = true;
    return VisibilityEvalResult::Unknown;
  }

  VisibilityEvalResult negate(VisibilityEvalResult value) {
    if (value == VisibilityEvalResult::Visible)
      return VisibilityEvalResult::Hidden;
    if (value == VisibilityEvalResult::Hidden)
      return VisibilityEvalResult::Visible;
    return VisibilityEvalResult::Unknown;
  }

  VisibilityEvalResult parseUnary() {
    skipSpaces();
    if (consumeChar('!')) {
      return negate(parseUnary());
    }
    return parsePrimary();
  }

  VisibilityEvalResult andExpr(VisibilityEvalResult lhs,
                               VisibilityEvalResult rhs) {
    if (lhs == VisibilityEvalResult::Hidden ||
        rhs == VisibilityEvalResult::Hidden)
      return VisibilityEvalResult::Hidden;
    if (lhs == VisibilityEvalResult::Visible &&
        rhs == VisibilityEvalResult::Visible)
      return VisibilityEvalResult::Visible;
    return VisibilityEvalResult::Unknown;
  }

  VisibilityEvalResult orExpr(VisibilityEvalResult lhs,
                              VisibilityEvalResult rhs) {
    if (lhs == VisibilityEvalResult::Visible ||
        rhs == VisibilityEvalResult::Visible)
      return VisibilityEvalResult::Visible;
    if (lhs == VisibilityEvalResult::Hidden &&
        rhs == VisibilityEvalResult::Hidden)
      return VisibilityEvalResult::Hidden;
    return VisibilityEvalResult::Unknown;
  }

  VisibilityEvalResult parseAndExpr() {
    VisibilityEvalResult value = parseUnary();
    while (true) {
      if (!consumeToken("&&"))
        break;
      value = andExpr(value, parseUnary());
    }
    return value;
  }

  VisibilityEvalResult parseOrExpr() {
    VisibilityEvalResult value = parseAndExpr();
    while (true) {
      if (!consumeToken("||"))
        break;
      value = orExpr(value, parseAndExpr());
    }
    return value;
  }
};
} // namespace

VisibilityEvalResult evaluateVisibilityCondition(
    const std::string &condition,
    const std::unordered_map<std::string, int> &defines) {
  if (condition.empty())
    return VisibilityEvalResult::Visible;
  Parser parser{condition, defines};
  VisibilityEvalResult value = parser.parseOrExpr();
  parser.skipSpaces();
  if (parser.malformed || parser.pos != condition.size())
    return VisibilityEvalResult::Unknown;
  return value;
}

const char *visibilityEvalResultToString(VisibilityEvalResult value) {
  switch (value) {
  case VisibilityEvalResult::Visible:
    return "visible";
  case VisibilityEvalResult::Hidden:
    return "hidden";
  case VisibilityEvalResult::Unknown:
    return "unknown";
  }
  return "unknown";
}
