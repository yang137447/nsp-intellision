#include "expanded_source.hpp"

#include "macro_statement_locals.hpp"

ExpandedSource
buildLinePreservingExpandedSource(const std::string &text,
                                  const PreprocessorView &preprocessorView) {
  ExpandedSource result;
  result.text = text;
  if (text.empty())
    return result;

  size_t lineStart = 0;
  int lineIndex = 0;
  while (lineStart < result.text.size()) {
    result.sourceMap.outputLineToSourceLine.push_back(lineIndex);

    size_t lineEnd = result.text.find('\n', lineStart);
    if (lineEnd == std::string::npos)
      lineEnd = result.text.size();

    if (lineIndex < static_cast<int>(preprocessorView.lineActive.size()) &&
        !preprocessorView.lineActive[lineIndex]) {
      for (size_t i = lineStart; i < lineEnd; i++) {
        if (result.text[i] != '\r')
          result.text[i] = ' ';
      }
    } else {
      const std::string lineText =
          result.text.substr(lineStart, lineEnd - lineStart);
      const auto macroLocals = collectStatementLikeMacroLocalDeclarations(
          preprocessorView, lineIndex, lineText, lineStart);
      for (const auto &macroLocal : macroLocals) {
        ExpandedSourceMacroLocalDeclaration item;
        item.name = macroLocal.name;
        item.type = macroLocal.type;
        item.macroName = macroLocal.macroName;
        item.invocationLine = macroLocal.invocationLine;
        item.invocationStart = macroLocal.invocationStart;
        item.invocationEnd = macroLocal.invocationEnd;
        item.invocationOffset = macroLocal.invocationOffset;
        item.sourceUri = macroLocal.sourceUri;
        item.sourceLine = macroLocal.sourceLine;
        item.sourceStart = macroLocal.sourceStart;
        item.sourceEnd = macroLocal.sourceEnd;
        result.macroLocalDeclarations.push_back(std::move(item));
      }
    }

    if (lineEnd == result.text.size())
      break;
    lineStart = lineEnd + 1;
    lineIndex++;
  }

  return result;
}

ExpandedSource
buildLinePreservingExpandedSource(
    const std::string &text,
    const std::unordered_map<std::string, int> &defines) {
  return buildLinePreservingExpandedSource(text,
                                           buildPreprocessorView(text, defines));
}
