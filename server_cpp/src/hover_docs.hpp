#pragma once

#include <string>

std::string extractLeadingDocumentationAtLine(const std::string &text,
                                              int lineIndex);

std::string extractTrailingInlineCommentAtLine(const std::string &text,
                                               int lineIndex,
                                               int minCharacter);
