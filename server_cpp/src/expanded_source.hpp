#pragma once

#include "preprocessor_view.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct ExpandedSourceMacroLocalDeclaration {
  std::string name;
  std::string type;
  std::string macroName;
  int invocationLine = -1;
  int invocationStart = 0;
  int invocationEnd = 0;
  size_t invocationOffset = 0;
  std::string sourceUri;
  int sourceLine = -1;
  int sourceStart = 0;
  int sourceEnd = 0;
};

struct ExpandedSourceMap {
  std::vector<int> outputLineToSourceLine;
};

struct ExpandedSource {
  std::string text;
  ExpandedSourceMap sourceMap;
  std::vector<ExpandedSourceMacroLocalDeclaration> macroLocalDeclarations;
};

ExpandedSource
buildLinePreservingExpandedSource(const std::string &text,
                                  const PreprocessorView &preprocessorView);

ExpandedSource
buildLinePreservingExpandedSource(
    const std::string &text,
    const std::unordered_map<std::string, int> &defines);

