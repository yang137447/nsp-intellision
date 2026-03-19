#include "type_eval.hpp"

const char *typeEvalConfidenceToString(TypeEvalConfidence confidence) {
  switch (confidence) {
  case TypeEvalConfidence::L1:
    return "L1";
  case TypeEvalConfidence::L2:
    return "L2";
  case TypeEvalConfidence::L3:
    return "L3";
  }
  return "L3";
}
