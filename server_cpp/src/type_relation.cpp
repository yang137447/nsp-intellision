#include "type_relation.hpp"

#include "type_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace {
constexpr int kIllegalCost = 1000000000;

bool isFloatFamily(const std::string &base) {
  return base == "min10float" || base == "min16float" || base == "half" ||
         base == "float" || base == "double";
}

bool isIntFamily(const std::string &base) {
  return base == "min12int" || base == "min16int" ||
         base == "min16uint" || base == "dword" || base == "int" ||
         base == "uint" || base == "int64_t" || base == "uint64_t";
}

bool isNumericScalarBase(const std::string &base) {
  return isFloatFamily(base) || isIntFamily(base);
}

int floatRank(const std::string &base) {
  if (base == "min10float" || base == "min16float" || base == "half")
    return 0;
  if (base == "float")
    return 1;
  if (base == "double")
    return 2;
  return -1;
}

bool isUnsignedIntFamily(const std::string &base) {
  return base == "min16uint" || base == "dword" || base == "uint" ||
         base == "uint64_t";
}

int intRank(const std::string &base) {
  if (base == "min12int" || base == "min16int" || base == "min16uint" ||
      base == "dword" || base == "int" || base == "uint")
    return 0;
  if (base == "int64_t" || base == "uint64_t")
    return 1;
  return -1;
}

std::string makeShapeType(const std::string &base, TypeDescKind kind, int rows,
                          int cols) {
  if (base.empty())
    return "";
  if (kind == TypeDescKind::Scalar)
    return base;
  if (kind == TypeDescKind::Vector && rows > 1)
    return base + std::to_string(rows);
  if (kind == TypeDescKind::Matrix && rows > 0 && cols > 0)
    return base + std::to_string(rows) + "x" + std::to_string(cols);
  return base;
}

