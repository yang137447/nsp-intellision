#pragma once

#include <string>

struct DefinitionLocation {
  std::string uri;
  int line = -1;
  int start = -1;
  int end = -1;
};
