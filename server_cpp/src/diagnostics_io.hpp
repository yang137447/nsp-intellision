#pragma once

#include <string>

std::string diagnosticsGetLineByIndex(const std::string &text, int lineIndex);

bool diagnosticsReadFileToString(const std::string &path, std::string &out);
