#include "type_desc.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <unordered_set>

namespace {
std::string toLowerCopy(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string trimCopy(const std::string &value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start]))) {
    start++;
  }
  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }
  return value.substr(start, end - start);
}

std::vector<std::string> splitBySpace(const std::string &value) {
  std::vector<std::string> out;
  std::string current;
  for (char ch : value) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      if (!current.empty()) {
        out.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty())
    out.push_back(current);
  return out;
}

bool isQualifierToken(const std::string &value) {
  static const std::unordered_set<std::string> qualifiers = {
      "const", "in", "out", "inout", "uniform", "volatile", "static"};
  return qualifiers.find(value) != qualifiers.end();
}

bool isNumericScalar(const std::string &base) {
  return base == "float" || base == "half" || base == "double" ||
         base == "int" || base == "uint" || base == "bool";
}
} // namespace

TypeDesc parseTypeDesc(const std::string &value) {
  TypeDesc out;
  const std::string trimmed = trimCopy(toLowerCopy(value));
  if (trimmed.empty())
    return out;

  std::vector<std::string> parts = splitBySpace(trimmed);
  std::string typeToken;
  out.qualifiers.reserve(parts.size());
  for (const std::string &part : parts) {
    if (isQualifierToken(part)) {
      out.qualifiers.push_back(part);
      continue;
    }
    if (typeToken.empty())
      typeToken = part;
  }
  if (typeToken.empty())
    return out;

  if (typeToken == "texture") {
    out.kind = TypeDescKind::Object;
    out.objectKind = ObjectTypeKind::Texture;
    out.base = "texture";
    return out;
  }
  if (typeToken == "texture2d") {
    out.kind = TypeDescKind::Object;
    out.objectKind = ObjectTypeKind::Texture2D;
    out.base = "texture2d";
    return out;
  }
  if (typeToken == "sampler") {
    out.kind = TypeDescKind::Object;
    out.objectKind = ObjectTypeKind::Sampler;
    out.base = "sampler";
    return out;
  }
  if (typeToken == "samplerstate") {
    out.kind = TypeDescKind::Object;
    out.objectKind = ObjectTypeKind::SamplerState;
    out.base = "samplerstate";
    return out;
  }

  static const std::regex matrixPattern(
      R"(^(float|half|double|int|uint|bool)([2-4])x([2-4])$)");
  std::smatch matrixMatch;
  if (std::regex_match(typeToken, matrixMatch, matrixPattern)) {
    out.kind = TypeDescKind::Matrix;
    out.base = matrixMatch[1].str();
    out.rows = std::stoi(matrixMatch[2].str());
    out.cols = std::stoi(matrixMatch[3].str());
    return out;
  }

  static const std::regex vectorPattern(
      R"(^(float|half|double|int|uint|bool)([2-4])$)");
  std::smatch vectorMatch;
  if (std::regex_match(typeToken, vectorMatch, vectorPattern)) {
    out.kind = TypeDescKind::Vector;
    out.base = vectorMatch[1].str();
    out.rows = std::stoi(vectorMatch[2].str());
    out.cols = 1;
    return out;
  }

  if (isNumericScalar(typeToken)) {
    out.kind = TypeDescKind::Scalar;
    out.base = typeToken;
    out.rows = 1;
    out.cols = 1;
    return out;
  }

  out.kind = TypeDescKind::Object;
  out.objectKind = ObjectTypeKind::Unknown;
  out.base = typeToken;
  return out;
}

std::string typeDescToCanonicalString(const TypeDesc &type) {
  switch (type.kind) {
  case TypeDescKind::Scalar:
    return type.base;
  case TypeDescKind::Vector:
    if (!type.base.empty() && type.rows >= 2 && type.rows <= 4)
      return type.base + std::to_string(type.rows);
    return type.base;
  case TypeDescKind::Matrix:
    if (!type.base.empty() && type.rows >= 2 && type.rows <= 4 && type.cols >= 2 &&
        type.cols <= 4) {
      return type.base + std::to_string(type.rows) + "x" +
             std::to_string(type.cols);
    }
    return type.base;
  case TypeDescKind::Object:
    if (!type.base.empty())
      return type.base;
    switch (type.objectKind) {
    case ObjectTypeKind::Texture:
      return "texture";
    case ObjectTypeKind::Texture2D:
      return "texture2d";
    case ObjectTypeKind::Sampler:
      return "sampler";
    case ObjectTypeKind::SamplerState:
      return "samplerstate";
    case ObjectTypeKind::Unknown:
      break;
    }
    return "";
  case TypeDescKind::Unknown:
    return "";
  }
  return "";
}

bool typeDescIsNumeric(const TypeDesc &type) {
  if (type.kind == TypeDescKind::Scalar || type.kind == TypeDescKind::Vector ||
      type.kind == TypeDescKind::Matrix) {
    return isNumericScalar(type.base);
  }
  return false;
}
