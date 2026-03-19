#include "type_relation.hpp"
#include "type_model.hpp"

namespace {
constexpr int kIllegalCost = 1000000000;

bool isFloatFamily(const std::string &base) {
  return base == "half" || base == "float" || base == "double";
}

bool isIntFamily(const std::string &base) {
  return base == "int" || base == "uint";
}

int floatRank(const std::string &base) {
  if (base == "half")
    return 0;
  if (base == "float")
    return 1;
  if (base == "double")
    return 2;
  return -1;
}

bool sameShape(const TypeDesc &a, const TypeDesc &b) {
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
  case TypeDescKind::Scalar:
    return true;
  case TypeDescKind::Vector:
    return a.rows == b.rows;
  case TypeDescKind::Matrix:
    return a.rows == b.rows && a.cols == b.cols;
  case TypeDescKind::Object:
    return true;
  case TypeDescKind::Unknown:
    return false;
  }
  return false;
}

bool sameObjectFamily(const TypeDesc &expected, const TypeDesc &actual) {
  if (!expected.base.empty() && !actual.base.empty() &&
      typeModelSameObjectFamily(expected.base, actual.base)) {
    return true;
  }
  if (expected.objectKind == actual.objectKind &&
      expected.objectKind != ObjectTypeKind::Unknown) {
    return true;
  }
  if ((expected.objectKind == ObjectTypeKind::Texture &&
       actual.objectKind == ObjectTypeKind::Texture2D) ||
      (expected.objectKind == ObjectTypeKind::Texture2D &&
       actual.objectKind == ObjectTypeKind::Texture)) {
    return true;
  }
  if ((expected.objectKind == ObjectTypeKind::Sampler &&
       actual.objectKind == ObjectTypeKind::SamplerState) ||
      (expected.objectKind == ObjectTypeKind::SamplerState &&
       actual.objectKind == ObjectTypeKind::Sampler)) {
    return true;
  }
  return false;
}

TypeRelationResult makeResult(TypeRelationKind kind, int cost, bool viable) {
  TypeRelationResult out;
  out.kind = kind;
  out.cost = cost;
  out.viable = viable;
  return out;
}
} // namespace

TypeRelationResult evaluateTypeRelation(const TypeDesc &expected,
                                        const TypeDesc &actual,
                                        bool allowNarrowing) {
  if (expected.kind == TypeDescKind::Unknown ||
      actual.kind == TypeDescKind::Unknown) {
    return makeResult(TypeRelationKind::Illegal, kIllegalCost, false);
  }

  if (expected.kind == TypeDescKind::Object ||
      actual.kind == TypeDescKind::Object) {
    if (expected.kind != TypeDescKind::Object ||
        actual.kind != TypeDescKind::Object) {
      return makeResult(TypeRelationKind::Illegal, kIllegalCost, false);
    }
    if (expected.base == actual.base && !expected.base.empty()) {
      return makeResult(TypeRelationKind::Exact, 0, true);
    }
    if (sameObjectFamily(expected, actual)) {
      return makeResult(TypeRelationKind::ObjectFamilyConversion, 3, true);
    }
    return makeResult(TypeRelationKind::Illegal, kIllegalCost, false);
  }

  if (!sameShape(expected, actual)) {
    return makeResult(TypeRelationKind::Illegal, kIllegalCost, false);
  }

  if (expected.base == actual.base) {
    return makeResult(TypeRelationKind::Exact, 0, true);
  }

  if (isFloatFamily(expected.base) && isFloatFamily(actual.base)) {
    int eRank = floatRank(expected.base);
    int aRank = floatRank(actual.base);
    if (eRank < 0 || aRank < 0)
      return makeResult(TypeRelationKind::Illegal, kIllegalCost, false);
    if (aRank <= eRank) {
      return makeResult(TypeRelationKind::WidenPromotion, 1, true);
    }
    if (!allowNarrowing) {
      return makeResult(TypeRelationKind::Narrowing, 8, false);
    }
    return makeResult(TypeRelationKind::Narrowing, 8, true);
  }

  if (isIntFamily(expected.base) && isIntFamily(actual.base)) {
    return makeResult(TypeRelationKind::NumericConversion, 2, true);
  }

  if ((isFloatFamily(expected.base) && isIntFamily(actual.base)) ||
      (isIntFamily(expected.base) && isFloatFamily(actual.base))) {
    return makeResult(TypeRelationKind::NumericConversion, 2, true);
  }

  if (expected.base == "bool" && actual.base == "bool") {
    return makeResult(TypeRelationKind::Exact, 0, true);
  }

  return makeResult(TypeRelationKind::Illegal, kIllegalCost, false);
}

const char *typeRelationKindToString(TypeRelationKind kind) {
  switch (kind) {
  case TypeRelationKind::Exact:
    return "exact";
  case TypeRelationKind::WidenPromotion:
    return "widen_promotion";
  case TypeRelationKind::NumericConversion:
    return "numeric_conversion";
  case TypeRelationKind::ObjectFamilyConversion:
    return "object_family_conversion";
  case TypeRelationKind::Narrowing:
    return "narrowing";
  case TypeRelationKind::Illegal:
    return "illegal";
  }
  return "illegal";
}
