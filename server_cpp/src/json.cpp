#include "json.hpp"
#include <cctype>
#include <cstdlib>
#include <sstream>

static void skipWs(const std::string &text, size_t &i) {
  while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
    i++;
  }
}

static bool parseValue(const std::string &text, size_t &i, Json &out);

static bool parseString(const std::string &text, size_t &i, std::string &out) {
  if (i >= text.size() || text[i] != '"')
    return false;
  i++;
  std::string result;
  while (i < text.size()) {
    char c = text[i++];
    if (c == '"') {
      out = result;
      return true;
    }
    if (c == '\\') {
      if (i >= text.size())
        return false;
      char esc = text[i++];
      switch (esc) {
      case '"':
        result.push_back('"');
        break;
      case '\\':
        result.push_back('\\');
        break;
      case '/':
        result.push_back('/');
        break;
      case 'b':
        result.push_back('\b');
        break;
      case 'f':
        result.push_back('\f');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      case 'u': {
        if (i + 4 > text.size())
          return false;
        unsigned int code = 0;
        for (int k = 0; k < 4; k++) {
          char h = text[i++];
          code <<= 4;
          if (h >= '0' && h <= '9')
            code |= static_cast<unsigned int>(h - '0');
          else if (h >= 'a' && h <= 'f')
            code |= static_cast<unsigned int>(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F')
            code |= static_cast<unsigned int>(h - 'A' + 10);
          else
            return false;
        }
        if (code <= 0x7F)
          result.push_back(static_cast<char>(code));
        else if (code <= 0x7FF) {
          result.push_back(static_cast<char>(0xC0 | ((code >> 6) & 0x1F)));
          result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
          result.push_back(static_cast<char>(0xE0 | ((code >> 12) & 0x0F)));
          result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        break;
      }
      default:
        return false;
      }
      continue;
    }
    result.push_back(c);
  }
  return false;
}

static bool parseNumber(const std::string &text, size_t &i, double &out) {
  size_t start = i;
  if (text[i] == '-')
    i++;
  while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
    i++;
  if (i < text.size() && text[i] == '.') {
    i++;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
      i++;
  }
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    i++;
    if (i < text.size() && (text[i] == '+' || text[i] == '-'))
      i++;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
      i++;
  }
  char *endPtr = nullptr;
  std::string sub = text.substr(start, i - start);
  out = std::strtod(sub.c_str(), &endPtr);
  return endPtr != sub.c_str();
}

static bool parseArray(const std::string &text, size_t &i, Json &out) {
  if (text[i] != '[')
    return false;
  i++;
  skipWs(text, i);
  std::vector<Json> values;
  if (i < text.size() && text[i] == ']') {
    i++;
    out.type = Json::Type::Array;
    out.a = std::move(values);
    return true;
  }
  while (i < text.size()) {
    Json value;
    if (!parseValue(text, i, value))
      return false;
    values.push_back(std::move(value));
    skipWs(text, i);
    if (i >= text.size())
      return false;
    if (text[i] == ',') {
      i++;
      skipWs(text, i);
      continue;
    }
    if (text[i] == ']') {
      i++;
      out.type = Json::Type::Array;
      out.a = std::move(values);
      return true;
    }
    return false;
  }
  return false;
}

static bool parseObject(const std::string &text, size_t &i, Json &out) {
  if (text[i] != '{')
    return false;
  i++;
  skipWs(text, i);
  std::unordered_map<std::string, Json> values;
  if (i < text.size() && text[i] == '}') {
    i++;
    out.type = Json::Type::Object;
    out.o = std::move(values);
    return true;
  }
  while (i < text.size()) {
    std::string key;
    if (!parseString(text, i, key))
      return false;
    skipWs(text, i);
    if (i >= text.size() || text[i] != ':')
      return false;
    i++;
    skipWs(text, i);
    Json value;
    if (!parseValue(text, i, value))
      return false;
    values.emplace(std::move(key), std::move(value));
    skipWs(text, i);
    if (i >= text.size())
      return false;
    if (text[i] == ',') {
      i++;
      skipWs(text, i);
      continue;
    }
    if (text[i] == '}') {
      i++;
      out.type = Json::Type::Object;
      out.o = std::move(values);
      return true;
    }
    return false;
  }
  return false;
}

static bool parseValue(const std::string &text, size_t &i, Json &out) {
  skipWs(text, i);
  if (i >= text.size())
    return false;
  char c = text[i];
  if (c == '"') {
    std::string value;
    if (!parseString(text, i, value))
      return false;
    out.type = Json::Type::String;
    out.s = std::move(value);
    return true;
  }
  if (c == '{') {
    return parseObject(text, i, out);
  }
  if (c == '[') {
    return parseArray(text, i, out);
  }
  if (c == 't' && text.compare(i, 4, "true") == 0) {
    i += 4;
    out.type = Json::Type::Bool;
    out.b = true;
    return true;
  }
  if (c == 'f' && text.compare(i, 5, "false") == 0) {
    i += 5;
    out.type = Json::Type::Bool;
    out.b = false;
    return true;
  }
  if (c == 'n' && text.compare(i, 4, "null") == 0) {
    i += 4;
    out.type = Json::Type::Null;
    return true;
  }
  if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
    double value = 0.0;
    if (!parseNumber(text, i, value))
      return false;
    out.type = Json::Type::Number;
    out.n = value;
    return true;
  }
  return false;
}

bool parseJson(const std::string &text, Json &out) {
  size_t i = 0;
  if (!parseValue(text, i, out))
    return false;
  skipWs(text, i);
  return i == text.size();
}

static std::string escapeJson(const std::string &value) {
  std::ostringstream out;
  for (char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out << "\\u";
        out << std::hex << std::uppercase << (int)c;
        out << std::dec << std::nouppercase;
      } else {
        out << c;
      }
    }
  }
  return out.str();
}

std::string serializeJson(const Json &value) {
  switch (value.type) {
  case Json::Type::Null:
    return "null";
  case Json::Type::Bool:
    return value.b ? "true" : "false";
  case Json::Type::Number: {
    std::ostringstream out;
    out << value.n;
    return out.str();
  }
  case Json::Type::String:
    return "\"" + escapeJson(value.s) + "\"";
  case Json::Type::Array: {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < value.a.size(); i++) {
      if (i > 0)
        out << ",";
      out << serializeJson(value.a[i]);
    }
    out << "]";
    return out.str();
  }
  case Json::Type::Object: {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto &pair : value.o) {
      if (!first)
        out << ",";
      first = false;
      out << "\"" << escapeJson(pair.first) << "\":" << serializeJson(pair.second);
    }
    out << "}";
    return out.str();
  }
  }
  return "null";
}

const Json *getObjectValue(const Json &obj, const std::string &key) {
  if (obj.type != Json::Type::Object)
    return nullptr;
  auto it = obj.o.find(key);
  if (it == obj.o.end())
    return nullptr;
  return &it->second;
}

std::string getStringValue(const Json &value, const std::string &fallback) {
  if (value.type == Json::Type::String)
    return value.s;
  return fallback;
}

double getNumberValue(const Json &value, double fallback) {
  if (value.type == Json::Type::Number)
    return value.n;
  return fallback;
}

bool getBoolValue(const Json &value, bool fallback) {
  if (value.type == Json::Type::Bool)
    return value.b;
  return fallback;
}
