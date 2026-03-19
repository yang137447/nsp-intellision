#pragma once
#include <string>
#include <unordered_map>
#include <vector>

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool b = false;
  double n = 0.0;
  std::string s;
  std::vector<Json> a;
  std::unordered_map<std::string, Json> o;
};

bool parseJson(const std::string &text, Json &out);
std::string serializeJson(const Json &value);
const Json *getObjectValue(const Json &obj, const std::string &key);
std::string getStringValue(const Json &value, const std::string &fallback = "");
double getNumberValue(const Json &value, double fallback = 0.0);
bool getBoolValue(const Json &value, bool fallback = false);
