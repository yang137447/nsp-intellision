#pragma once
#include <string>
#include <vector>

struct Occurrence {
  int line = 0;
  int start = 0;
  int end = 0;
};

std::string getLineAt(const std::string &text, int line);
std::string extractWordAt(const std::string &lineText, int character);
std::vector<Occurrence> findOccurrences(const std::string &text, const std::string &word);

int utf16ToByteOffsetInLine(const std::string &lineText, int utf16Character);
int byteOffsetInLineToUtf16(const std::string &lineText, int byteOffset);
size_t positionToOffsetUtf16(const std::string &text, int line, int character);
