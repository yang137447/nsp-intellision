#include "diagnostics_syntax.hpp"

#include "diagnostics_emit.hpp"
#include "diagnostics_preprocessor.hpp"
#include "nsf_lexer.hpp"

#include <sstream>
#include <string>
#include <vector>

void collectBracketDiagnostics(const std::string &text, Json &diags) {
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

void collectPreprocessorDiagnostics(const std::string &text, Json &diags) {
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
