#include "callsite_parser.hpp"

#include "text_utils.hpp"

#include <cctype>
#include <functional>

namespace {

struct InlayCallFrame {
  bool isCallable = false;
  bool isMemberCall = false;
  std::string functionName;
  int argumentIndex = 0;
  bool expectingArgument = true;
  int angleDepth = 0;
  int bracketDepth = 0;
  int braceDepth = 0;
};

static bool isSpace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

static bool isLikelyStringPrefixChar(char ch) {
  unsigned char c = static_cast<unsigned char>(ch);
  return std::isalpha(c) != 0 || ch == '_';
}

static bool isCallLikeKeyword(const std::string &name) {
  return name == "if" || name == "for" || name == "while" ||
         name == "switch" || name == "register" || name == "packoffset";
}

static std::string extractCallNameBefore(const std::string &text,
                                         size_t offset,
                                         bool *isMemberCallOut) {
  if (isMemberCallOut)
    *isMemberCallOut = false;
  if (offset == 0 || offset > text.size())
    return "";
  size_t end = offset;
  while (end > 0 && isSpace(text[end - 1]))
    end--;
  if (end == 0 || !isIdentifierChar(text[end - 1]))
    return "";
  size_t start = end;
  while (start > 0 && isIdentifierChar(text[start - 1]))
    start--;
  size_t left = start;
  while (left > 0 && isSpace(text[left - 1]))
    left--;
  if (left > 0 && text[left - 1] == '.') {
    size_t dotStart = left - 1;
    while (dotStart > 0 && isSpace(text[dotStart - 1]))
      dotStart--;
    if (dotStart == 0)
      return "";
    if (isMemberCallOut)
      *isMemberCallOut = true;
  } else if (left >= 2 && text[left - 1] == '>' && text[left - 2] == '-') {
    size_t arrowStart = left - 2;
    while (arrowStart > 0 && isSpace(text[arrowStart - 1]))
      arrowStart--;
    if (arrowStart == 0)
      return "";
    if (isMemberCallOut)
      *isMemberCallOut = true;
  }
  return text.substr(start, end - start);
}

static std::string toLowerAscii(std::string value) {
  for (char &ch : value)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value;
}

static bool isDigits(const std::string &value) {
  if (value.empty())
    return false;
  for (char ch : value) {
    if (!std::isdigit(static_cast<unsigned char>(ch)))
      return false;
  }
  return true;
}

static bool isLikelyTypeConstructor(const std::string &name) {
  const std::string lower = toLowerAscii(name);
  if (lower == "bool" || lower == "int" || lower == "uint" ||
      lower == "dword" || lower == "half" || lower == "float" ||
      lower == "double" || lower == "min16float" || lower == "min10float" ||
      lower == "min16int" || lower == "min12int" || lower == "min16uint")
    return true;
  const std::vector<std::string> bases = {
      "bool",       "int",        "uint",     "half",     "float",    "double",
      "min16float", "min10float", "min16int", "min12int", "min16uint"};
  for (const auto &base : bases) {
    if (lower.rfind(base, 0) != 0)
      continue;
    const std::string suffix = lower.substr(base.size());
    if (suffix.empty())
      continue;
    if (isDigits(suffix))
      return true;
    const size_t x = suffix.find('x');
    if (x == std::string::npos)
      continue;
    const std::string rows = suffix.substr(0, x);
    const std::string cols = suffix.substr(x + 1);
    if (isDigits(rows) && isDigits(cols))
      return true;
  }
  return false;
}

} // namespace

