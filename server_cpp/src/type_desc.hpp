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

// Parses a type token into the shared lightweight type shape used by overload
// ranking and diagnostics compatibility checks. This is intentionally limited
// to HLSL scalar/vector/matrix names, 64-bit integer scalar aliases used by
// project shaders, known object aliases, and common macro-like numeric aliases
// such as MaterialFloat3; full typedef expansion and user struct modeling stay
// in semantic snapshot / symbol query layers.
TypeDesc parseTypeDesc(const std::string &value);
std::string typeDescToCanonicalString(const TypeDesc &type);
bool typeDescIsNumeric(const TypeDesc &type);
