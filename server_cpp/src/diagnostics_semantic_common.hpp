#pragma once

#include "preprocessor_view.hpp"

#include <string>
#include <vector>

// Internal diagnostics helper surface shared by semantic/type submodules.
// Scope: comment/code masking, lightweight preprocessor-line detection, and
// branch-signature overlap checks used by diagnostics-only implementations.
std::string formatTypeList(const std::vector<std::string> &types);

std::vector<char> buildCodeMaskForLine(const std::string &lineText,
                                       bool &inBlockCommentInOut);

bool isPreprocessorDirectiveLine(const std::string &lineText,
                                 const std::vector<char> &mask);

bool preprocBranchSigsOverlap(const PreprocBranchSig &a,
                              const PreprocBranchSig &b);
