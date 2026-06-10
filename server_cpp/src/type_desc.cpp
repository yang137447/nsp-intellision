#include "type_desc.hpp"

#include "scalar_type_model.hpp"

#include <algorithm>
#include <cctype>
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
      "const", "in", "out", "inout", "uniform", "volatile", "static",
      "groupshared"};
  return qualifiers.find(value) != qualifiers.end();
}

bool isNumericScalar(const std::string &base) {
  if (base == "int64_t" || base == "uint64_t")
    return true;
  HlslScalarTypeShape shape;
  if (!parseHlslScalarVectorMatrixTypeShape(base, shape))
    return false;
  return shape.shape == HlslScalarTypeShapeKind::Scalar &&
         (hlslScalarTypeKindIsNumeric(shape.kind) ||
          hlslScalarTypeKindIsBoolean(shape.kind));
}

std::string normalizeMacroLikeNumericAlias(const std::string &typeToken) {
  static const std::vector<std::pair<std::string, std::string>> aliases = {
      {"materialfloat", "float"},
      {"materialhalf", "half"},
      {"materialdouble", "double"},
      {"materialint", "int"},
      {"materialuint", "uint"},
      {"materialint64", "int64_t"},
      {"materialuint64", "uint64_t"},
  };
  for (const auto &alias : aliases) {
    const std::string &prefix = alias.first;
    if (typeToken.rfind(prefix, 0) != 0)
      continue;
    const std::string suffix = typeToken.substr(prefix.size());
    if (suffix.empty())
      return alias.second;
    if (suffix.size() == 1 && suffix[0] >= '2' && suffix[0] <= '4')
      return alias.second + suffix;
    if (suffix.size() == 3 && suffix[0] >= '2' && suffix[0] <= '4' &&
        suffix[1] == 'x' && suffix[2] >= '2' && suffix[2] <= '4') {
      return alias.second + suffix;
    }
  }
  return typeToken;
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
  typeToken = normalizeMacroLikeNumericAlias(typeToken);

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

  HlslScalarTypeShape shape;
  if (parseHlslScalarVectorMatrixTypeShape(typeToken, shape) &&
      (hlslScalarTypeKindIsNumeric(shape.kind) ||
       hlslScalarTypeKindIsBoolean(shape.kind))) {
    out.base = shape.baseName;
    out.rows = shape.rows;
    out.cols = shape.cols;
    if (shape.shape == HlslScalarTypeShapeKind::Matrix) {
      out.kind = TypeDescKind::Matrix;
      return out;
    }
    if (shape.shape == HlslScalarTypeShapeKind::Vector && shape.rows > 1) {
      out.kind = TypeDescKind::Vector;
      return out;
    }
    out.kind = TypeDescKind::Scalar;
    out.rows = 1;
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
    if (!type.base.empty() && type.rows >= 1 && type.rows <= 4 && type.cols >= 1 &&
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