std::string canonicalTypeOrFallback(const TypeDesc &type) {
  const std::string canonical = typeDescToCanonicalString(type);
  if (!canonical.empty())
    return canonical;
  return type.base;
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

bool literalLooksFloating(const std::string &text) {
  for (char ch : text) {
    if (ch == '.' || ch == 'e' || ch == 'E' || ch == 'h' || ch == 'H' ||
        ch == 'f' || ch == 'F')
      return true;
  }
  return false;
}

bool parseSimpleNumericLiteralValue(const std::string &text, double &out) {
  if (text.empty())
    return false;
  char *end = nullptr;
  out = std::strtod(text.c_str(), &end);
  if (end == text.c_str())
    return false;
  while (end && *end != '\0') {
    if (*end == 'h' || *end == 'H' || *end == 'f' || *end == 'F' ||
        *end == 'l' || *end == 'L' || *end == 'u' || *end == 'U') {
      end++;
      continue;
    }
    return false;
  }
  return true;
}

bool literalIsSafelyAdaptable(const TypeDesc &expected, const TypeDesc &actual,
                              const TypeRelationOptions &options) {
  if (!options.actualIsNumericLiteral)
    return false;
  if (!isNumericScalarBase(expected.base) || !isNumericScalarBase(actual.base))
    return false;
  if (expected.kind != TypeDescKind::Scalar &&
      expected.kind != TypeDescKind::Vector &&
      expected.kind != TypeDescKind::Matrix)
    return false;
  if (actual.kind != TypeDescKind::Scalar)
    return false;
  if (expected.base == "bool" || actual.base == "bool")
    return false;
  if (isIntFamily(expected.base) && isFloatFamily(actual.base)) {
    double value = 0.0;
    if (!parseSimpleNumericLiteralValue(options.actualLiteralText, value))
      return false;
    return value == static_cast<double>(static_cast<long long>(value));
  }
  return true;
}

TypeRelationResult makeResult(TypeRelationKind kind,
                              TypeRelationWarningKind warningKind, int cost,
                              bool viable, const TypeDesc &expected,
                              const TypeDesc &actual,
                              const TypeRelationOptions &options) {
  TypeRelationResult out;
  out.kind = kind;
  out.warningKind =
      options.suppressWarnings ? TypeRelationWarningKind::None : warningKind;
  out.cost = cost;
  out.viable = viable;
  out.expectedType = canonicalTypeOrFallback(expected);
  out.actualType = canonicalTypeOrFallback(actual);
  return out;
}

TypeRelationWarningKind higherPriorityWarning(TypeRelationWarningKind a,
                                              TypeRelationWarningKind b) {
  return typeRelationWarningPriority(a) <= typeRelationWarningPriority(b) ? a
                                                                         : b;
}

TypeRelationWarningKind scalarConversionWarning(
    const TypeDesc &expected, const TypeDesc &actual,
    const TypeRelationOptions &options) {
  if (options.suppressWarnings)
    return TypeRelationWarningKind::None;
  if (literalIsSafelyAdaptable(expected, actual, options))
    return TypeRelationWarningKind::None;
  if (expected.base == actual.base)
    return TypeRelationWarningKind::None;
  if (expected.base == "bool" || actual.base == "bool")
    return TypeRelationWarningKind::Boolean;
  if ((isFloatFamily(expected.base) && isIntFamily(actual.base)) ||
      (isIntFamily(expected.base) && isFloatFamily(actual.base))) {
    if (isFloatFamily(expected.base) && isIntFamily(actual.base) &&
        expected.base != "half")
      return TypeRelationWarningKind::None;
    return TypeRelationWarningKind::FloatingIntegral;
  }
  if (isIntFamily(expected.base) && isIntFamily(actual.base) &&
      expected.base != actual.base) {
    if (options.actualIsNumericLiteral)
      return TypeRelationWarningKind::None;
    return TypeRelationWarningKind::Signedness;
  }
  if (isFloatFamily(expected.base) && isFloatFamily(actual.base)) {
    const int eRank = floatRank(expected.base);
    const int aRank = floatRank(actual.base);
    if (eRank >= 0 && aRank >= 0 && aRank > eRank)
      return TypeRelationWarningKind::Narrowing;
  }
  return TypeRelationWarningKind::None;
}

bool isNumericShape(const TypeDesc &type) {
  return (type.kind == TypeDescKind::Scalar || type.kind == TypeDescKind::Vector ||
          type.kind == TypeDescKind::Matrix) &&
         isNumericScalarBase(type.base);
}

bool isBooleanShape(const TypeDesc &type) {
  return (type.kind == TypeDescKind::Scalar || type.kind == TypeDescKind::Vector) &&
         type.base == "bool";
}

bool sameShapeOrTruncatable(const TypeDesc &expected, const TypeDesc &actual,
                            bool &truncationOut) {
  truncationOut = false;
  if (expected.kind == TypeDescKind::Scalar) {
    if (actual.kind == TypeDescKind::Scalar)
      return true;
    if (actual.kind == TypeDescKind::Vector || actual.kind == TypeDescKind::Matrix) {
      truncationOut = true;
      return true;
    }
    return false;
  }
  if (expected.kind == TypeDescKind::Vector) {
    if (actual.kind == TypeDescKind::Scalar)
      return true;
    if (actual.kind == TypeDescKind::Vector) {
      if (expected.rows == actual.rows)
        return true;
      if (expected.rows < actual.rows) {
        truncationOut = true;
        return true;
      }
      return false;
    }
    if (actual.kind == TypeDescKind::Matrix) {
      const int actualComponents = actual.rows * actual.cols;
      if (expected.rows <= actualComponents) {
        truncationOut = true;
        return true;
      }
    }
    return false;
  }
  if (expected.kind == TypeDescKind::Matrix) {
    if (actual.kind == TypeDescKind::Scalar)
      return true;
    if (actual.kind != TypeDescKind::Matrix)
      return false;
    if (expected.rows == actual.rows && expected.cols == actual.cols)
      return true;
    if (expected.rows <= actual.rows && expected.cols <= actual.cols) {
      truncationOut = true;
      return true;
    }
    return false;
  }
  return false;
}

std::string commonNumericBase(const std::string &left,
                              const std::string &right) {
  const int leftFloat = floatRank(left);
  const int rightFloat = floatRank(right);
  if (leftFloat >= 0 || rightFloat >= 0) {
    const int rank = std::max(leftFloat, rightFloat);
    if (rank >= 2)
      return "double";
    if (rank == 1)
      return "float";
    return "half";
  }
  const int rank = std::max(intRank(left), intRank(right));
  if (rank >= 1)
    return (isUnsignedIntFamily(left) || isUnsignedIntFamily(right))
               ? "uint64_t"
               : "int64_t";
  if (left == "uint" || right == "uint")
    return "uint";
  return "int";
}
} // namespace

TypeRelationResult evaluateTypeRelation(const TypeDesc &expected,
                                        const TypeDesc &actual,
                                        bool allowNarrowing) {
  TypeRelationOptions options;
  TypeRelationResult result =
      evaluateTypeRelationWithOptions(expected, actual, options);
  if (!allowNarrowing && result.kind == TypeRelationKind::Narrowing) {
    return result;
  }
  return result;
}

