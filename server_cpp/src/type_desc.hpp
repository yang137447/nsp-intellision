#pragma once

#include <string>
#include <vector>

enum class TypeDescKind { Unknown, Scalar, Vector, Matrix, Object };

enum class ObjectTypeKind {
  Unknown,
  Texture,
  Texture2D,
  Sampler,
  SamplerState
};

struct TypeDesc {
  TypeDescKind kind = TypeDescKind::Unknown;
  std::string base;
  int rows = 0;
  int cols = 0;
  ObjectTypeKind objectKind = ObjectTypeKind::Unknown;
  std::vector<std::string> qualifiers;
};

TypeDesc parseTypeDesc(const std::string &value);
std::string typeDescToCanonicalString(const TypeDesc &type);
bool typeDescIsNumeric(const TypeDesc &type);
