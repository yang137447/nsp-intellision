#pragma once
#include <string>
#include "json.hpp"

Json makeNull();
Json makeString(const std::string &value);
Json makeNumber(double value);
Json makeBool(bool value);
Json makeObject();
Json makeArray();
Json makePosition(int line, int character);
Json makeRange(int line, int character);
Json makeRangeExact(int line, int start, int end);
Json makeLocation(const std::string &uri);
Json makeLocationRange(const std::string &uri, int line, int start, int end);
Json makeMarkup(const std::string &markdown);
