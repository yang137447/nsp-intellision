#include "diagnostics_preprocessor.hpp"

#include "diagnostics_io.hpp"
#include "preprocessor_view.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

PreprocessorView buildDiagnosticsPreprocessorView(
    const std::string &uri, const std::string &text,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const DiagnosticsBuildOptions &options) {
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
                   [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                   });
    std::transform(rhsPath.begin(), rhsPath.end(), rhsPath.begin(),
                   [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                   });
    return lhsPath == rhsPath;
  };
  PreprocessorIncludeContext includeContext;
  includeContext.currentUri = uri;
  includeContext.workspaceFolders = workspaceFolders;
  includeContext.includePaths = includePaths;
  includeContext.shaderExtensions = shaderExtensions;
  includeContext.loadText =
      [uri, text, activeUnitUri = options.activeUnitUri, sameDocumentUri,
       activeUnitText = options.activeUnitText](
          const std::string &includeUri, std::string &textOut) -> bool {
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
    return !path.empty() && diagnosticsReadFileToString(path, textOut);
  };

  if (!options.activeUnitUri.empty() && options.activeUnitUri != uri) {
    std::string activeUnitText = options.activeUnitText;
    if (activeUnitText.empty()) {
      const std::string activeUnitPath = uriToPath(options.activeUnitUri);
      if (!activeUnitPath.empty())
        diagnosticsReadFileToString(activeUnitPath, activeUnitText);
    }
    if (!activeUnitText.empty()) {
      PreprocessorIncludeContext rootContext = includeContext;
      rootContext.currentUri = options.activeUnitUri;
      PreprocessorView includedView;
      if (buildIncludedDocumentPreprocessorView(activeUnitText, defines,
                                                rootContext, uri,
                                                includedView)) {
        return includedView;
      }
    }
  }

  return buildPreprocessorView(text, defines, includeContext);
}

bool findIncludePathSpan(const std::string &lineText, size_t includePos,
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

size_t findIncludeDirectiveOutsideComments(const std::string &lineText,
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

    if (ch == '/' && next == '/') {
      inLineComment = true;
      character += 2;
      i++;
      continue;
    }

    if (ch == '/' && next == '*') {
      inBlockComment = true;
      lastBlockLine = line;
      lastBlockChar = character;
      character += 2;
      i++;
      continue;
    }

    if (ch == '"') {
      inString = true;
      character++;
      continue;
    }

    character++;
  }

  if (inBlockComment) {
    lineOut = lastBlockLine;
    charOut = lastBlockChar;
    return true;
  }
  lineOut = -1;
  charOut = 0;
  return false;
}
