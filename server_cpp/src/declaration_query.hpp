#pragma once

#include "preprocessor_view.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>

struct DeclCandidate {
  bool found = false;
  int line = -1;
  int braceDepth = -1;
  size_t nameBytePos = 0;
  std::string lineText;
};

bool findBestDeclarationUpTo(const std::string &text, const std::string &word,
                             size_t maxOffset, DeclCandidate &out);

// Shared declaration lookup that reuses already-computed active-line state.
// Callers should prefer this when document_runtime.* already owns the current
// active branch for the same document.
bool findBestDeclarationUpTo(const std::string &text, const std::string &word,
                             size_t maxOffset,
                             const std::vector<char> &lineActive,
                             DeclCandidate &out);

// Current-document declaration lookup that respects the active preprocessor
// branch of `text`. This is the shared current-doc declaration query for
// request paths that must not drift between hover and definition.
bool findBestCurrentDocDeclarationUpTo(
    const std::string &text, const std::string &word, size_t maxOffset,
    const std::unordered_map<std::string, int> &defines, DeclCandidate &out);

// Include-aware current-document declaration lookup for active-unit flows where
// the current branch state depends on include-chain macros, not just bare
// document-local defines.
bool findBestCurrentDocDeclarationUpTo(
    const std::string &text, const std::string &word, size_t maxOffset,
    const std::unordered_map<std::string, int> &defines,
    const PreprocessorIncludeContext &includeContext, DeclCandidate &out);