TypeRelationResult
evaluateTypeRelationWithOptions(const TypeDesc &expected,
                                const TypeDesc &actual,
                                const TypeRelationOptions &options) {
  if (expected.kind == TypeDescKind::Unknown ||
      actual.kind == TypeDescKind::Unknown) {
    return makeResult(TypeRelationKind::Illegal, TypeRelationWarningKind::None,
                      kIllegalCost, false, expected, actual, options);
  }

  if (expected.kind == TypeDescKind::Object ||
      actual.kind == TypeDescKind::Object) {
    if (expected.kind != TypeDescKind::Object ||
        actual.kind != TypeDescKind::Object) {
      return makeResult(TypeRelationKind::Illegal, TypeRelationWarningKind::None,
                        kIllegalCost, false, expected, actual, options);
    }
    if (expected.base == actual.base && !expected.base.empty()) {
      return makeResult(TypeRelationKind::Exact, TypeRelationWarningKind::None,
                        0, true, expected, actual, options);
    }
    if (sameObjectFamily(expected, actual)) {
      return makeResult(TypeRelationKind::ObjectFamilyConversion,
                        TypeRelationWarningKind::None, 3, true, expected,
                        actual, options);
    }
    return makeResult(TypeRelationKind::Illegal, TypeRelationWarningKind::None,
                      kIllegalCost, false, expected, actual, options);
  }

  if ((!isNumericShape(expected) && !isBooleanShape(expected)) ||
      (!isNumericShape(actual) && !isBooleanShape(actual))) {
    return makeResult(TypeRelationKind::Illegal, TypeRelationWarningKind::None,
                      kIllegalCost, false, expected, actual, options);
  }

  bool truncation = false;
  if (!sameShapeOrTruncatable(expected, actual, truncation)) {
    return makeResult(TypeRelationKind::Illegal, TypeRelationWarningKind::None,
                      kIllegalCost, false, expected, actual, options);
  }

  TypeRelationWarningKind warning =
      scalarConversionWarning(expected, actual, options);
  TypeRelationKind kind = TypeRelationKind::NumericConversion;
  int cost = 4;

  if (expected.kind == actual.kind && expected.rows == actual.rows &&
      expected.cols == actual.cols && expected.base == actual.base) {
    kind = TypeRelationKind::Exact;
    cost = 0;
    warning = TypeRelationWarningKind::None;
  } else if (actual.kind == TypeDescKind::Scalar &&
             expected.kind != TypeDescKind::Scalar) {
    kind = TypeRelationKind::ScalarSplat;
    cost = 2;
  } else if (truncation) {
    kind = TypeRelationKind::Truncation;
    cost = 12;
    warning = TypeRelationWarningKind::Truncation;
  } else if (isFloatFamily(expected.base) && isFloatFamily(actual.base)) {
    const int eRank = floatRank(expected.base);
    const int aRank = floatRank(actual.base);
    if (aRank <= eRank) {
      kind = TypeRelationKind::WidenPromotion;
      cost = 1;
    } else {
      kind = TypeRelationKind::Narrowing;
      cost = 9;
    }
  } else if (expected.base == "bool" || actual.base == "bool") {
    kind = TypeRelationKind::NumericConversion;
    cost = 7;
  } else if (isIntFamily(expected.base) && isIntFamily(actual.base)) {
    kind = TypeRelationKind::NumericConversion;
    cost = expected.base == actual.base ? 0 : 5;
  } else {
    kind = TypeRelationKind::NumericConversion;
    cost = 4;
  }

  if (kind == TypeRelationKind::ScalarSplat &&
      warning == TypeRelationWarningKind::None &&
      isFloatFamily(expected.base) && isFloatFamily(actual.base) &&
      floatRank(actual.base) > floatRank(expected.base)) {
    warning = TypeRelationWarningKind::Narrowing;
  }

  return makeResult(kind, warning, cost, true, expected, actual, options);
}

