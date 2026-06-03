#include "diagnostics_preprocessor.hpp"

#include "diagnostics_io.hpp"
#include "preprocessor_view.hpp"
#include "uri_utils.hpp"
#include "workspace_summary_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

std::string lowercaseCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  });
  return value;
}

bool uriHasExtension(const std::string &uri, const std::string &extension) {
  const std::string path = lowercaseCopy(uriToPath(uri));
  const std::string ext = lowercaseCopy(extension);
  return path.size() >= ext.size() &&
         path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

} // namespace

DiagnosticsPreprocessorBuildResult buildDiagnosticsPreprocessorContext(
    const std::string &uri, const std::string &text,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const DiagnosticsBuildOptions &options) {
  DiagnosticsPreprocessorBuildResult result;
  PreprocessorIncludeContext includeContext;
  includeContext.currentUri = uri;
  includeContext.workspaceFolders = workspaceFolders;
  includeContext.includePaths = includePaths;
  includeContext.shaderExtensions = shaderExtensions;
  includeContext.compilerPrivateConstantCacheScope =
      options.compilerPrivateConstantCacheScope;
  workspaceSummaryRuntimeCollectArtDefaultZeroMacros(
      includeContext.artDefaultZeroMacros, 4096);
  includeContext.loadText =
      [uri, text, activeUnitUri = options.activeUnitUri,
       activeUnitText = options.activeUnitText](
          const std::string &includeUri, std::string &textOut) -> bool {
    if (!uri.empty() && uriEquivalent(includeUri, uri)) {
      textOut = text;
      return true;
    }
    if (!activeUnitUri.empty() && uriEquivalent(includeUri, activeUnitUri) &&
        !activeUnitText.empty()) {
      textOut = activeUnitText;
      return true;
    }
    const std::string path = uriToPath(includeUri);
    return !path.empty() && diagnosticsReadFileToString(path, textOut);
  };

  const bool targetIsUnit = uriHasExtension(uri, ".nsf") ||
                            (!options.activeUnitUri.empty() &&
                             uriEquivalent(options.activeUnitUri, uri));
  if (!targetIsUnit && options.activeUnitUri.empty()) {
    result.prerequisites.activeUnitReady = false;
    result.prerequisites.includeClosureReady = false;
    result.prerequisites.preprocessorContextReliable = false;
  }

  if (!options.activeUnitUri.empty() && !uriEquivalent(options.activeUnitUri, uri)) {
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
        result.view = std::move(includedView);
        return result;
      }
      if (!targetIsUnit) {
        result.prerequisites.includeClosureReady = false;
        result.prerequisites.preprocessorContextReliable = false;
      }
    } else if (!targetIsUnit) {
      result.prerequisites.activeUnitReady = false;
      result.prerequisites.includeClosureReady = false;
      result.prerequisites.preprocessorContextReliable = false;
    }
  }

  result.view = buildPreprocessorView(text, defines, includeContext);
  return result;
}

PreprocessorView buildDiagnosticsPreprocessorView(
    const std::string &uri, const std::string &text,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const DiagnosticsBuildOptions &options) {
  return buildDiagnosticsPreprocessorContext(
             uri, text, workspaceFolders, includePaths, shaderExtensions,
             defines, options)
      .view;
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
