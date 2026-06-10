#pragma once

#include <string>
#include <vector>

// Shared scalar/vector/matrix type-name model backed by
// resources/types/scalar_types.
//
// Responsibilities:
// - Load and merge the scalar type bundle once through resource_registry.
// - Expose precomputed scalar/vector/matrix type-name sets for completion,
//   semantic tokens, parser helpers, and diagnostics.
// - Parse a type token into a consumer-ready shape without each feature
//   duplicating scalar base lists or vector/matrix regexes.
//
// Non-goals:
// - Object/resource type family semantics stay in type_model.*.
// - Full typedef expansion and user struct modeling stay in semantic snapshot
//   and symbol-query layers.
// - Project-only aliases such as int64_t / uint64_t remain implementation
//   aliases in type_desc/type_relation unless promoted to a resource entry.
enum class HlslScalarTypeKind { Unknown, Bool, Integer, Floating, Special };

enum class HlslScalarTypeShapeKind { Unknown, Scalar, Vector, Matrix };

struct HlslScalarTypeShape {
  HlslScalarTypeShapeKind shape = HlslScalarTypeShapeKind::Unknown;
  HlslScalarTypeKind kind = HlslScalarTypeKind::Unknown;
  std::string baseName;
  int rows = 0;
  int cols = 0;
  bool minPrecision = false;
  bool legacy = false;
};

bool isScalarTypeModelAvailable();
const std::string &getScalarTypeModelError();

const std::vector<std::string> &getHlslScalarTypeBaseNames();
const std::vector<std::string> &getHlslScalarVectorMatrixTypeNames();

bool isHlslScalarTypeBaseName(const std::string &typeName);
bool isHlslScalarVectorMatrixTypeName(const std::string &typeName);
bool parseHlslScalarVectorMatrixTypeShape(
    const std::string &typeName, HlslScalarTypeShape &outShape);

bool hlslScalarTypeKindIsNumeric(HlslScalarTypeKind kind);
bool hlslScalarTypeKindIsBoolean(HlslScalarTypeKind kind);
