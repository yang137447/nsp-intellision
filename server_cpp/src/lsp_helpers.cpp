#include "lsp_helpers.hpp"

Json makeNull() {
  Json v;
  v.type = Json::Type::Null;
  return v;
}

Json makeString(const std::string &value) {
  Json v;
  v.type = Json::Type::String;
  v.s = value;
  return v;
}

Json makeNumber(double value) {
  Json v;
  v.type = Json::Type::Number;
  v.n = value;
  return v;
}

Json makeBool(bool value) {
  Json v;
  v.type = Json::Type::Bool;
  v.b = value;
  return v;
}

Json makeObject() {
  Json v;
  v.type = Json::Type::Object;
  return v;
}

Json makeArray() {
  Json v;
  v.type = Json::Type::Array;
  return v;
}

Json makePosition(int line, int character) {
  Json pos = makeObject();
  pos.o["line"] = makeNumber(line);
  pos.o["character"] = makeNumber(character);
  return pos;
}

Json makeRange(int line, int character) {
  Json range = makeObject();
  range.o["start"] = makePosition(line, character);
  range.o["end"] = makePosition(line, character);
  return range;
}

Json makeRangeExact(int line, int start, int end) {
  Json range = makeObject();
  range.o["start"] = makePosition(line, start);
  range.o["end"] = makePosition(line, end);
  return range;
}

Json makeLocation(const std::string &uri) {
  Json loc = makeObject();
  loc.o["uri"] = makeString(uri);
  loc.o["range"] = makeRange(0, 0);
  return loc;
}

Json makeLocationRange(const std::string &uri, int line, int start, int end) {
  Json loc = makeObject();
  loc.o["uri"] = makeString(uri);
  loc.o["range"] = makeRangeExact(line, start, end);
  return loc;
}

Json makeMarkup(const std::string &markdown) {
  Json content = makeObject();
  content.o["kind"] = makeString("markdown");
  content.o["value"] = makeString(markdown);
  return content;
}