bool parseCallLikeAtOffset(const std::string &text, size_t cursorOffset,
                           std::string &calleeOut, int &activeParameterOut,
                           CallSiteKind &kindOut, size_t *openParenOffsetOut) {
  calleeOut.clear();
  activeParameterOut = 0;
  kindOut = CallSiteKind::FunctionCall;
  if (cursorOffset > text.size())
    cursorOffset = text.size();
  if (cursorOffset == 0)
    return false;

  int depth = 0;
  size_t openParen = std::string::npos;
  for (size_t i = cursorOffset; i > 0; i--) {
    char ch = text[i - 1];
    if (ch == ')') {
      depth++;
      continue;
    }
    if (ch == '(') {
      if (depth == 0) {
        openParen = i - 1;
        break;
      }
      depth--;
      continue;
    }
  }
  if (openParen == std::string::npos)
    return false;

  size_t nameEnd = openParen;
  while (nameEnd > 0 &&
         std::isspace(static_cast<unsigned char>(text[nameEnd - 1])))
    nameEnd--;
  size_t nameStart = nameEnd;
  while (nameStart > 0 && isIdentifierChar(text[nameStart - 1]))
    nameStart--;
  if (nameStart == nameEnd)
    return false;
  calleeOut = text.substr(nameStart, nameEnd - nameStart);
  if (isLikelyTypeConstructor(calleeOut))
    kindOut = CallSiteKind::ConstructorCast;

  int paren = 0;
  int angle = 0;
  int bracket = 0;
  int active = 0;
  for (size_t i = openParen + 1; i < cursorOffset; i++) {
    char ch = text[i];
    if (ch == '(')
      paren++;
    else if (ch == ')' && paren > 0)
      paren--;
    else if (ch == '<')
      angle++;
    else if (ch == '>' && angle > 0)
      angle--;
    else if (ch == '[')
      bracket++;
    else if (ch == ']' && bracket > 0)
      bracket--;
    if (ch == ',' && paren == 0 && angle == 0 && bracket == 0)
      active++;
  }

  activeParameterOut = active;
  if (openParenOffsetOut)
    *openParenOffsetOut = openParen;
  return !calleeOut.empty();
}

bool parseCallSiteAtOffset(const std::string &text, size_t cursorOffset,
                           std::string &functionNameOut,
                           int &activeParameterOut,
                           size_t *openParenOffsetOut) {
  CallSiteKind kind = CallSiteKind::FunctionCall;
  if (!parseCallLikeAtOffset(text, cursorOffset, functionNameOut,
                             activeParameterOut, kind, openParenOffsetOut)) {
    return false;
  }
  return kind == CallSiteKind::FunctionCall;
}

bool detectCallLikeCalleeAtOffset(const std::string &text, size_t cursorOffset,
                                  std::string &calleeOut,
                                  CallSiteKind &kindOut) {
  calleeOut.clear();
  kindOut = CallSiteKind::FunctionCall;
  if (cursorOffset >= text.size())
    return false;
  if (!isIdentifierChar(text[cursorOffset]))
    return false;
  size_t start = cursorOffset;
  while (start > 0 && isIdentifierChar(text[start - 1]))
    start--;
  size_t end = cursorOffset + 1;
  while (end < text.size() && isIdentifierChar(text[end]))
    end++;
  if (end <= start)
    return false;
  calleeOut = text.substr(start, end - start);
  size_t scan = end;
  while (scan < text.size() &&
         std::isspace(static_cast<unsigned char>(text[scan])))
    scan++;
  if (scan >= text.size() || text[scan] != '(')
    return false;
  if (isLikelyTypeConstructor(calleeOut))
    kindOut = CallSiteKind::ConstructorCast;
  return true;
}

bool isLikelyTypeConstructorCallName(const std::string &name) {
  return isLikelyTypeConstructor(name);
}

