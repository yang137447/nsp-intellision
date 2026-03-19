#pragma once

#include "type_desc.hpp"

enum class TypeRelationKind {
  Exact,
  WidenPromotion,
  NumericConversion,
  ObjectFamilyConversion,
  Narrowing,
  Illegal
};

struct TypeRelationResult {
  TypeRelationKind kind = TypeRelationKind::Illegal;
  int cost = 1000000000;
  bool viable = false;
};

TypeRelationResult evaluateTypeRelation(const TypeDesc &expected,
                                        const TypeDesc &actual,
                                        bool allowNarrowing = false);

const char *typeRelationKindToString(TypeRelationKind kind);
