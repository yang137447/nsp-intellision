#pragma once

#include "hover_rendering.hpp"

#include <string>
#include <vector>

std::string extractLeadingDocumentationAtLine(const std::string &text,
                                              int lineIndex);

std::string extractTrailingInlineCommentAtLine(const std::string &text,
                                               int lineIndex,
                                               int minCharacter);

// Collects known UI metadata fields from a top-level `<>` metadata block that
// immediately follows the declaration at `declarationLineIndex`.
void collectKnownUiMetadataHoverItems(const std::string &text,
                                      int declarationLineIndex,
                                      std::vector<HoverKeyValueItem> &outItems);