static bool extractMemberBaseIdentifierBeforeDot(const std::string &text,
                                                 size_t dotPos,
                                                 std::string &baseOut) {
  baseOut.clear();
  if (dotPos == 0 || dotPos > text.size())
    return false;

  size_t cur = dotPos;
  while (cur > 0 && isSpace(text[cur - 1]))
    cur--;
  if (cur == 0)
    return false;

  std::function<bool(size_t, size_t, std::string &)> parseBackward =
      [&](size_t startBound, size_t endExclusive, std::string &out) -> bool {
    out.clear();
    if (endExclusive <= startBound)
      return false;

    size_t end = endExclusive;
    while (end > startBound && isSpace(text[end - 1]))
      end--;
    if (end <= startBound)
      return false;

    if (isIdentifierChar(text[end - 1])) {
      size_t start = end;
      while (start > startBound && isIdentifierChar(text[start - 1]))
        start--;
      if (start == end)
        return false;
      out = text.substr(start, end - start);
      return true;
    }

    if (text[end - 1] == ']') {
      int depth = 0;
      size_t i = end;
      while (i > startBound) {
        i--;
        char ch = text[i];
        if (ch == ']') {
          depth++;
          continue;
        }
        if (ch == '[') {
          depth--;
          if (depth == 0) {
            return parseBackward(startBound, i, out);
          }
        }
      }
      return false;
    }

    if (text[end - 1] == ')') {
      int depth = 0;
      size_t i = end;
      while (i > startBound) {
        i--;
        char ch = text[i];
        if (ch == ')') {
          depth++;
          continue;
        }
        if (ch == '(') {
          depth--;
          if (depth == 0) {
            if (parseBackward(i + 1, end - 1, out))
              return true;
            return parseBackward(startBound, i, out);
          }
        }
      }
      return false;
    }
    return false;
  };

  return parseBackward(0, cur, baseOut);
}

bool extractMemberAccessAtOffset(const std::string &text, size_t cursorOffset,
                                 std::string &baseOut, std::string &memberOut) {
  baseOut.clear();
  memberOut.clear();
  if (text.empty())
    return false;
  if (cursorOffset > text.size())
    cursorOffset = text.size();

  auto parseIdentifierLeft = [&](size_t endExclusive, std::string &out,
                                 size_t &startOut, size_t &endOut) -> bool {
    out.clear();
    startOut = endExclusive;
    endOut = endExclusive;
    size_t end = endExclusive;
    while (end > 0 && isSpace(text[end - 1]))
      end--;
    if (end == 0 || !isIdentifierChar(text[end - 1]))
      return false;
    size_t start = end;
    while (start > 0 && isIdentifierChar(text[start - 1]))
      start--;
    if (start == end)
      return false;
    out = text.substr(start, end - start);
    startOut = start;
    endOut = end;
    return true;
  };

  auto parseIdentifierRight = [&](size_t startInclusive, std::string &out,
                                  size_t &startOut, size_t &endOut) -> bool {
    out.clear();
    startOut = startInclusive;
    endOut = startInclusive;
    size_t start = startInclusive;
    while (start < text.size() && isSpace(text[start]))
      start++;
    if (start >= text.size() || !isIdentifierChar(text[start]))
      return false;
    size_t end = start;
    while (end < text.size() && isIdentifierChar(text[end]))
      end++;
    if (end == start)
      return false;
    out = text.substr(start, end - start);
    startOut = start;
    endOut = end;
    return true;
  };

  size_t dotPos = std::string::npos;
  if (cursorOffset < text.size() && text[cursorOffset] == '.') {
    dotPos = cursorOffset;
  } else if (cursorOffset > 0 && text[cursorOffset - 1] == '.') {
    dotPos = cursorOffset - 1;
  } else {
    size_t tokenStart = cursorOffset;
    size_t tokenEnd = cursorOffset;
    if (cursorOffset > 0 && isIdentifierChar(text[cursorOffset - 1])) {
      tokenStart = cursorOffset - 1;
      while (tokenStart > 0 && isIdentifierChar(text[tokenStart - 1]))
        tokenStart--;
      tokenEnd = cursorOffset;
      while (tokenEnd < text.size() && isIdentifierChar(text[tokenEnd]))
        tokenEnd++;
    } else if (cursorOffset < text.size() &&
               isIdentifierChar(text[cursorOffset])) {
      tokenStart = cursorOffset;
      while (tokenStart > 0 && isIdentifierChar(text[tokenStart - 1]))
        tokenStart--;
      tokenEnd = cursorOffset + 1;
      while (tokenEnd < text.size() && isIdentifierChar(text[tokenEnd]))
        tokenEnd++;
    }
    if (tokenStart != tokenEnd) {
      size_t left = tokenStart;
      while (left > 0 && isSpace(text[left - 1]))
        left--;
      if (left > 0 && text[left - 1] == '.') {
        dotPos = left - 1;
      } else {
        size_t right = tokenEnd;
        while (right < text.size() && isSpace(text[right]))
          right++;
        if (right < text.size() && text[right] == '.')
          dotPos = right;
      }
    }
  }

  if (dotPos == std::string::npos)
    return false;

  if (!extractMemberBaseIdentifierBeforeDot(text, dotPos, baseOut))
    return false;

  std::string memberName;
  size_t memberStart = 0;
  size_t memberEnd = 0;
  if (parseIdentifierRight(dotPos + 1, memberName, memberStart, memberEnd)) {
    memberOut = memberName;
  }
  return true;
}

