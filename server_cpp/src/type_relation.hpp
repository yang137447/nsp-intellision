#pragma once

#include "type_desc.hpp"

#include <string>
#include <vector>

enum class TypeRelationKind {
  Exact,
  ScalarSplat,
  WidenPromotion,
  NumericConversion,
  ObjectFamilyConversion,
  Narrowing,
  Truncation,
  Illegal
};

enum class TypeRelationWarningKind {
  None,
  Truncation,
  Boolean,
  FloatingIntegral,
  Signedness,
  Narrowing
};

struct TypeRelationOptions {
  bool suppressWarnings = false;
  bool actualIsNumericLiteral = false;
  std::string actualLiteralText;
};

struct TypeRelationResult {
  TypeRelationKind kind = TypeRelationKind::Illegal;
  TypeRelationWarningKind warningKind = TypeRelationWarningKind::None;
  int cost = 1000000000;
  bool viable = false;
  std::string expectedType;
  std::string actualType;
};

struct TypeBinaryConversionResult {
  bool viable = false;
  std::string resultType;
  TypeRelationResult leftConversion;
  TypeRelationResult rightConversion;
};

// Shared HLSL conversion model used by diagnostics and overload ranking.
// The result answers whether official implicit conversion can form from
// `actual` to `expected`, and carries the highest-priority warning that should
// be shown for legal but risky implicit conversions. It intentionally models
// type compatibility only; target-profile-specific storage width, full constant
// folding, object-method coordinate dimensions, and label syntax are owned by
// their dedicated layers.
TypeRelationResult evaluateTypeRelation(const TypeDesc &expected,
                                        const TypeDesc &actual,
                                        bool allowNarrowing = false);

TypeRelationResult
evaluateTypeRelationWithOptions(const TypeDesc &expected,
                                const TypeDesc &actual,
                                const TypeRelationOptions &options);

TypeBinaryConversionResult
evaluateUsualArithmeticConversion(const TypeDesc &left,
                                  const TypeDesc &right);

const char *typeRelationKindToString(TypeRelationKind kind);
const char *typeRelationWarningKindToString(TypeRelationWarningKind kind);
int typeRelationWarningPriority(TypeRelationWarningKind kind);
std::string typeRelationWarningMessage(const TypeRelationResult &relation);
