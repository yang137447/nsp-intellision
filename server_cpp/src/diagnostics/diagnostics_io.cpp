#include "diagnostics_io.hpp"

#include <fstream>
#include <sstream>
#include <string>

bool diagnosticsReadFileToString(const std::string &path, std::string &out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  out = buffer.str();
  return true;
}

std::string diagnosticsGetLineByIndex(const std::string &text, int lineIndex) {
  if (lineIndex < 0)
    return "";
  std::istringstream stream(text);
  std::string lineText;
  int i = 0;
  while (std::getline(stream, lineText)) {
    if (i == lineIndex)
      return lineText;
    i++;
  }
  return "";
}
