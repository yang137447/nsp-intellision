#include "text_utils.hpp"
#include <cctype>
#include <sstream>

static bool isWordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static uint32_t decodeUtf8CodePoint(const std::string &text, size_t &i) {
  unsigned char lead = static_cast<unsigned char>(text[i]);
  if (lead < 0x80) {
    i += 1;
    return lead;
  }
  if ((lead >> 5) == 0x6 && i + 1 < text.size()) {
    unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
    if ((b1 & 0xC0) != 0x80) {
      i += 1;
      return lead;
    }
    uint32_t cp = ((lead & 0x1F) << 6) | (b1 & 0x3F);
    i += 2;
    return cp;
  }
  if ((lead >> 4) == 0xE && i + 2 < text.size()) {
    unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
    unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
      i += 1;
      return lead;
    }
    uint32_t cp =
        ((lead & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
    i += 3;
    return cp;
  }
  if ((lead >> 3) == 0x1E && i + 3 < text.size()) {
    unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
    unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
    unsigned char b3 = static_cast<unsigned char>(text[i + 3]);
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
      i += 1;
      return lead;
    }
    uint32_t cp = ((lead & 0x07) << 18) | ((b1 & 0x3F) << 12) |
                  ((b2 & 0x3F) << 6) | (b3 & 0x3F);
    i += 4;
    return cp;
  }
  i += 1;
  return lead;
}

int utf16ToByteOffsetInLine(const std::string &lineText, int utf16Character) {
  if (utf16Character <= 0)
    return 0;
  int col = 0;
  size_t i = 0;
  while (i < lineText.size()) {
    size_t start = i;
    uint32_t cp = decodeUtf8CodePoint(lineText, i);
    int units = cp > 0xFFFF ? 2 : 1;
    if (col + units > utf16Character) {
      return static_cast<int>(start);
    }
    col += units;
    if (col == utf16Character) {
      return static_cast<int>(i);
    }
  }
  return static_cast<int>(lineText.size());
}

int byteOffsetInLineToUtf16(const std::string &lineText, int byteOffset) {
  if (byteOffset <= 0)
    return 0;
  size_t limit = std::min(static_cast<size_t>(byteOffset), lineText.size());
  int col = 0;
  size_t i = 0;
  while (i < limit) {
    uint32_t cp = decodeUtf8CodePoint(lineText, i);
    col += cp > 0xFFFF ? 2 : 1;
  }
  return col;
}

size_t positionToOffsetUtf16(const std::string &text, int line,
                             int character) {
  if (line < 0 || character < 0)
    return 0;
  size_t offset = 0;
  int currentLine = 0;
  while (offset < text.size() && currentLine < line) {
    if (text[offset] == '\n')
      currentLine++;
    offset++;
  }
  size_t lineStart = offset;
  while (offset < text.size() && text[offset] != '\n') {
    offset++;
  }
  size_t lineEnd = offset;
  std::string lineText = text.substr(lineStart, lineEnd - lineStart);
  int byteInLine = utf16ToByteOffsetInLine(lineText, character);
  size_t result = lineStart + static_cast<size_t>(std::max(0, byteInLine));
  return std::min(result, lineEnd);
}

std::string getLineAt(const std::string &text, int line) {
  if (line < 0)
    return "";
  int current = 0;
  size_t start = 0;
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] == '\n') {
      if (current == line) {
        return text.substr(start, i - start);
      }
      current++;
      start = i + 1;
    }
  }
  if (current == line) {
    return text.substr(start);
  }
  return "";
}

std::string extractWordAt(const std::string &lineText, int character) {
  if (lineText.empty())
    return "";
  int idx = utf16ToByteOffsetInLine(lineText, character);
  if (idx >= static_cast<int>(lineText.size()))
    idx = static_cast<int>(lineText.size());
  if (idx > 0 && idx == static_cast<int>(lineText.size()))
    idx -= 1;
  if (idx < 0)
    return "";
  if (!isWordChar(lineText[idx])) {
    if (idx > 0 && isWordChar(lineText[idx - 1])) {
      idx -= 1;
    } else {
      return "";
    }
  }
  int start = idx;
  int end = idx + 1;
  while (start > 0 && isWordChar(lineText[start - 1]))
    start--;
  while (end < static_cast<int>(lineText.size()) && isWordChar(lineText[end]))
    end++;
  if (end <= start)
    return "";
  return lineText.substr(start, end - start);
}

std::vector<Occurrence> findOccurrences(const std::string &text, const std::string &word) {
  std::vector<Occurrence> results;
  if (word.empty())
    return results;
  std::istringstream stream(text);
  std::string lineText;
  int line = 0;
  while (std::getline(stream, lineText)) {
    size_t pos = 0;
    while (pos < lineText.size()) {
      size_t found = lineText.find(word, pos);
      if (found == std::string::npos)
        break;
      bool leftOk = found == 0 || !isWordChar(lineText[found - 1]);
      bool rightOk = (found + word.size() >= lineText.size()) ||
                     !isWordChar(lineText[found + word.size()]);
      if (leftOk && rightOk) {
        int startUtf16 = byteOffsetInLineToUtf16(lineText, static_cast<int>(found));
        int endUtf16 = byteOffsetInLineToUtf16(
            lineText, static_cast<int>(found + word.size()));
        results.push_back({line, startUtf16, endUtf16});
      }
      pos = found + word.size();
    }
    line++;
  }
  return results;
}
