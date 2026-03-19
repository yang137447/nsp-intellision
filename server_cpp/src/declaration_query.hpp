#pragma once

#include <cstddef>
#include <string>

struct DeclCandidate {
  bool found = false;
  int line = -1;
  int braceDepth = -1;
  size_t nameBytePos = 0;
  std::string lineText;
};

bool findBestDeclarationUpTo(const std::string &text, const std::string &word,
                             size_t maxOffset, DeclCandidate &out);
