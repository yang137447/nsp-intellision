#include "immediate_syntax_diagnostics.hpp"

#include "lsp_helpers.hpp"
#include "nsf_lexer.hpp"
#include "preprocessor_view.hpp"
#include "server_documents.hpp"
#include "server_parse.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace {

Json makeSyntaxDiagnostic(const std::string &text, int line, int startByte,
                          int endByte, int severity,
                          const std::string &message) {
  std::string lineText = getLineAt(text, line);
  int start = byteOffsetInLineToUtf16(lineText, startByte);
  int end = byteOffsetInLineToUtf16(lineText, endByte);
  if (end < start)
    end = start;
  Json diag = makeObject();
  diag.o["range"] = makeRangeExact(line, start, end);
  diag.o["severity"] = makeNumber(severity);
  diag.o["source"] = makeString("nsf");
  diag.o["message"] = makeString(message);
  return diag;
}

PreprocessorView buildDiagnosticsPreprocessorView(
    const std::string &uri, const std::string &text,
    const ImmediateSyntaxDiagnosticsOptions &options) {
  auto sameDocumentUri = [](const std::string &lhs,
                            const std::string &rhs) -> bool {
    if (lhs == rhs)
      return true;
    if (lhs.empty() || rhs.empty())
      return false;
    std::string lhsPath = uriToPath(lhs);
    std::string rhsPath = uriToPath(rhs);
    if (lhsPath.empty() || rhsPath.empty())
      return false;
    std::replace(lhsPath.begin(), lhsPath.end(), '/', '\\');
    std::replace(rhsPath.begin(), rhsPath.end(), '/', '\\');
    std::transform(lhsPath.begin(), lhsPath.end(), lhsPath.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(rhsPath.begin(), rhsPath.end(), rhsPath.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lhsPath == rhsPath;
  };
  PreprocessorIncludeContext includeContext;
  includeContext.currentUri = uri;
  includeContext.workspaceFolders = options.workspaceFolders;
  includeContext.includePaths = options.includePaths;
  includeContext.shaderExtensions = options.shaderExtensions;
  includeContext.loadText =
      [uri, text, activeUnitUri = options.activeUnitUri,
       sameDocumentUri,
       activeUnitText = options.activeUnitText](const std::string &includeUri,
                                                std::string &textOut) -> bool {
        if (!uri.empty() && sameDocumentUri(includeUri, uri)) {
          textOut = text;
          return true;
        }
        if (!activeUnitUri.empty() && sameDocumentUri(includeUri, activeUnitUri) &&
            !activeUnitText.empty()) {
          textOut = activeUnitText;
          return true;
        }
        const std::string path = uriToPath(includeUri);
        return !path.empty() && readFileText(path, textOut);
      };

  if (!options.activeUnitUri.empty() && options.activeUnitUri != uri) {
    std::string activeUnitText = options.activeUnitText;
    if (activeUnitText.empty()) {
      const std::string activeUnitPath = uriToPath(options.activeUnitUri);
      if (!activeUnitPath.empty())
        readFileText(activeUnitPath, activeUnitText);
    }
    if (!activeUnitText.empty()) {
      PreprocessorIncludeContext rootContext = includeContext;
      rootContext.currentUri = options.activeUnitUri;
      PreprocessorView includedView;
      if (buildIncludedDocumentPreprocessorView(activeUnitText, options.defines,
                                                rootContext, uri,
                                                includedView)) {
        return includedView;
      }
    }
  }

  return buildPreprocessorView(text, options.defines, includeContext);
}

bool isWhitespace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

std::pair<int, int> computeChangedWindow(const std::vector<ChangedRange> &ranges,
                                         int lineCount, int padding) {
  if (lineCount <= 0)
    return {0, 0};
  if (ranges.empty())
    return {0, lineCount - 1};
  int startLine = lineCount - 1;
  int endLine = 0;
  for (const auto &range : ranges) {
    startLine = std::min(startLine, std::max(0, range.startLine - padding));
    endLine = std::max(
        endLine, std::min(lineCount - 1, std::max(range.endLine,
                                                  range.newEndLine) + padding));
  }
  if (endLine < startLine)
    endLine = startLine;
  return {startLine, endLine};
}

void collectBracketDiagnostics(const std::string &text, Json &diags,
                               size_t maxItems) {
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
  auto push = [&](char bracket) { stack.push_back(Entry{bracket, line, character}); };
  auto pushDiag = [&](const Json &diag) {
    if (diags.a.size() >= maxItems)
      return;
    diags.a.push_back(diag);
  };
  for (size_t i = 0; i < text.size(); i++) {
    char ch = text[i];
    char next = i + 1 < text.size() ? text[i + 1] : '\0';
    if (ch == '\n') {
      line++;
      character = 0;
      inLineComment = false;
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
    if (inString) {
      if (ch == '"' && (i == 0 || text[i - 1] != '\\'))
        inString = false;
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
    if (ch == '"') {
      inString = true;
      character++;
      continue;
    }
    if (ch == '(' || ch == '[' || ch == '{') {
      push(ch);
    } else if (ch == ')' || ch == ']' || ch == '}') {
      const char expected = ch == ')' ? '(' : (ch == ']' ? '[' : '{');
      if (stack.empty() || stack.back().bracket != expected) {
        pushDiag(makeSyntaxDiagnostic(text, line, character, character + 1, 1,
                                      std::string("Unmatched closing bracket: ") +
                                          ch));
      } else {
        stack.pop_back();
      }
    }
    character++;
  }
  for (const auto &entry : stack) {
    if (diags.a.size() >= maxItems)
      break;
    diags.a.push_back(makeSyntaxDiagnostic(
        text, entry.line, entry.character, entry.character + 1, 1,
        std::string("Unterminated bracket: ") + entry.bracket));
  }
}

void collectPreprocessorDiagnostics(const std::string &text, Json &diags,
                                    size_t maxItems) {
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
  while (std::getline(stream, lineText)) {
    for (size_t i = 0; i < lineText.size(); i++) {
      char ch = lineText[i];
      char next = i + 1 < lineText.size() ? lineText[i + 1] : '\0';
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
      if (isWhitespace(ch))
        continue;
      if (ch != '#')
        break;
      size_t wordStart = i + 1;
      while (wordStart < lineText.size() && isWhitespace(lineText[wordStart]))
        wordStart++;
      size_t wordEnd = wordStart;
      while (wordEnd < lineText.size() && isIdentifierChar(lineText[wordEnd]))
        wordEnd++;
      if (wordEnd == wordStart)
        break;
      const std::string directive = lineText.substr(wordStart, wordEnd - wordStart);
      const int spanStart = static_cast<int>(i);
      const int spanEnd = static_cast<int>(wordEnd);
      if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
        stack.push_back(ConditionalEntry{lineIndex, spanStart, spanEnd, directive});
      } else if (directive == "else" || directive == "elif") {
        if (stack.empty() && diags.a.size() < maxItems) {
          diags.a.push_back(makeSyntaxDiagnostic(
              text, lineIndex, spanStart, spanEnd, 2,
              "Unmatched preprocessor directive: #" + directive + "."));
        }
      } else if (directive == "endif") {
        if (stack.empty()) {
          if (diags.a.size() < maxItems) {
            diags.a.push_back(makeSyntaxDiagnostic(
                text, lineIndex, spanStart, spanEnd, 2,
                "Unmatched preprocessor directive: #endif."));
          }
        } else {
          stack.pop_back();
        }
      }
      break;
    }
    lineIndex++;
  }
  for (const auto &entry : stack) {
    if (diags.a.size() >= maxItems)
      break;
    diags.a.push_back(makeSyntaxDiagnostic(
        text, entry.line, entry.start, entry.end, 2,
        "Unterminated preprocessor conditional: #" + entry.directive + "."));
  }
}

bool hasUnterminatedBlockComment(const std::string &text, int &lineOut,
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
    char next = i + 1 < text.size() ? text[i + 1] : '\0';
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
  if (!inBlockComment)
    return false;
  lineOut = lastBlockLine;
  charOut = lastBlockChar;
  return true;
}

void collectMissingSemicolonDiagnostics(
    const std::string &uri, const std::string &text,
    const std::vector<std::string> &lines, int startLine, int endLine,
    const ImmediateSyntaxDiagnosticsOptions &options, Json &diags,
    size_t maxItems) {
  const PreprocessorView preprocessorView =
      buildDiagnosticsPreprocessorView(uri, text, options);

  const TrimmedCodeLineScanSharedResult lineScan =
      buildTrimmedCodeLineScanShared(text, &preprocessorView.lineActive);
  const std::vector<std::string> &trimmedLines = lineScan.trimmedLines;

  for (int lineIndex = startLine; lineIndex <= endLine; lineIndex++) {
    if (lineIndex < 0 || lineIndex >= static_cast<int>(lines.size()))
      continue;
    if (diags.a.size() >= maxItems)
      return;
    if (lineIndex < static_cast<int>(preprocessorView.lineActive.size()) &&
        !preprocessorView.lineActive[lineIndex]) {
      continue;
    }

    std::string nextTrimmed;
    for (int nextLine = lineIndex + 1; nextLine < static_cast<int>(trimmedLines.size());
         nextLine++) {
      if (nextLine < static_cast<int>(preprocessorView.lineActive.size()) &&
          !preprocessorView.lineActive[nextLine]) {
        continue;
      }
      if (!trimmedLines[nextLine].empty()) {
        nextTrimmed = trimmedLines[nextLine];
        break;
      }
    }

    const std::string &trimmed = trimmedLines[lineIndex];
    const bool insideOpenGroupingAfterLine =
        lineIndex < static_cast<int>(lineScan.parenDepthAfterLine.size()) &&
            lineScan.parenDepthAfterLine[lineIndex] > 0 ||
        lineIndex < static_cast<int>(lineScan.bracketDepthAfterLine.size()) &&
            lineScan.bracketDepthAfterLine[lineIndex] > 0;
    if (!shouldReportMissingSemicolonShared(trimmed, nextTrimmed,
                                            insideOpenGroupingAfterLine))
      continue;

    size_t endByte = lines[lineIndex].find_last_not_of(" \t");
    if (endByte == std::string::npos)
      endByte = trimmed.empty() ? 0 : trimmed.size() - 1;
    diags.a.push_back(makeSyntaxDiagnostic(text, lineIndex,
                                           static_cast<int>(endByte),
                                           static_cast<int>(endByte + 1), 1,
                                           "Missing semicolon."));
  }
}

} // namespace

ImmediateSyntaxDiagnosticsResult buildImmediateSyntaxDiagnostics(
    const std::string &uri, const std::string &text,
    const std::vector<ChangedRange> &changedRanges,
    const ImmediateSyntaxDiagnosticsOptions &options) {
  ImmediateSyntaxDiagnosticsResult result;
  result.diagnostics = makeArray();
  const auto startedAt = std::chrono::steady_clock::now();
  const std::vector<std::string> lines = splitLinesShared(text);
  const int lineCount = static_cast<int>(lines.size());
  const auto [windowStart, windowEnd] = computeChangedWindow(
      changedRanges, std::max(1, lineCount), options.changedWindowPaddingLines);
  result.changedWindowStartLine = windowStart;
  result.changedWindowEndLine = windowEnd;
  result.changedWindowOnly = !changedRanges.empty();

  const size_t maxItems = static_cast<size_t>(std::max(20, options.maxItems));
  collectBracketDiagnostics(text, result.diagnostics, maxItems);
  collectPreprocessorDiagnostics(text, result.diagnostics, maxItems);
  int blockLine = -1;
  int blockChar = -1;
  if (result.diagnostics.a.size() < maxItems &&
      hasUnterminatedBlockComment(text, blockLine, blockChar)) {
    result.diagnostics.a.push_back(makeSyntaxDiagnostic(
        text, blockLine, blockChar, blockChar + 2, 1,
        "Unterminated block comment."));
  }
  if (result.diagnostics.a.size() < maxItems) {
    collectMissingSemicolonDiagnostics(uri, text, lines, windowStart, windowEnd,
                                       options, result.diagnostics, maxItems);
  }
  if (result.diagnostics.a.size() > maxItems) {
    result.diagnostics.a.resize(maxItems);
    result.truncated = true;
  }
  result.elapsedMs =
      static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - startedAt)
                              .count());
  return result;
}
