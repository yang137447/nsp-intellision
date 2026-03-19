#include "macro_generated_functions.hpp"

#include "active_unit.hpp"
#include "full_ast.hpp"
#include "include_resolver.hpp"
#include "nsf_lexer.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#include <sys/stat.h>
#endif

namespace {
static bool readFileToString(const std::string &path, std::string &outText) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in)
    return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  outText = ss.str();
  return true;
}

static std::string trimToken(const std::string &text) {
  size_t start = 0;
  size_t end = text.size();
  while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) {
    start++;
  }
  while (end > start &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    end--;
  }
  return text.substr(start, end - start);
}

static bool equalsIgnoreCase(const std::string &lhs, const std::string &rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
        std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

static bool fileExists(const std::string &path) {
#ifdef _WIN32
  struct _stat statBuffer;
  return _stat(path.c_str(), &statBuffer) == 0;
#else
  return false;
#endif
}

static void appendUnique(std::vector<std::string> &out,
                         const std::string &value) {
  for (const auto &item : out) {
    if (item == value)
      return;
  }
  out.push_back(value);
}
} // namespace

bool collectMacroGeneratedFunctions(
    const std::string &entryUri, const std::string &entryText,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::string &functionNameFilter,
    std::vector<MacroGeneratedFunctionInfo> &outCandidates, size_t limit) {
  outCandidates.clear();
  std::vector<std::string> scanExtensions = shaderExtensions;
  appendUnique(scanExtensions, ".hlsli");
  appendUnique(scanExtensions, ".h");

  std::vector<std::pair<std::string, std::string>> sourceQueue;
  std::unordered_set<std::string> knownUris;
  auto collectIncludeClosure = [&](const std::string &rootUri,
                                   const std::string &rootText) {
    if (rootUri.empty())
      return;
    std::unordered_set<std::string> visitedUris;
    std::vector<std::string> stackUris;
    stackUris.push_back(rootUri);
    visitedUris.insert(rootUri);

    while (!stackUris.empty()) {
      std::string currentUri = stackUris.back();
      stackUris.pop_back();

      std::string currentText;
      bool foundText = false;
      if (currentUri == rootUri) {
        currentText = rootText;
        foundText = true;
      } else {
        std::string currentPath = uriToPath(currentUri);
        if (!currentPath.empty() && readFileToString(currentPath, currentText))
          foundText = true;
      }
      if (!foundText)
        continue;

      if (knownUris.insert(currentUri).second)
        sourceQueue.emplace_back(currentUri, currentText);

      std::vector<std::string> includePathList;
      if (!queryFullAstIncludes(currentUri, currentText, 0, includePathList)) {
        includePathList.clear();
      }
      for (const auto &includePath : includePathList) {
        auto candidates =
            resolveIncludeCandidates(currentUri, includePath, workspaceFolders,
                                     includePaths, scanExtensions);
        for (const auto &candidate : candidates) {
          if (!fileExists(candidate))
            continue;
          std::string nextUri = pathToUri(candidate);
          if (!nextUri.empty() && visitedUris.insert(nextUri).second) {
            stackUris.push_back(nextUri);
          }
          break;
        }
      }
    }
  };

  collectIncludeClosure(entryUri, entryText);

  {
    std::string activeUnitUri = getActiveUnitUri();
    if (activeUnitUri.empty()) {
      const std::string activeUnitPath = getActiveUnitPath();
      if (!activeUnitPath.empty())
        activeUnitUri = pathToUri(activeUnitPath);
    }
    if (!activeUnitUri.empty() && activeUnitUri != entryUri &&
        knownUris.find(activeUnitUri) == knownUris.end()) {
      std::string activeText;
      std::string activePath = uriToPath(activeUnitUri);
      if (!activePath.empty() && readFileToString(activePath, activeText)) {
        collectIncludeClosure(activeUnitUri, activeText);
      }
    }
  }

  {
    std::vector<IndexedDefinition> macroDefs;
    if (workspaceIndexFindDefinitions("GET_LIGHTING_MULTIPLIER_DEF", macroDefs,
                                      64)) {
      for (const auto &macroDef : macroDefs) {
        if (macroDef.uri.empty() || !knownUris.insert(macroDef.uri).second)
          continue;
        const std::string macroPath = uriToPath(macroDef.uri);
        if (macroPath.empty())
          continue;
        std::string macroText;
        if (!readFileToString(macroPath, macroText))
          continue;
        sourceQueue.emplace_back(macroDef.uri, std::move(macroText));
      }
    }
  }

  for (const auto &source : sourceQueue) {
    std::istringstream stream(source.second);
    std::string sourceLine;
    int sourceLineIndex = 0;
    while (std::getline(stream, sourceLine)) {
      const auto lineTokens = lexLineTokens(sourceLine);
      if (lineTokens.empty()) {
        sourceLineIndex++;
        continue;
      }
      size_t macroIndex = std::string::npos;
      for (size_t ti = 0; ti < lineTokens.size(); ti++) {
        if (lineTokens[ti].kind == LexToken::Kind::Identifier &&
            lineTokens[ti].text == "GET_LIGHTING_MULTIPLIER_DEF") {
          macroIndex = ti;
          break;
        }
      }
      if (macroIndex == std::string::npos) {
        sourceLineIndex++;
        continue;
      }
      if (macroIndex + 1 >= lineTokens.size() ||
          lineTokens[macroIndex + 1].kind != LexToken::Kind::Punct ||
          lineTokens[macroIndex + 1].text != "(") {
        sourceLineIndex++;
        continue;
      }

      std::vector<std::string> args;
      std::string currentArg;
      int depth = 0;
      bool started = false;
      for (size_t ti = macroIndex + 1; ti < lineTokens.size(); ti++) {
        const auto &tok = lineTokens[ti];
        if (tok.kind == LexToken::Kind::Punct && tok.text == "(") {
          depth++;
          if (depth == 1) {
            started = true;
            continue;
          }
        } else if (tok.kind == LexToken::Kind::Punct && tok.text == ")") {
          if (depth == 1) {
            args.push_back(trimToken(currentArg));
            currentArg.clear();
            break;
          }
          if (depth > 0)
            depth--;
        }
        if (!started)
          continue;
        if (tok.kind == LexToken::Kind::Punct && tok.text == "," &&
            depth == 1) {
          args.push_back(trimToken(currentArg));
          currentArg.clear();
          continue;
        }
        if (!currentArg.empty())
          currentArg.push_back(' ');
        currentArg += tok.text;
      }
      if (args.size() < 2) {
        sourceLineIndex++;
        continue;
      }

      auto parseNamedArg = [&](const std::string &rawArg,
                               const std::string &key,
                               std::string &valueOut) -> bool {
        const auto tokens = lexLineTokens(rawArg);
        if (tokens.size() < 3)
          return false;
        if (tokens[0].kind != LexToken::Kind::Identifier ||
            !equalsIgnoreCase(tokens[0].text, key)) {
          return false;
        }
        if (tokens[1].kind != LexToken::Kind::Punct || tokens[1].text != ":")
          return false;
        std::string rebuilt;
        for (size_t i = 2; i < tokens.size(); i++) {
          if (!rebuilt.empty())
            rebuilt.push_back(' ');
          rebuilt += tokens[i].text;
        }
        valueOut = trimToken(rebuilt);
        return !valueOut.empty();
      };

      std::string typeArg;
      std::string nameArg;
      for (const auto &arg : args) {
        std::string value;
        if (typeArg.empty() && parseNamedArg(arg, "Type", value)) {
          typeArg = value;
          continue;
        }
        if (nameArg.empty() && parseNamedArg(arg, "Name", value)) {
          nameArg = value;
          continue;
        }
      }
      if (typeArg.empty())
        typeArg = trimToken(args[0]);
      if (nameArg.empty() && args.size() > 1)
        nameArg = trimToken(args[1]);

      const std::string generatedType = trimToken(typeArg);
      std::string generatedSuffix;
      const auto suffixTokens = lexLineTokens(nameArg);
      for (const auto &suffixToken : suffixTokens) {
        if (suffixToken.kind == LexToken::Kind::Identifier) {
          generatedSuffix = suffixToken.text;
          break;
        }
      }
      if (generatedType.empty() || generatedSuffix.empty()) {
        sourceLineIndex++;
        continue;
      }

      const std::string generatedName = "Get" + generatedSuffix + "Multiplier";
      if (!functionNameFilter.empty() && generatedName != functionNameFilter) {
        sourceLineIndex++;
        continue;
      }

      MacroGeneratedFunctionInfo info;
      info.name = generatedName;
      info.returnType = generatedType;
      info.parameterDecls.push_back("PixelData p");
      info.parameterTypes.push_back("PixelData");
      info.label = generatedType + " " + generatedName + "(PixelData p)";
      info.definition.uri = source.first;
      info.definition.line = sourceLineIndex;
      info.definition.start = static_cast<int>(lineTokens[macroIndex].start);
      info.definition.end = static_cast<int>(lineTokens[macroIndex].end);
      outCandidates.push_back(std::move(info));
      if (outCandidates.size() >= limit)
        return true;
    }
  }

  return !outCandidates.empty();
}
