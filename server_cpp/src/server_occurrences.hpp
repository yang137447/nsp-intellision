#pragma once

#include <string>

struct LocatedOccurrence {
  std::string uri;
  int line = 0;
  int start = 0;
  int end = 0;
};
