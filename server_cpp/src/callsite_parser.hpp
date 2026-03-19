#pragma once

#include "nsf_lexer.hpp"

#include <string>
#include <utility>
#include <vector>

struct CallSiteArgument {
  std::string functionName;
  int argumentIndex = 0;
  size_t argumentStartOffset = 0;
  bool isMemberCall = false;
};

enum class CallSiteKind {
  FunctionCall,
  ConstructorCast
};

bool parseCallLikeAtOffset(const std::string &text, size_t cursorOffset,
                           std::string &calleeOut, int &activeParameterOut,
                           CallSiteKind &kindOut,
                           size_t *openParenOffsetOut = nullptr);

bool parseCallSiteAtOffset(const std::string &text, size_t cursorOffset,
                           std::string &functionNameOut,
                           int &activeParameterOut,
                           size_t *openParenOffsetOut = nullptr);

bool detectCallLikeCalleeAtOffset(const std::string &text, size_t cursorOffset,
                                  std::string &calleeOut, CallSiteKind &kindOut);

bool isLikelyTypeConstructorCallName(const std::string &name);

bool extractMemberAccessAtOffset(const std::string &text, size_t cursorOffset,
                                 std::string &baseOut,
                                 std::string &memberOut);

bool parseMemberCallAtOffset(const std::string &text, size_t cursorOffset,
                             std::string &baseOut, std::string &memberOut,
                             int &activeParameterOut);

void collectCallArgumentsInRange(const std::string &text, size_t rangeStartOffset,
                                 size_t rangeEndOffset,
                                 std::vector<CallSiteArgument> &out);

bool collectCallArgumentTokenRanges(
    const std::vector<LexToken> &tokens, size_t openParenIndex,
    std::vector<std::pair<size_t, size_t>> &rangesOut, size_t &closeParenIndexOut);
