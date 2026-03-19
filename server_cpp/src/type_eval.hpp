#pragma once

#include <string>

enum class TypeEvalConfidence { L1, L2, L3 };

struct TypeEvalResult {
  std::string type;
  TypeEvalConfidence confidence = TypeEvalConfidence::L1;
  std::string reasonCode;
};

const char *typeEvalConfidenceToString(TypeEvalConfidence confidence);
