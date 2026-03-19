#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct AstFunctionSignatureEntry {
  std::string name;
  int line = -1;
  int character = -1;
  std::string label;
  std::vector<std::string> parameters;
};

void indexFunctionSignatures(
    const std::string &text, std::vector<AstFunctionSignatureEntry> &outEntries,
    std::unordered_map<std::string, std::vector<size_t>> &outByName);
