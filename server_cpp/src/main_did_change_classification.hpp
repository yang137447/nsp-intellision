#pragma once

#include "document_runtime.hpp"

#include <string>
#include <vector>

// Entry-layer didChange helper.
// Input: current document text plus changed ranges from the request payload.
// Output: whether the edit is comment-only and can skip heavier semantic work.
bool isCommentOnlyEditForDidChange(
    const std::string &text, const std::vector<ChangedRange> &changedRanges);