TypeBinaryConversionResult
evaluateUsualArithmeticConversion(const TypeDesc &left,
                                  const TypeDesc &right) {
  TypeBinaryConversionResult result;
  if ((!isNumericShape(left) && !isBooleanShape(left)) ||
      (!isNumericShape(right) && !isBooleanShape(right))) {
    return result;
  }

  TypeDesc out;
  out.kind = TypeDescKind::Scalar;
  out.rows = 1;
  out.cols = 1;
  out.base = commonNumericBase(left.base, right.base);

  if (left.kind == TypeDescKind::Matrix || right.kind == TypeDescKind::Matrix) {
    if (left.kind == TypeDescKind::Matrix && right.kind == TypeDescKind::Matrix) {
      out.kind = TypeDescKind::Matrix;
      out.rows = std::min(left.rows, right.rows);
      out.cols = std::min(left.cols, right.cols);
    } else if (left.kind == TypeDescKind::Matrix) {
      out.kind = TypeDescKind::Matrix;
      out.rows = left.rows;
      out.cols = left.cols;
    } else {
      out.kind = TypeDescKind::Matrix;
      out.rows = right.rows;
      out.cols = right.cols;
    }
  } else if (left.kind == TypeDescKind::Vector || right.kind == TypeDescKind::Vector) {
    out.kind = TypeDescKind::Vector;
    if (left.kind == TypeDescKind::Vector && right.kind == TypeDescKind::Vector)
      out.rows = std::min(left.rows, right.rows);
    else
      out.rows = left.kind == TypeDescKind::Vector ? left.rows : right.rows;
    out.cols = 1;
  }

  result.leftConversion =
      evaluateTypeRelationWithOptions(out, left, TypeRelationOptions{});
  result.rightConversion =
      evaluateTypeRelationWithOptions(out, right, TypeRelationOptions{});
  result.viable = result.leftConversion.viable && result.rightConversion.viable;
  if (result.viable)
    result.resultType = typeDescToCanonicalString(out);
  return result;
}

const char *typeRelationKindToString(TypeRelationKind kind) {
  switch (kind) {
  case TypeRelationKind::Exact:
    return "exact";
  case TypeRelationKind::ScalarSplat:
    return "scalar_splat";
  case TypeRelationKind::WidenPromotion:
    return "widen_promotion";
  case TypeRelationKind::NumericConversion:
    return "numeric_conversion";
  case TypeRelationKind::ObjectFamilyConversion:
    return "object_family_conversion";
  case TypeRelationKind::Narrowing:
    return "narrowing";
  case TypeRelationKind::Truncation:
    return "truncation";
  case TypeRelationKind::Illegal:
    return "illegal";
  }
  return "illegal";
}

const char *typeRelationWarningKindToString(TypeRelationWarningKind kind) {
  switch (kind) {
  case TypeRelationWarningKind::None:
    return "none";
  case TypeRelationWarningKind::Truncation:
    return "truncation";
  case TypeRelationWarningKind::Boolean:
    return "boolean";
  case TypeRelationWarningKind::FloatingIntegral:
    return "floating_integral";
  case TypeRelationWarningKind::Signedness:
    return "signedness";
  case TypeRelationWarningKind::Narrowing:
    return "narrowing";
  }
  return "none";
}

int typeRelationWarningPriority(TypeRelationWarningKind kind) {
  switch (kind) {
  case TypeRelationWarningKind::Truncation:
    return 1;
  case TypeRelationWarningKind::Boolean:
    return 2;
  case TypeRelationWarningKind::FloatingIntegral:
    return 3;
  case TypeRelationWarningKind::Signedness:
    return 4;
  case TypeRelationWarningKind::Narrowing:
    return 5;
  case TypeRelationWarningKind::None:
    return 100;
  }
  return 100;
}

std::string typeRelationWarningMessage(const TypeRelationResult &relation) {
  if (relation.warningKind == TypeRelationWarningKind::None)
    return "";
  std::ostringstream oss;
  switch (relation.warningKind) {
  case TypeRelationWarningKind::Truncation:
    oss << "Implicit truncation conversion: ";
    break;
  case TypeRelationWarningKind::Boolean:
    oss << "Implicit boolean conversion: ";
    break;
  case TypeRelationWarningKind::FloatingIntegral:
    oss << "Implicit floating-integral conversion: ";
    break;
  case TypeRelationWarningKind::Signedness:
    oss << "Implicit signedness conversion: ";
    break;
  case TypeRelationWarningKind::Narrowing:
    oss << "Implicit narrowing conversion: ";
    break;
  case TypeRelationWarningKind::None:
    break;
  }
  oss << relation.actualType << " -> " << relation.expectedType
      << ". Use an explicit cast";
  if (relation.warningKind == TypeRelationWarningKind::Truncation)
    oss << " or swizzle";
  oss << " if this is intentional.";
  return oss.str();
}