bool parseMemberCallAtOffset(const std::string &text, size_t cursorOffset,
                             std::string &baseOut, std::string &memberOut,
                             int &activeParameterOut) {
  baseOut.clear();
  memberOut.clear();
  activeParameterOut = 0;
  std::string callee;
  CallSiteKind kind = CallSiteKind::FunctionCall;
  size_t openParen = std::string::npos;
  if (!parseCallLikeAtOffset(text, cursorOffset, callee, activeParameterOut,
                             kind, &openParen)) {
    return false;
  }
  if (kind != CallSiteKind::FunctionCall)
    return false;

  size_t memberEnd = openParen;
  while (memberEnd > 0 && isSpace(text[memberEnd - 1]))
    memberEnd--;
  size_t memberStart = memberEnd;
  while (memberStart > 0 && isIdentifierChar(text[memberStart - 1]))
    memberStart--;
  if (memberStart == memberEnd)
    return false;
  const std::string member = text.substr(memberStart, memberEnd - memberStart);

  size_t dotScan = memberStart;
  while (dotScan > 0 && isSpace(text[dotScan - 1]))
    dotScan--;
  if (dotScan == 0 || text[dotScan - 1] != '.')
    return false;

  if (!extractMemberBaseIdentifierBeforeDot(text, dotScan - 1, baseOut))
    return false;
  memberOut = member;
  return !baseOut.empty() && !memberOut.empty();
}

