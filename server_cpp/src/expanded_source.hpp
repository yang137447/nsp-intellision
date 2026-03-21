#pragma once

#include "preprocessor_view.hpp"

#include <string>
#include <unordered_map>
#include <vector>

struct ExpandedSourceMap {
  std::vector<int> outputLineToSourceLine;
};

struct ExpandedSource {
  std::string text;
  ExpandedSourceMap sourceMap;
};

ExpandedSource
buildLinePreservingExpandedSource(const std::string &text,
                                  const PreprocessorView &preprocessorView);

ExpandedSource
buildLinePreservingExpandedSource(
    const std::string &text,
    const std::unordered_map<std::string, int> &defines);