void collectCallArgumentsInRange(const std::string &text,
                                 size_t rangeStartOffset, size_t rangeEndOffset,
                                 std::vector<CallSiteArgument> &out) {
  out.clear();
  const size_t scanEnd = std::min(rangeEndOffset, text.size());
  if (rangeStartOffset >= scanEnd)
    return;

  enum class ScanState {
    Normal,
    LineComment,
    BlockComment,
    StringDouble,
    StringSingle
  };

  ScanState state = ScanState::Normal;
  std::vector<InlayCallFrame> stack;

  auto maybeRecordArgumentStart = [&](size_t pos) {
    if (stack.empty())
      return;
    InlayCallFrame &frame = stack.back();
    if (!frame.isCallable || !frame.expectingArgument)
      return;
    if (frame.angleDepth != 0 || frame.bracketDepth != 0 ||
        frame.braceDepth != 0)
      return;
    frame.expectingArgument = false;
    if (pos >= rangeStartOffset && pos < rangeEndOffset) {
      out.push_back({frame.functionName, frame.argumentIndex, pos,
                     frame.isMemberCall});
    }
  };

  for (size_t i = 0; i < scanEnd; i++) {
    char ch = text[i];
    char next = i + 1 < text.size() ? text[i + 1] : '\0';

    if (state == ScanState::LineComment) {
      if (ch == '\n')
        state = ScanState::Normal;
      continue;
    }
    if (state == ScanState::BlockComment) {
      if (ch == '*' && next == '/') {
        state = ScanState::Normal;
        i++;
      }
      continue;
    }
    if (state == ScanState::StringDouble) {
      if (ch == '\\' && next != '\0') {
        i++;
        continue;
      }
      if (ch == '"')
        state = ScanState::Normal;
      continue;
    }
    if (state == ScanState::StringSingle) {
      if (ch == '\\' && next != '\0') {
        i++;
        continue;
      }
      if (ch == '\'')
        state = ScanState::Normal;
      continue;
    }

    if (ch == '/' && next == '/') {
      state = ScanState::LineComment;
      i++;
      continue;
    }
    if (ch == '/' && next == '*') {
      state = ScanState::BlockComment;
      i++;
      continue;
    }
    if (ch == '"') {
      bool prefixed = i > 0 && isLikelyStringPrefixChar(text[i - 1]);
      if (!prefixed)
        state = ScanState::StringDouble;
      continue;
    }
    if (ch == '\'') {
      state = ScanState::StringSingle;
      continue;
    }

    if (ch == '(') {
      bool isMemberCall = false;
      std::string callName = extractCallNameBefore(text, i, &isMemberCall);
      InlayCallFrame frame;
      frame.isCallable =
          !callName.empty() && !isLikelyTypeConstructor(callName) &&
          !isCallLikeKeyword(callName);
      frame.isMemberCall = isMemberCall;
      frame.functionName = std::move(callName);
      stack.push_back(std::move(frame));
      continue;
    }
    if (ch == ')') {
      if (!stack.empty())
        stack.pop_back();
      continue;
    }
    if (stack.empty())
      continue;

    InlayCallFrame &frame = stack.back();
    if (!frame.isCallable)
      continue;

    if (ch == '<') {
      frame.angleDepth++;
      continue;
    }
    if (ch == '>' && frame.angleDepth > 0) {
      frame.angleDepth--;
      continue;
    }
    if (ch == '[') {
      frame.bracketDepth++;
      continue;
    }
    if (ch == ']' && frame.bracketDepth > 0) {
      frame.bracketDepth--;
      continue;
    }
    if (ch == '{') {
      frame.braceDepth++;
      continue;
    }
    if (ch == '}' && frame.braceDepth > 0) {
      frame.braceDepth--;
      continue;
    }
    if (frame.angleDepth == 0 && frame.bracketDepth == 0 &&
        frame.braceDepth == 0 && ch == ',') {
      frame.argumentIndex++;
      frame.expectingArgument = true;
      continue;
    }
    if (!isSpace(ch))
      maybeRecordArgumentStart(i);
  }
}

bool collectCallArgumentTokenRanges(
    const std::vector<LexToken> &tokens, size_t openParenIndex,
    std::vector<std::pair<size_t, size_t>> &rangesOut,
    size_t &closeParenIndexOut) {
  rangesOut.clear();
  closeParenIndexOut = std::string::npos;
  if (openParenIndex >= tokens.size() ||
      tokens[openParenIndex].kind != LexToken::Kind::Punct ||
      tokens[openParenIndex].text != "(") {
    return false;
  }
  size_t j = openParenIndex + 1;
  if (j < tokens.size() && tokens[j].kind == LexToken::Kind::Punct &&
      tokens[j].text == ")") {
    closeParenIndexOut = j;
    return true;
  }
  while (j < tokens.size()) {
    if (tokens[j].kind == LexToken::Kind::Punct && tokens[j].text == ")") {
      closeParenIndexOut = j;
      return true;
    }
    size_t argStart = j;
    int parenDepth = 0;
    int bracketDepth = 0;
    while (j < tokens.size()) {
      const auto &t = tokens[j];
      if (t.kind == LexToken::Kind::Punct) {
        if (t.text == "(")
          parenDepth++;
        else if (t.text == ")") {
          if (parenDepth == 0)
            break;
          parenDepth--;
        } else if (t.text == "[")
          bracketDepth++;
        else if (t.text == "]")
          bracketDepth = bracketDepth > 0 ? bracketDepth - 1 : 0;
        else if (t.text == "," && parenDepth == 0 && bracketDepth == 0)
          break;
      }
      j++;
    }
    rangesOut.push_back({argStart, j});
    if (j < tokens.size() && tokens[j].kind == LexToken::Kind::Punct &&
        tokens[j].text == ",") {
      j++;
      continue;
    }
    break;
  }
  if (j < tokens.size() && tokens[j].kind == LexToken::Kind::Punct &&
      tokens[j].text == ")") {
    closeParenIndexOut = j;
    return true;
  }
  return false;
}
