#include "diagnostics_expression_type.hpp"
#include "diagnostics_semantic_common.hpp"


#include "callsite_parser.hpp"
#include "diagnostics_emit.hpp"
#include "diagnostics_indeterminate.hpp"
#include "diagnostics_io.hpp"
#include "diagnostics_preprocessor.hpp"
#include "diagnostics_syntax.hpp"
#include "fast_ast.hpp"
#include "full_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_markdown.hpp"
#include "include_resolver.hpp"
#include "indeterminate_reasons.hpp"
#include "language_registry.hpp"
#include "lsp_helpers.hpp"
#include "macro_generated_functions.hpp"
#include "nsf_lexer.hpp"
#include "overload_resolver.hpp"
#include "preprocessor_view.hpp"
#include "semantic_cache.hpp"
#include "semantic_snapshot.hpp"
#include "server_parse.hpp"
#include "text_utils.hpp"
#include "type_desc.hpp"
#include "type_eval.hpp"
#include "type_model.hpp"
#include "type_relation.hpp"
#include "uri_utils.hpp"
#include "workspace_summary_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

static bool isWhitespace(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) != 0;
}


std::string normalizeTypeToken(std::string value);
bool isVectorType(const std::string &type, int &dimensionOut);
bool isScalarType(const std::string &type);
bool isMatrixType(const std::string &type, std::string &scalarOut,
                         int &rowsOut, int &colsOut);
std::string makeVectorOrScalarType(const std::string &scalar, int dim);
std::string makeMatrixType(const std::string &scalar, int rows,
                                  int cols);

static BuiltinElemKind parseBuiltinElemKind(const std::string &t) {
  if (t == "bool")
    return BuiltinElemKind::Bool;
  if (t == "int")
    return BuiltinElemKind::Int;
  if (t == "uint")
    return BuiltinElemKind::UInt;
  if (t == "half")
    return BuiltinElemKind::Half;
  if (t == "float")
    return BuiltinElemKind::Float;
  if (t == "double")
    return BuiltinElemKind::Double;
  return BuiltinElemKind::Unknown;
}

static std::string builtinElemKindToString(BuiltinElemKind k) {
  if (k == BuiltinElemKind::Bool)
    return "bool";
  if (k == BuiltinElemKind::Int)
    return "int";
  if (k == BuiltinElemKind::UInt)
    return "uint";
  if (k == BuiltinElemKind::Half)
    return "half";
  if (k == BuiltinElemKind::Float)
    return "float";
  if (k == BuiltinElemKind::Double)
    return "double";
  return "";
}

BuiltinTypeInfo parseBuiltinTypeInfo(std::string type) {
  TypeDesc desc = parseTypeDesc(type);
  std::string canonicalDesc = typeDescToCanonicalString(desc);
  type = canonicalDesc.empty() ? normalizeTypeToken(type) : canonicalDesc;
  BuiltinTypeInfo out;
  if (type.empty())
    return out;

  int dim = 0;
  if (isScalarType(type)) {
    out.shape = BuiltinTypeInfo::ShapeKind::Scalar;
    out.elem = parseBuiltinElemKind(type);
    out.dim = 1;
    return out;
  }
  if (isVectorType(type, dim)) {
    std::string base = type.substr(0, type.size() - 1);
    out.shape = BuiltinTypeInfo::ShapeKind::Vector;
    out.elem = parseBuiltinElemKind(base);
    out.dim = dim;
    return out;
  }
  std::string scalar;
  int rows = 0;
  int cols = 0;
  if (isMatrixType(type, scalar, rows, cols)) {
    out.shape = BuiltinTypeInfo::ShapeKind::Matrix;
    out.elem = parseBuiltinElemKind(scalar);
    out.rows = rows;
    out.cols = cols;
    return out;
  }
  return out;
}

static std::string builtinTypeInfoToString(const BuiltinTypeInfo &t) {
  const std::string base = builtinElemKindToString(t.elem);
  if (base.empty())
    return "";
  if (t.shape == BuiltinTypeInfo::ShapeKind::Scalar)
    return base;
  if (t.shape == BuiltinTypeInfo::ShapeKind::Vector)
    return makeVectorOrScalarType(base, t.dim);
  if (t.shape == BuiltinTypeInfo::ShapeKind::Matrix)
    return makeMatrixType(base, t.rows, t.cols);
  return "";
}

static TypeDesc builtinInfoToTypeDesc(const BuiltinTypeInfo &t) {
  return parseTypeDesc(builtinTypeInfoToString(t));
}

static bool isBuiltinNumericElem(BuiltinElemKind k) {
  return k == BuiltinElemKind::Int || k == BuiltinElemKind::UInt ||
         k == BuiltinElemKind::Half || k == BuiltinElemKind::Float ||
         k == BuiltinElemKind::Double;
}

static bool isBuiltinUnarySameType(const std::string &name) {
  return name == "normalize" || name == "saturate" || name == "abs" ||
         name == "sin" || name == "cos" || name == "tan" || name == "asin" ||
         name == "acos" || name == "atan" || name == "exp" || name == "exp2" ||
         name == "floor" || name == "ceil" || name == "frac" ||
         name == "fmod" || name == "rsqrt" || name == "sqrt" || name == "sign";
}

BuiltinResolveResult
resolveBuiltinCall(const std::string &name,
                   const std::vector<BuiltinTypeInfo> &args) {
  BuiltinResolveResult r;
  if (args.empty())
    return r;
  for (const auto &a : args) {
    if (a.shape == BuiltinTypeInfo::ShapeKind::Unknown ||
        a.elem == BuiltinElemKind::Unknown) {
      r.indeterminate = true;
      return r;
    }
  }

  auto exactShapeEq = [&](const BuiltinTypeInfo &a,
                          const BuiltinTypeInfo &b) -> bool {
    if (a.shape != b.shape)
      return false;
    if (a.shape == BuiltinTypeInfo::ShapeKind::Vector)
      return a.dim == b.dim;
    if (a.shape == BuiltinTypeInfo::ShapeKind::Matrix)
      return a.rows == b.rows && a.cols == b.cols;
    return true;
  };
  auto scalarOrExactMatch = [&](const BuiltinTypeInfo &a,
                                const BuiltinTypeInfo &ref) -> bool {
    if (a.shape == BuiltinTypeInfo::ShapeKind::Scalar)
      return true;
    return exactShapeEq(a, ref);
  };
  auto scalarInfoForElem = [](BuiltinElemKind elem) {
    BuiltinTypeInfo out;
    out.shape = BuiltinTypeInfo::ShapeKind::Scalar;
    out.elem = elem;
    out.dim = 1;
    return out;
  };
  auto usualConversion = [&](const BuiltinTypeInfo &a,
                             const BuiltinTypeInfo &b,
                             BuiltinTypeInfo &out) -> bool {
    TypeBinaryConversionResult conversion = evaluateUsualArithmeticConversion(
        builtinInfoToTypeDesc(a), builtinInfoToTypeDesc(b));
    if (!conversion.viable)
      return false;
    out = parseBuiltinTypeInfo(conversion.resultType);
    if (out.shape == BuiltinTypeInfo::ShapeKind::Unknown ||
        out.elem == BuiltinElemKind::Unknown)
      return false;
    r.conversions.push_back(conversion.leftConversion);
    r.conversions.push_back(conversion.rightConversion);
    return true;
  };
  auto unifyElem = [&](BuiltinElemKind &outElem) -> bool {
    BuiltinTypeInfo current = scalarInfoForElem(args[0].elem);
    for (size_t i = 1; i < args.size(); i++) {
      BuiltinTypeInfo next = scalarInfoForElem(args[i].elem);
      BuiltinTypeInfo converted;
      if (!usualConversion(current, next, converted))
        return false;
      current = converted;
    }
    outElem = current.elem;
    return outElem != BuiltinElemKind::Unknown;
  };

  if (name == "length" || name == "distance") {
    if (args.size() < 1)
      return r;
    if (!isBuiltinNumericElem(args[0].elem))
      return r;
    if (name == "length") {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Scalar;
      r.ret.elem = args[0].elem;
      r.ret.dim = 1;
      return r;
    } else {
      if (args.size() < 2)
        return r;
      if (!isBuiltinNumericElem(args[1].elem))
        return r;
      BuiltinTypeInfo converted;
      if (!usualConversion(args[0], args[1], converted))
        return r;
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Scalar;
      r.ret.elem = converted.elem;
      r.ret.dim = 1;
      return r;
    }
  }

  if (name == "dot") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    BuiltinTypeInfo converted;
    if (!usualConversion(args[0], args[1], converted))
      return r;
    r.ok = true;
    r.ret.shape = BuiltinTypeInfo::ShapeKind::Scalar;
    r.ret.elem = converted.elem;
    r.ret.dim = 1;
    return r;
  }

  if (name == "cross") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    if (args[0].shape != BuiltinTypeInfo::ShapeKind::Vector ||
        args[1].shape != BuiltinTypeInfo::ShapeKind::Vector)
      return r;
    if (args[0].dim != 3 || args[1].dim != 3)
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret.shape = BuiltinTypeInfo::ShapeKind::Vector;
    r.ret.elem = outElem;
    r.ret.dim = 3;
    return r;
  }

  if (isBuiltinUnarySameType(name)) {
    if (args.size() < 1)
      return r;
    if (!isBuiltinNumericElem(args[0].elem))
      return r;
    r.ok = true;
    r.ret = args[0];
    return r;
  }

  if (name == "asfloat" || name == "asint" || name == "asuint") {
    if (args.size() != 1)
      return r;
    if (args[0].elem != BuiltinElemKind::Float &&
        args[0].elem != BuiltinElemKind::Half &&
        args[0].elem != BuiltinElemKind::Int &&
        args[0].elem != BuiltinElemKind::UInt) {
      return r;
    }
    r.ok = true;
    r.ret = args[0];
    if (name == "asfloat")
      r.ret.elem = BuiltinElemKind::Float;
    else if (name == "asint")
      r.ret.elem = BuiltinElemKind::Int;
    else if (name == "asuint")
      r.ret.elem = BuiltinElemKind::UInt;
    return r;
  }

  if (name == "countbits" || name == "firstbithigh" || name == "firstbitlow" ||
      name == "reversebits") {
    if (args.size() != 1)
      return r;
    if (args[0].elem != BuiltinElemKind::Int &&
        args[0].elem != BuiltinElemKind::UInt) {
      return r;
    }
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = BuiltinElemKind::Int;
    return r;
  }

  if (name == "f16tof32") {
    if (args.size() != 1)
      return r;
    if (args[0].elem != BuiltinElemKind::UInt)
      return r;
    if (args[0].shape == BuiltinTypeInfo::ShapeKind::Matrix)
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = BuiltinElemKind::Float;
    return r;
  }

  if (name == "f32tof16") {
    if (args.size() != 1)
      return r;
    if (!isBuiltinNumericElem(args[0].elem))
      return r;
    if (args[0].shape == BuiltinTypeInfo::ShapeKind::Matrix)
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = BuiltinElemKind::UInt;
    return r;
  }

  if (name == "rcp") {
    if (args.size() != 1)
      return r;
    if (!isBuiltinNumericElem(args[0].elem))
      return r;
    r.ok = true;
    r.ret = args[0];
    if (r.ret.elem != BuiltinElemKind::Half &&
        r.ret.elem != BuiltinElemKind::Float) {
      r.ret.elem = BuiltinElemKind::Float;
    }
    return r;
  }

  if (name == "mad") {
    if (args.size() != 3)
      return r;
    for (const auto &a : args) {
      if (!isBuiltinNumericElem(a.elem))
        return r;
    }
    BuiltinTypeInfo ref = args[0];
    if (args[1].shape != BuiltinTypeInfo::ShapeKind::Scalar)
      ref = args[1];
    if (args[2].shape != BuiltinTypeInfo::ShapeKind::Scalar)
      ref = args[2];
    if (!scalarOrExactMatch(args[0], ref) ||
        !scalarOrExactMatch(args[1], ref) || !scalarOrExactMatch(args[2], ref))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = ref;
    r.ret.elem = outElem;
    return r;
  }

  if (name == "pow") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    if (!exactShapeEq(args[0], args[1]) &&
        !(args[0].shape == BuiltinTypeInfo::ShapeKind::Vector &&
          args[1].shape == BuiltinTypeInfo::ShapeKind::Scalar) &&
        !(args[1].shape == BuiltinTypeInfo::ShapeKind::Vector &&
          args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret =
        args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar ? args[1] : args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "atan2") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    if (!exactShapeEq(args[0], args[1]) &&
        !(args[0].shape == BuiltinTypeInfo::ShapeKind::Vector &&
          args[1].shape == BuiltinTypeInfo::ShapeKind::Scalar) &&
        !(args[1].shape == BuiltinTypeInfo::ShapeKind::Vector &&
          args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret =
        args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar ? args[1] : args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "reflect") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    if (!exactShapeEq(args[0], args[1]))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "refract") {
    if (args.size() < 3)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem) ||
        !isBuiltinNumericElem(args[2].elem))
      return r;
    if (!exactShapeEq(args[0], args[1]))
      return r;
    if (args[2].shape != BuiltinTypeInfo::ShapeKind::Scalar)
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "clamp") {
    if (args.size() < 3)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem) ||
        !isBuiltinNumericElem(args[2].elem))
      return r;
    if (!scalarOrExactMatch(args[1], args[0]))
      return r;
    if (!scalarOrExactMatch(args[2], args[0]))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "lerp") {
    if (args.size() < 3)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem) ||
        !isBuiltinNumericElem(args[2].elem))
      return r;
    BuiltinTypeInfo xyType;
    if (!usualConversion(args[0], args[1], xyType))
      return r;
    BuiltinTypeInfo resultType;
    if (!usualConversion(xyType, args[2], resultType))
      return r;
    r.ok = true;
    r.ret = resultType;
    return r;
  }

  if (name == "step") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    if (!scalarOrExactMatch(args[0], args[1]))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[1];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "smoothstep") {
    if (args.size() < 3)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem) ||
        !isBuiltinNumericElem(args[2].elem))
      return r;
    if (!scalarOrExactMatch(args[0], args[2]))
      return r;
    if (!scalarOrExactMatch(args[1], args[2]))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[2];
    r.ret.elem = outElem;
    return r;
  }

  if (name == "min" || name == "max") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    BuiltinTypeInfo converted;
    if (!usualConversion(args[0], args[1], converted))
      return r;
    r.ok = true;
    r.ret = converted;
    return r;
  }

  if (name == "mul" && args.size() >= 2) {
    const auto &a = args[0];
    const auto &b = args[1];
    if (!isBuiltinNumericElem(a.elem) || !isBuiltinNumericElem(b.elem))
      return r;
    BuiltinTypeInfo converted;
    if (!usualConversion(scalarInfoForElem(a.elem), scalarInfoForElem(b.elem),
                         converted))
      return r;
    BuiltinElemKind outElem = converted.elem;
    if (outElem == BuiltinElemKind::Unknown)
      return r;

    if (a.shape == BuiltinTypeInfo::ShapeKind::Matrix &&
        b.shape == BuiltinTypeInfo::ShapeKind::Matrix) {
      if (a.cols != b.rows)
        return r;
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Matrix;
      r.ret.elem = outElem;
      r.ret.rows = a.rows;
      r.ret.cols = b.cols;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Vector &&
        b.shape == BuiltinTypeInfo::ShapeKind::Matrix) {
      if (a.dim != b.rows)
        return r;
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Vector;
      r.ret.elem = outElem;
      r.ret.dim = b.cols;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Matrix &&
        b.shape == BuiltinTypeInfo::ShapeKind::Vector) {
      if (a.cols != b.dim)
        return r;
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Vector;
      r.ret.elem = outElem;
      r.ret.dim = a.rows;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Matrix &&
        b.shape == BuiltinTypeInfo::ShapeKind::Scalar) {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Matrix;
      r.ret.elem = outElem;
      r.ret.rows = a.rows;
      r.ret.cols = a.cols;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Scalar &&
        b.shape == BuiltinTypeInfo::ShapeKind::Matrix) {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Matrix;
      r.ret.elem = outElem;
      r.ret.rows = b.rows;
      r.ret.cols = b.cols;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Vector &&
        b.shape == BuiltinTypeInfo::ShapeKind::Scalar) {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Vector;
      r.ret.elem = outElem;
      r.ret.dim = a.dim;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Scalar &&
        b.shape == BuiltinTypeInfo::ShapeKind::Vector) {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Vector;
      r.ret.elem = outElem;
      r.ret.dim = b.dim;
      return r;
    }
    if (a.shape == BuiltinTypeInfo::ShapeKind::Scalar &&
        b.shape == BuiltinTypeInfo::ShapeKind::Scalar) {
      r.ok = true;
      r.ret.shape = BuiltinTypeInfo::ShapeKind::Scalar;
      r.ret.elem = outElem;
      r.ret.dim = 1;
      return r;
    }
  }

  return r;
}

std::string normalizeTypeToken(std::string value) {
  while (!value.empty() && isWhitespace(value.front()))
    value.erase(value.begin());
  while (!value.empty() && isWhitespace(value.back()))
    value.pop_back();
  return value;
}

bool isVectorType(const std::string &type, int &dimensionOut) {
  if (type.size() < 2)
    return false;
  size_t digitPos = type.size() - 1;
  if (!std::isdigit(static_cast<unsigned char>(type[digitPos])))
    return false;
  int dim = type[digitPos] - '0';
  if (dim < 2 || dim > 4)
    return false;
  std::string base = type.substr(0, digitPos);
  if (base == "float" || base == "half" || base == "double" ||
      base == "int" || base == "uint" || base == "bool") {
    dimensionOut = dim;
    return true;
  }
  return false;
}

bool isScalarType(const std::string &type) {
  return type == "float" || type == "half" || type == "double" ||
         type == "int" || type == "uint" || type == "bool";
}

bool isNumericScalarType(const std::string &type) {
  return type == "float" || type == "half" || type == "double" ||
         type == "int" || type == "uint";
}

bool isMatrixType(const std::string &type, std::string &scalarOut,
                         int &rowsOut, int &colsOut) {
  scalarOut.clear();
  rowsOut = 0;
  colsOut = 0;
  if (type.size() < 4)
    return false;
  size_t xPos = type.find('x');
  if (xPos == std::string::npos)
    return false;
  if (xPos == 0 || xPos + 1 >= type.size())
    return false;
  if (!std::isdigit(static_cast<unsigned char>(type[xPos - 1])) ||
      !std::isdigit(static_cast<unsigned char>(type[xPos + 1])))
    return false;
  int rows = type[xPos - 1] - '0';
  int cols = type[xPos + 1] - '0';
  if (rows < 1 || rows > 4 || cols < 1 || cols > 4)
    return false;
  std::string base = type.substr(0, xPos - 1);
  if (base != "float" && base != "half" && base != "double" &&
      base != "int" && base != "uint")
    return false;
  if (xPos + 2 != type.size())
    return false;
  scalarOut = base;
  rowsOut = rows;
  colsOut = cols;
  return true;
}

bool isMatrixType(const std::string &type, int &rowsOut, int &colsOut) {
  std::string scalar;
  return isMatrixType(type, scalar, rowsOut, colsOut);
}

static bool isNumericMatrixType(const std::string &type) {
  std::string scalar;
  int r = 0;
  int c = 0;
  if (!isMatrixType(type, scalar, r, c))
    return false;
  return isNumericScalarType(scalar);
}

std::string makeMatrixType(const std::string &scalar, int rows,
                                  int cols) {
  if (scalar.empty() || rows <= 0 || cols <= 0)
    return "";
  return scalar + std::to_string(rows) + "x" + std::to_string(cols);
}

std::string inferLiteralType(const std::string &token) {
  if (token == "true" || token == "false")
    return "bool";
  if (token.empty())
    return "";
  std::vector<LexToken> tokens;
  tokens.push_back(
      LexToken{LexToken::Kind::Identifier, token, 0, token.size()});
  NumericLiteralParseResult numeric = parseNumericLiteralFromTokens(tokens, 0);
  if (numeric.matched && !numeric.type.empty())
    return numeric.type;
  return "";
}

static bool isSignedDigitsToken(const std::string &token) {
  if (token.empty())
    return false;
  size_t start = 0;
  if (token[0] == '+' || token[0] == '-')
    start = 1;
  if (start >= token.size())
    return false;
  for (size_t i = start; i < token.size(); i++) {
    if (!std::isdigit(static_cast<unsigned char>(token[i])))
      return false;
  }
  return true;
}

bool isSignedDigitsWithOptionalSuffixToken(const std::string &token,
                                           char &suffixOut) {
  suffixOut = 0;
  if (token.empty())
    return false;
  size_t start = 0;
  if (token[0] == '+' || token[0] == '-')
    start = 1;
  if (start >= token.size())
    return false;
  size_t i = start;
  while (i < token.size() &&
         std::isdigit(static_cast<unsigned char>(token[i]))) {
    i++;
  }
  if (i == start)
    return false;
  if (i == token.size())
    return true;
  if (i + 1 == token.size()) {
    char suf = token[i];
    if (suf == 'h' || suf == 'f' || suf == 'F' || suf == 'u' || suf == 'U') {
      suffixOut = suf;
      return true;
    }
  }
  return false;
}

bool isSignedDigitsWithSingleAlphaSuffixToken(const std::string &token,
                                              char &suffixOut) {
  suffixOut = 0;
  if (token.empty())
    return false;
  size_t start = 0;
  if (token[0] == '+' || token[0] == '-')
    start = 1;
  if (start >= token.size())
    return false;
  size_t i = start;
  while (i < token.size() &&
         std::isdigit(static_cast<unsigned char>(token[i]))) {
    i++;
  }
  if (i == start)
    return false;
  if (i + 1 != token.size())
    return false;
  unsigned char suf = static_cast<unsigned char>(token[i]);
  if (!std::isalpha(suf))
    return false;
  suffixOut = token[i];
  return true;
}

namespace {

struct NumericLiteralTextParseResult {
  bool matched = false;
  bool valid = false;
  std::string type;
  char invalidSuffix = 0;
  size_t invalidSuffixOffset = 0;
  std::string deprecatedSuffix;
  std::string recommendedSuffix;
  size_t deprecatedSuffixOffset = 0;
};

bool isFloatLiteralSuffix(char ch) {
  return ch == 'h' || ch == 'H' || ch == 'f' || ch == 'F' || ch == 'l' ||
         ch == 'L';
}

bool isUnsignedSuffix(char ch) { return ch == 'u' || ch == 'U'; }

bool isLongSuffix(char ch) { return ch == 'l' || ch == 'L'; }

bool isIntegerSuffixStart(char ch) {
  return isUnsignedSuffix(ch) || isLongSuffix(ch);
}

bool isDecimalDigit(char ch) {
  return std::isdigit(static_cast<unsigned char>(ch));
}

bool isOctalDigit(char ch) { return ch >= '0' && ch <= '7'; }

int diagnosticsNumericScalarRank(const std::string &scalar) {
  if (scalar == "half")
    return 0;
  if (scalar == "float")
    return 1;
  if (scalar == "double")
    return 2;
  return -1;
}

std::string mergeNumericScalar(const std::string &left,
                               const std::string &right) {
  if (left == right)
    return left;
  const int leftFloatRank = diagnosticsNumericScalarRank(left);
  const int rightFloatRank = diagnosticsNumericScalarRank(right);
  if (leftFloatRank >= 0 || rightFloatRank >= 0) {
    const int outRank = std::max(leftFloatRank, rightFloatRank);
    if (outRank >= 2)
      return "double";
    if (outRank == 1)
      return "float";
    return "half";
  }
  if (left == "uint" || right == "uint")
    return "uint";
  return "int";
}

std::string decimalLiteralType(bool hasFloatSyntax, char suffix) {
  if (suffix == 'h' || suffix == 'H')
    return "half";
  if (suffix == 'f' || suffix == 'F')
    return "float";
  if (suffix == 'l' || suffix == 'L')
    return hasFloatSyntax ? "double" : "int";
  return hasFloatSyntax ? "float" : "int";
}

bool parseIntegerSuffix(const std::string &value, size_t &i,
                        bool &isUnsigned, size_t &invalidOffset) {
  const size_t start = i;
  bool sawUnsigned = false;
  bool sawLong = false;
  if (i < value.size() && isUnsignedSuffix(value[i])) {
    sawUnsigned = true;
    i++;
  }
  if (i < value.size() && isLongSuffix(value[i])) {
    sawLong = true;
    i++;
  }
  if (!sawUnsigned && i < value.size() && isUnsignedSuffix(value[i])) {
    sawUnsigned = true;
    i++;
  }
  if (i < value.size() && isLongSuffix(value[i])) {
    invalidOffset = i;
    return false;
  }
  if (i < value.size() && isUnsignedSuffix(value[i])) {
    invalidOffset = i;
    return false;
  }
  if (i < value.size()) {
    invalidOffset = i;
    return false;
  }
  isUnsigned = sawUnsigned;
  return i > start || sawLong;
}

bool parseLegacyLongLongIntegerSuffix(const std::string &value, size_t &i,
                                      bool &isUnsigned,
                                      std::string &deprecatedSuffix,
                                      std::string &recommendedSuffix) {
  const size_t start = i;
  bool sawUnsigned = false;
  if (i < value.size() && isUnsignedSuffix(value[i])) {
    sawUnsigned = true;
    i++;
  }
  if (i + 1 >= value.size() || !isLongSuffix(value[i]) ||
      !isLongSuffix(value[i + 1]))
    return false;
  i += 2;
  if (!sawUnsigned && i < value.size() && isUnsignedSuffix(value[i])) {
    sawUnsigned = true;
    i++;
  }
  if (i != value.size())
    return false;
  isUnsigned = sawUnsigned;
  deprecatedSuffix = value.substr(start);
  recommendedSuffix = isUnsigned ? "ul" : "l";
  return true;
}

NumericLiteralTextParseResult parseNumericLiteralText(
    const std::string &value) {
  NumericLiteralTextParseResult result;
  if (value.empty())
    return result;

  size_t i = 0;
  if (value[i] == '+' || value[i] == '-')
    i++;
  if (i >= value.size())
    return result;

  if (std::isdigit(static_cast<unsigned char>(value[i])) &&
      i + 1 < value.size() && value[i] == '0' &&
      (value[i + 1] == 'x' || value[i + 1] == 'X')) {
    i += 2;
    const size_t digitsStart = i;
    while (i < value.size() &&
           std::isxdigit(static_cast<unsigned char>(value[i]))) {
      i++;
    }
    if (i == digitsStart)
      return result;

    result.matched = true;
    result.type = "int";
    if (i == value.size()) {
      result.valid = true;
      return result;
    }
    if (isIntegerSuffixStart(value[i])) {
      bool isUnsigned = false;
      size_t invalidOffset = 0;
      const size_t suffixOffset = i;
      std::string deprecatedSuffix;
      std::string recommendedSuffix;
      size_t legacyCursor = i;
      if (parseLegacyLongLongIntegerSuffix(value, legacyCursor, isUnsigned,
                                           deprecatedSuffix,
                                           recommendedSuffix)) {
        result.type = isUnsigned ? "uint" : "int";
        result.valid = true;
        result.deprecatedSuffix = deprecatedSuffix;
        result.recommendedSuffix = recommendedSuffix;
        result.deprecatedSuffixOffset = suffixOffset;
        return result;
      }
      if (parseIntegerSuffix(value, i, isUnsigned, invalidOffset)) {
        result.type = isUnsigned ? "uint" : "int";
        result.valid = true;
        return result;
      }
      result.type = isUnsigned ? "uint" : "int";
      result.invalidSuffix = value[invalidOffset];
      result.invalidSuffixOffset = invalidOffset;
      return result;
    }
    if (std::isalpha(static_cast<unsigned char>(value[i]))) {
      result.invalidSuffix = value[i];
      result.invalidSuffixOffset = i;
      return result;
    }
    result.invalidSuffix = value[i];
    result.invalidSuffixOffset = i;
    return result;
  }

  if (value[i] == '0' && i + 1 < value.size() &&
      isDecimalDigit(value[i + 1])) {
    i++;
    while (i < value.size() && isOctalDigit(value[i])) {
      i++;
    }

    result.matched = true;
    result.type = "int";
    if (i < value.size() && isDecimalDigit(value[i])) {
      result.invalidSuffix = value[i];
      result.invalidSuffixOffset = i;
      return result;
    }
    if (i == value.size()) {
      result.valid = true;
      return result;
    }
    if (isIntegerSuffixStart(value[i])) {
      bool isUnsigned = false;
      size_t invalidOffset = 0;
      const size_t suffixOffset = i;
      std::string deprecatedSuffix;
      std::string recommendedSuffix;
      size_t legacyCursor = i;
      if (parseLegacyLongLongIntegerSuffix(value, legacyCursor, isUnsigned,
                                           deprecatedSuffix,
                                           recommendedSuffix)) {
        result.type = isUnsigned ? "uint" : "int";
        result.valid = true;
        result.deprecatedSuffix = deprecatedSuffix;
        result.recommendedSuffix = recommendedSuffix;
        result.deprecatedSuffixOffset = suffixOffset;
        return result;
      }
      if (parseIntegerSuffix(value, i, isUnsigned, invalidOffset)) {
        result.type = isUnsigned ? "uint" : "int";
        result.valid = true;
        return result;
      }
      result.type = isUnsigned ? "uint" : "int";
      result.invalidSuffix = value[invalidOffset];
      result.invalidSuffixOffset = invalidOffset;
      return result;
    }
    if (std::isalpha(static_cast<unsigned char>(value[i]))) {
      result.invalidSuffix = value[i];
      result.invalidSuffixOffset = i;
      return result;
    }
    result.invalidSuffix = value[i];
    result.invalidSuffixOffset = i;
    return result;
  }

  const size_t digitsStart = i;
  while (i < value.size() &&
         std::isdigit(static_cast<unsigned char>(value[i]))) {
    i++;
  }
  const bool hasWholeDigits = i > digitsStart;

  bool hasDot = false;
  bool hasFractionDigits = false;
  if (i < value.size() && value[i] == '.') {
    hasDot = true;
    i++;
    const size_t fractionDigitsStart = i;
    while (i < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[i]))) {
      i++;
    }
    hasFractionDigits = i > fractionDigitsStart;
  }
  if (!hasWholeDigits && !hasFractionDigits)
    return result;

  bool hasExponent = false;
  size_t exponentOffset = std::string::npos;
  if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
    hasExponent = true;
    exponentOffset = i;
    i++;
    if (i < value.size() && (value[i] == '+' || value[i] == '-'))
      i++;
    const size_t exponentDigitsStart = i;
    while (i < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[i]))) {
      i++;
    }
    if (i == exponentDigitsStart) {
      result.matched = true;
      result.type = hasDot ? "float" : "int";
      result.invalidSuffix = value[exponentOffset];
      result.invalidSuffixOffset = exponentOffset;
      return result;
    }
  }

  const bool hasFloatSyntax = hasDot || hasExponent;
  if (i < value.size()) {
    if (!std::isalpha(static_cast<unsigned char>(value[i]))) {
      result.matched = true;
      result.type = decimalLiteralType(hasFloatSyntax, 0);
      result.invalidSuffix = value[i];
      result.invalidSuffixOffset = i;
      return result;
    }

    result.matched = true;
    if (hasFloatSyntax) {
      char suffix = value[i];
      result.type = decimalLiteralType(hasFloatSyntax, suffix);
      if (!isFloatLiteralSuffix(suffix)) {
        result.invalidSuffix = suffix;
        result.invalidSuffixOffset = i;
        return result;
      }
      i++;
      if (i != value.size()) {
        result.invalidSuffix = value[i];
        result.invalidSuffixOffset = i;
        return result;
      }
      result.valid = true;
      return result;
    }

    if (isIntegerSuffixStart(value[i])) {
      bool isUnsigned = false;
      size_t invalidOffset = 0;
      const size_t suffixOffset = i;
      std::string deprecatedSuffix;
      std::string recommendedSuffix;
      size_t legacyCursor = i;
      if (parseLegacyLongLongIntegerSuffix(value, legacyCursor, isUnsigned,
                                           deprecatedSuffix,
                                           recommendedSuffix)) {
        result.type = isUnsigned ? "uint" : "int";
        result.valid = true;
        result.deprecatedSuffix = deprecatedSuffix;
        result.recommendedSuffix = recommendedSuffix;
        result.deprecatedSuffixOffset = suffixOffset;
        return result;
      }
      if (parseIntegerSuffix(value, i, isUnsigned, invalidOffset)) {
        result.type = isUnsigned ? "uint" : "int";
        result.valid = true;
        return result;
      }
      result.type = isUnsigned ? "uint" : "int";
      result.invalidSuffix = value[invalidOffset];
      result.invalidSuffixOffset = invalidOffset;
      return result;
    }

    result.type = decimalLiteralType(hasFloatSyntax, value[i]);
    result.invalidSuffix = value[i];
    result.invalidSuffixOffset = i;
    return result;
  }

  result.matched = true;
  result.valid = true;
  result.type = decimalLiteralType(hasFloatSyntax, 0);
  return result;
}

bool tokensAreAdjacent(const std::vector<LexToken> &tokens, size_t begin,
                       size_t endInclusive) {
  if (begin >= tokens.size() || endInclusive >= tokens.size())
    return false;
  for (size_t i = begin; i < endInclusive; i++) {
    if (tokens[i].end != tokens[i + 1].start)
      return false;
  }
  return true;
}

bool startsWithDigit(const LexToken &token) {
  if (token.text.empty())
    return false;
  size_t start = 0;
  if (token.text[0] == '+' || token.text[0] == '-')
    start = 1;
  return start < token.text.size() &&
         std::isdigit(static_cast<unsigned char>(token.text[start]));
}

bool isNumericLiteralStartToken(const std::vector<LexToken> &tokens,
                                size_t index) {
  if (index >= tokens.size())
    return false;
  if (tokens[index].kind == LexToken::Kind::Identifier)
    return startsWithDigit(tokens[index]);
  return tokens[index].kind == LexToken::Kind::Punct &&
         tokens[index].text == "." && index + 1 < tokens.size() &&
         tokens[index + 1].kind == LexToken::Kind::Identifier &&
         startsWithDigit(tokens[index + 1]);
}

bool isDigitsText(const std::string &value) {
  if (value.empty())
    return false;
  for (char ch : value) {
    if (!std::isdigit(static_cast<unsigned char>(ch)))
      return false;
  }
  return true;
}

bool isIntegerSuffixText(const std::string &value) {
  size_t i = 0;
  bool isUnsigned = false;
  size_t invalidOffset = 0;
  return parseIntegerSuffix(value, i, isUnsigned, invalidOffset);
}

bool hasExponentDigitsAfterMarker(const std::string &value) {
  const size_t exponent = value.find_first_of("eE");
  if (exponent == std::string::npos || exponent + 1 >= value.size())
    return false;
  size_t digits = exponent + 1;
  if (value[digits] == '+' || value[digits] == '-')
    digits++;
  if (digits >= value.size())
    return false;
  for (size_t i = digits; i < value.size(); i++) {
    if (!std::isdigit(static_cast<unsigned char>(value[i])))
      return false;
  }
  return true;
}

bool canAppendNumericLiteralToken(const std::string &current,
                                  const LexToken &next) {
  if (current.empty() || next.text.empty())
    return false;

  if (next.kind == LexToken::Kind::Punct) {
    if (next.text == ".") {
      return current.find('.') == std::string::npos &&
             current.find_first_of("eE") == std::string::npos;
    }
    if (next.text == "+" || next.text == "-") {
      return current.back() == 'e' || current.back() == 'E';
    }
    return false;
  }

  if (next.kind != LexToken::Kind::Identifier)
    return false;
  if (startsWithDigit(next)) {
    return current.back() == '.' || current.back() == '+' ||
           current.back() == '-' || current.back() == 'e' ||
           current.back() == 'E';
  }
  if (next.text.size() == 1 && isFloatLiteralSuffix(next.text[0])) {
    return current.find('.') != std::string::npos ||
           hasExponentDigitsAfterMarker(current);
  }
  if (isIntegerSuffixText(next.text)) {
    return current.find('.') == std::string::npos &&
           current.find_first_of("eE") == std::string::npos;
  }
  return false;
}

NumericLiteralParseResult makeNumericLiteralCandidate(
    const std::vector<LexToken> &tokens, size_t index, size_t span) {
  NumericLiteralParseResult result;
  if (index >= tokens.size() || span == 0 || index + span > tokens.size())
    return result;
  if (!tokensAreAdjacent(tokens, index, index + span - 1))
    return result;

  std::string text;
  std::vector<std::pair<size_t, size_t>> tokenOffsets;
  tokenOffsets.reserve(span);
  for (size_t k = 0; k < span; k++) {
    tokenOffsets.push_back({text.size(), index + k});
    text += tokens[index + k].text;
  }

  NumericLiteralTextParseResult parsed = parseNumericLiteralText(text);
  if (!parsed.matched)
    return result;

  result.matched = true;
  result.valid = parsed.valid;
  result.tokenSpan = span;
  result.type = parsed.type;
  result.invalidSuffix = parsed.invalidSuffix;
  result.invalidSuffixTokenIndex = index;
  result.deprecatedSuffix = parsed.deprecatedSuffix;
  result.recommendedSuffix = parsed.recommendedSuffix;
  result.deprecatedSuffixTokenIndex = index;
  if (result.invalidSuffix != 0) {
    for (size_t k = 0; k < tokenOffsets.size(); k++) {
      const size_t begin = tokenOffsets[k].first;
      const size_t end =
          k + 1 < tokenOffsets.size() ? tokenOffsets[k + 1].first : text.size();
      if (parsed.invalidSuffixOffset >= begin &&
          parsed.invalidSuffixOffset < end) {
        result.invalidSuffixTokenIndex = tokenOffsets[k].second;
        break;
      }
    }
  }
  if (!result.deprecatedSuffix.empty()) {
    for (size_t k = 0; k < tokenOffsets.size(); k++) {
      const size_t begin = tokenOffsets[k].first;
      const size_t end =
          k + 1 < tokenOffsets.size() ? tokenOffsets[k + 1].first : text.size();
      if (parsed.deprecatedSuffixOffset >= begin &&
          parsed.deprecatedSuffixOffset < end) {
        result.deprecatedSuffixTokenIndex = tokenOffsets[k].second;
        break;
      }
    }
  }
  return result;
}

std::vector<size_t>
numericLiteralCandidateSpans(const std::vector<LexToken> &tokens,
                             size_t index) {
  std::vector<size_t> spans;
  if (index >= tokens.size())
    return spans;

  std::string text;
  for (size_t cursor = index; cursor < tokens.size(); cursor++) {
    if (cursor > index && tokens[cursor - 1].end != tokens[cursor].start)
      break;

    text += tokens[cursor].text;
    NumericLiteralTextParseResult parsed = parseNumericLiteralText(text);
    if (parsed.matched)
      spans.push_back(cursor - index + 1);

    if (cursor + 1 >= tokens.size())
      break;
    if (tokens[cursor].end != tokens[cursor + 1].start)
      break;
    if (parsed.valid && tokens[cursor + 1].kind == LexToken::Kind::Punct &&
        tokens[cursor + 1].text == "." && cursor + 2 < tokens.size() &&
        tokens[cursor + 1].end == tokens[cursor + 2].start &&
        tokens[cursor + 2].kind == LexToken::Kind::Identifier &&
        !startsWithDigit(tokens[cursor + 2]) &&
        !tokens[cursor + 2].text.empty() &&
        tokens[cursor + 2].text[0] != 'e' &&
        tokens[cursor + 2].text[0] != 'E')
      break;
    if (!canAppendNumericLiteralToken(text, tokens[cursor + 1]))
      break;
  }
  return spans;
}

} // namespace

NumericLiteralParseResult parseNumericLiteralFromTokens(
    const std::vector<LexToken> &tokens, size_t index) {
  NumericLiteralParseResult empty;
  if (!isNumericLiteralStartToken(tokens, index))
    return empty;

  std::vector<size_t> spans = numericLiteralCandidateSpans(tokens, index);

  NumericLiteralParseResult longestValid;
  NumericLiteralParseResult longestInvalid;
  for (size_t span : spans) {
    NumericLiteralParseResult candidate =
        makeNumericLiteralCandidate(tokens, index, span);
    if (!candidate.matched)
      continue;
    if (candidate.valid) {
      if (!longestValid.matched ||
          candidate.tokenSpan > longestValid.tokenSpan)
        longestValid = candidate;
      continue;
    }
    if (!longestInvalid.matched ||
        candidate.tokenSpan > longestInvalid.tokenSpan)
      longestInvalid = candidate;
  }
  if (longestInvalid.matched &&
      (!longestValid.matched ||
       longestInvalid.tokenSpan > longestValid.tokenSpan))
    return longestInvalid;
  if (longestValid.matched)
    return longestValid;
  return longestInvalid;
}

size_t numericLiteralTokenSpan(const std::vector<LexToken> &tokens,
                                      size_t index) {
  NumericLiteralParseResult parsed =
      parseNumericLiteralFromTokens(tokens, index);
  return parsed.matched && parsed.valid ? parsed.tokenSpan : 0;
}

std::string
inferNumericLiteralTypeFromTokens(const std::vector<LexToken> &tokens,
                                  size_t index) {
  NumericLiteralParseResult parsed =
      parseNumericLiteralFromTokens(tokens, index);
  return parsed.matched ? parsed.type : "";
}

bool parseVectorOrScalarType(const std::string &type,
                                    std::string &scalarOut, int &dimensionOut) {
  int dim = 0;
  if (isVectorType(type, dim)) {
    dimensionOut = dim;
    scalarOut = type.substr(0, type.size() - 1);
    return true;
  }
  if (isScalarType(type)) {
    dimensionOut = 1;
    scalarOut = type;
    return true;
  }
  return false;
}

bool isNarrowingPrecisionAssignment(const std::string &lhsType,
                                           const std::string &rhsType) {
  std::string lhsScalar;
  std::string rhsScalar;
  int lhsDim = 0;
  int rhsDim = 0;
  if (parseVectorOrScalarType(lhsType, lhsScalar, lhsDim) &&
      parseVectorOrScalarType(rhsType, rhsScalar, rhsDim) && lhsDim == rhsDim) {
    return lhsScalar == "half" &&
           (rhsScalar == "float" || rhsScalar == "double");
  }
  std::string lhsMatScalar;
  std::string rhsMatScalar;
  int lhsRows = 0;
  int lhsCols = 0;
  int rhsRows = 0;
  int rhsCols = 0;
  if (isMatrixType(lhsType, lhsMatScalar, lhsRows, lhsCols) &&
      isMatrixType(rhsType, rhsMatScalar, rhsRows, rhsCols) &&
      lhsRows == rhsRows && lhsCols == rhsCols) {
    return lhsMatScalar == "half" &&
           (rhsMatScalar == "float" || rhsMatScalar == "double");
  }
  return false;
}

bool isHalfFamilyType(const std::string &type) {
  std::string scalar;
  int dim = 0;
  if (parseVectorOrScalarType(type, scalar, dim))
    return scalar == "half";
  std::string matScalar;
  int rows = 0;
  int cols = 0;
  if (isMatrixType(type, matScalar, rows, cols))
    return matScalar == "half";
  return false;
}

std::string
inferNarrowingFallbackRhsTypeFromTokens(const std::vector<LexToken> &tokens,
                                        size_t startIndex, size_t endIndex) {
  endIndex = std::min(endIndex, tokens.size());
  bool sawFloatLiteral = false;
  bool sawHalfLiteral = false;
  for (size_t i = startIndex; i < endIndex; i++) {
    NumericLiteralParseResult numeric = parseNumericLiteralFromTokens(tokens, i);
    if (numeric.matched) {
      if (numeric.type == "float" || numeric.type == "double")
        sawFloatLiteral = true;
      else if (numeric.type == "half")
        sawHalfLiteral = true;
      i += std::max<size_t>(numeric.tokenSpan, 1) - 1;
    } else {
      const std::string literal = inferLiteralType(tokens[i].text);
      if (literal == "float" || literal == "double")
        sawFloatLiteral = true;
      else if (literal == "half")
        sawHalfLiteral = true;
    }
    if (sawFloatLiteral)
      return "float";
  }
  if (sawHalfLiteral)
    return "half";
  return "";
}

std::string makeVectorOrScalarType(const std::string &scalar, int dim) {
  if (scalar.empty())
    return "";
  if (dim <= 1)
    return scalar;
  return scalar + std::to_string(dim);
}

bool isSwizzleToken(const std::string &token) {
  if (token.empty() || token.size() > 4)
    return false;
  for (char ch : token) {
    if (ch == 'x' || ch == 'y' || ch == 'z' || ch == 'w' || ch == 'r' ||
        ch == 'g' || ch == 'b' || ch == 'a')
      continue;
    return false;
  }
  return true;
}

std::string applySwizzleType(const std::string &baseType,
                             const std::string &swizzle) {
  if (!isSwizzleToken(swizzle))
    return "";
  std::string scalar;
  int dim = 0;
  if (!parseVectorOrScalarType(baseType, scalar, dim)) {
    scalar = "float";
  }
  return makeVectorOrScalarType(scalar, static_cast<int>(swizzle.size()));
}

std::string applyIndexAccessType(const std::string &baseType) {
  const std::string normalized = normalizeTypeToken(baseType);
  if (normalized.empty())
    return normalized;

  std::string matrixScalar;
  int rows = 0;
  int cols = 0;
  if (isMatrixType(normalized, matrixScalar, rows, cols)) {
    return makeVectorOrScalarType(matrixScalar, cols);
  }

  int vecDim = 0;
  if (isVectorType(normalized, vecDim)) {
    return normalized.substr(0, normalized.size() - 1);
  }

  if (!normalized.empty() && normalized.back() == ']') {
    int depth = 0;
    for (size_t pos = normalized.size(); pos-- > 0;) {
      const char ch = normalized[pos];
      if (ch == ']') {
        depth++;
        continue;
      }
      if (ch == '[') {
        depth--;
        if (depth == 0)
          return normalizeTypeToken(normalized.substr(0, pos));
      }
    }
  }

  return normalized;
}

std::string inferExpressionTypeFromTokens(
    const std::vector<LexToken> &tokens, size_t startIndex,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentUri,
    const std::string &currentText, const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &scanExtensions,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    StructTypeCache &structCache, SymbolTypeCache &symbolCache,
    std::unordered_map<std::string, std::string> *fileTextCache);
std::string inferExpressionTypeFromTokensRange(
    const std::vector<LexToken> &tokens, size_t startIndex, size_t endIndex,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentUri,
    const std::string &currentText, const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &scanExtensions,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    StructTypeCache &structCache, SymbolTypeCache &symbolCache,
    std::unordered_map<std::string, std::string> *fileTextCache);

struct ExprParser {
  const std::vector<LexToken> &tokens;
  size_t endIndex;
  size_t i;
  const std::unordered_map<std::string, std::string> &locals;
  const std::string &currentUri;
  const std::string &currentText;
  const std::vector<std::string> &scanRoots;
  const std::vector<std::string> &workspaceFolders;
  const std::vector<std::string> &includePaths;
  const std::vector<std::string> &scanExtensions;
  const std::vector<std::string> &shaderExtensions;
  const std::unordered_map<std::string, int> &defines;
  StructTypeCache &structCache;
  SymbolTypeCache &symbolCache;
  std::unordered_map<std::string, std::string> *fileTextCache;

  const LexToken *peek() const {
    if (i >= endIndex)
      return nullptr;
    return &tokens[i];
  }

  const LexToken *consume() {
    if (i >= endIndex)
      return nullptr;
    return &tokens[i++];
  }

  bool matchPunct(const std::string &text) {
    const LexToken *t = peek();
    if (!t || t->kind != LexToken::Kind::Punct || t->text != text)
      return false;
    consume();
    return true;
  }

  std::string parseExpression() { return parseConditional(); }

  std::string parseConditional() {
    std::string base = parseOr();
    const LexToken *t = peek();
    if (!t || t->kind != LexToken::Kind::Punct || t->text != "?")
      return base;
    consume();
    std::string trueType = parseExpression();
    matchPunct(":");
    std::string falseType = parseExpression();
    return mergeArithmeticType(trueType, falseType);
  }

  std::string parseOr() {
    std::string left = parseAnd();
    bool saw = false;
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "||")
        break;
      saw = true;
      consume();
      std::string right = parseAnd();
      if (left.empty())
        left = right;
    }
    return saw ? "bool" : left;
  }

  std::string parseAnd() {
    std::string left = parseBitwiseOr();
    bool saw = false;
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "&&")
        break;
      saw = true;
      consume();
      std::string right = parseBitwiseOr();
      if (left.empty())
        left = right;
    }
    return saw ? "bool" : left;
  }

  std::string parseComparison() {
    std::string left = parseShift();
    bool saw = false;
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct)
        break;
      const std::string &op = t->text;
      if (op != "==" && op != "!=" && op != "<" && op != "<=" && op != ">" &&
          op != ">=")
        break;
      saw = true;
      consume();
      std::string right = parseShift();
      if (left.empty())
        left = right;
    }
    return saw ? "bool" : left;
  }

  static std::string mergeArithmeticType(const std::string &leftType,
                                         const std::string &rightType) {
    if (leftType.empty())
      return rightType;
    if (rightType.empty())
      return leftType;

    std::string leftMatScalar;
    std::string rightMatScalar;
    int leftRows = 0;
    int leftCols = 0;
    int rightRows = 0;
    int rightCols = 0;
    bool leftIsMat = isMatrixType(leftType, leftMatScalar, leftRows, leftCols);
    bool rightIsMat =
        isMatrixType(rightType, rightMatScalar, rightRows, rightCols);
    if (leftIsMat || rightIsMat) {
      if (leftIsMat && rightIsMat) {
        if (leftRows != rightRows || leftCols != rightCols)
          return leftType;
        std::string outScalar = mergeNumericScalar(leftMatScalar, rightMatScalar);
        return makeMatrixType(outScalar, leftRows, leftCols);
      }
      if (leftIsMat) {
        if (isNumericScalarType(rightType)) {
          return makeMatrixType(mergeNumericScalar(leftMatScalar, rightType),
                                leftRows, leftCols);
        }
        return leftType;
      }
      if (isNumericScalarType(leftType)) {
        return makeMatrixType(mergeNumericScalar(leftType, rightMatScalar),
                              rightRows, rightCols);
      }
      return rightType;
    }

    std::string leftScalar;
    std::string rightScalar;
    int leftDim = 0;
    int rightDim = 0;
    bool leftOk = parseVectorOrScalarType(leftType, leftScalar, leftDim);
    bool rightOk = parseVectorOrScalarType(rightType, rightScalar, rightDim);
    if (!leftOk || !rightOk) {
      if (leftType == rightType)
        return leftType;
      return leftType;
    }

    int outDim = std::max(leftDim, rightDim);
    std::string outScalar = mergeNumericScalar(leftScalar, rightScalar);
    return makeVectorOrScalarType(outScalar, outDim);
  }

  static std::string mergeBitwiseType(const std::string &leftType,
                                      const std::string &rightType) {
    if (leftType.empty())
      return rightType;
    if (rightType.empty())
      return leftType;
    std::string leftScalar;
    std::string rightScalar;
    int leftDim = 0;
    int rightDim = 0;
    bool leftOk = parseVectorOrScalarType(leftType, leftScalar, leftDim);
    bool rightOk = parseVectorOrScalarType(rightType, rightScalar, rightDim);
    if (!leftOk || !rightOk)
      return leftType;
    if ((leftScalar != "int" && leftScalar != "uint") ||
        (rightScalar != "int" && rightScalar != "uint")) {
      return leftType;
    }
    int outDim = 1;
    if (leftDim == rightDim)
      outDim = leftDim;
    else if (leftDim == 1)
      outDim = rightDim;
    else if (rightDim == 1)
      outDim = leftDim;
    else
      outDim = leftDim;
    std::string outScalar =
        (leftScalar == "uint" || rightScalar == "uint") ? "uint" : "int";
    return makeVectorOrScalarType(outScalar, outDim);
  }

  static std::string mergeMultiplyType(const std::string &leftType,
                                       const std::string &rightType) {
    if (leftType.empty())
      return rightType;
    if (rightType.empty())
      return leftType;

    std::string leftMatScalar;
    std::string rightMatScalar;
    int leftRows = 0;
    int leftCols = 0;
    int rightRows = 0;
    int rightCols = 0;
    bool leftIsMat = isMatrixType(leftType, leftMatScalar, leftRows, leftCols);
    bool rightIsMat =
        isMatrixType(rightType, rightMatScalar, rightRows, rightCols);

    std::string leftScalar;
    std::string rightScalar;
    int leftDim = 0;
    int rightDim = 0;
    bool leftIsVec =
        parseVectorOrScalarType(leftType, leftScalar, leftDim) && leftDim > 1;
    bool rightIsVec =
        parseVectorOrScalarType(rightType, rightScalar, rightDim) &&
        rightDim > 1;

    if (leftIsMat && rightIsMat) {
      if (leftCols != rightRows)
        return leftType;
      std::string outScalar = mergeNumericScalar(leftMatScalar, rightMatScalar);
      return makeMatrixType(outScalar, leftRows, rightCols);
    }

    if (leftIsVec && rightIsMat) {
      if (leftDim != rightRows) {
        return makeVectorOrScalarType(
            mergeNumericScalar(leftScalar, rightMatScalar), rightCols);
      }
      std::string outScalar = mergeNumericScalar(leftScalar, rightMatScalar);
      return makeVectorOrScalarType(outScalar, rightCols);
    }

    if (leftIsMat && rightIsVec) {
      if (leftCols != rightDim) {
        return makeVectorOrScalarType(
            mergeNumericScalar(leftMatScalar, rightScalar), leftRows);
      }
      std::string outScalar = mergeNumericScalar(leftMatScalar, rightScalar);
      return makeVectorOrScalarType(outScalar, leftRows);
    }

    if (leftIsMat && isNumericScalarType(rightType)) {
      return makeMatrixType(mergeNumericScalar(leftMatScalar, rightType),
                            leftRows, leftCols);
    }
    if (rightIsMat && isNumericScalarType(leftType)) {
      return makeMatrixType(mergeNumericScalar(leftType, rightMatScalar),
                            rightRows, rightCols);
    }

    return mergeArithmeticType(leftType, rightType);
  }

  std::string parseShift() {
    std::string left = parseAddSub();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct ||
          (t->text != "<<" && t->text != ">>"))
        break;
      consume();
      std::string right = parseAddSub();
      if (left.empty())
        left = right;
      else if (!right.empty())
        left = mergeBitwiseType(left, right);
    }
    return left;
  }

  std::string parseBitwiseAnd() {
    std::string left = parseComparison();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "&")
        break;
      consume();
      std::string right = parseComparison();
      left = mergeBitwiseType(left, right);
    }
    return left;
  }

  std::string parseBitwiseXor() {
    std::string left = parseBitwiseAnd();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "^")
        break;
      consume();
      std::string right = parseBitwiseAnd();
      left = mergeBitwiseType(left, right);
    }
    return left;
  }

  std::string parseBitwiseOr() {
    std::string left = parseBitwiseXor();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct || t->text != "|")
        break;
      consume();
      std::string right = parseBitwiseXor();
      left = mergeBitwiseType(left, right);
    }
    return left;
  }

  std::string parseAddSub() {
    std::string left = parseMulDiv();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct ||
          (t->text != "+" && t->text != "-"))
        break;
      consume();
      std::string right = parseMulDiv();
      left = mergeArithmeticType(left, right);
    }
    return left;
  }

  std::string parseMulDiv() {
    std::string left = parseUnary();
    while (true) {
      const LexToken *t = peek();
      if (!t || t->kind != LexToken::Kind::Punct ||
          (t->text != "*" && t->text != "/" && t->text != "%"))
        break;
      const std::string op = t->text;
      consume();
      std::string right = parseUnary();
      if (op == "*")
        left = mergeMultiplyType(left, right);
      else
        left = mergeArithmeticType(left, right);
    }
    return left;
  }

  std::string parseUnary() {
    const LexToken *t = peek();
    if (t && t->kind == LexToken::Kind::Punct &&
        (t->text == "!" || t->text == "-" || t->text == "+")) {
      consume();
      return parseUnary();
    }
    return parsePostfix(parsePrimary());
  }

  static void skipBalanced(const std::vector<LexToken> &tokens, size_t &i,
                           size_t endIndex, const std::string &open,
                           const std::string &close) {
    int depth = 0;
    while (i < endIndex) {
      const auto &t = tokens[i];
      if (t.kind == LexToken::Kind::Punct) {
        if (t.text == open)
          depth++;
        else if (t.text == close) {
          depth = depth > 0 ? depth - 1 : 0;
          if (depth == 0) {
            i++;
            break;
          }
        }
      }
      i++;
    }
  }

  static std::string
  inferBuiltinReturnType(const std::string &name,
                         const std::vector<std::string> &args) {
    std::vector<BuiltinTypeInfo> infos;
    infos.reserve(args.size());
    for (const auto &a : args) {
      infos.push_back(parseBuiltinTypeInfo(a));
    }
    BuiltinResolveResult rr = resolveBuiltinCall(name, infos);
    if (!rr.ok)
      return "";
    return builtinTypeInfoToString(rr.ret);
  }

  std::vector<std::string> parseCallArgumentTypes() {
    std::vector<std::string> args;
    if (!matchPunct("("))
      return args;
    if (matchPunct(")"))
      return args;

    while (i < endIndex) {
      size_t argStart = i;
      int parenDepth = 0;
      int bracketDepth = 0;
      while (i < endIndex) {
        const auto &t = tokens[i];
        if (t.kind == LexToken::Kind::Punct) {
          if (t.text == "(")
            parenDepth++;
          else if (t.text == ")") {
            if (parenDepth == 0)
              break;
            parenDepth--;
          } else if (t.text == "[")
            bracketDepth++;
          else if (t.text == "]")
            bracketDepth = bracketDepth > 0 ? bracketDepth - 1 : 0;
          else if (t.text == "," && parenDepth == 0 && bracketDepth == 0)
            break;
        }
        i++;
      }
      size_t argEnd = i;
      args.push_back(inferExpressionTypeFromTokensRange(
          tokens, argStart, argEnd, locals, currentUri, currentText, scanRoots,
          workspaceFolders, includePaths, scanExtensions, shaderExtensions,
          defines, structCache, symbolCache, fileTextCache));
      if (i < endIndex && tokens[i].kind == LexToken::Kind::Punct &&
          tokens[i].text == ",") {
        i++;
        continue;
      }
      break;
    }

    matchPunct(")");
    return args;
  }

  std::string parsePrimary() {
    const LexToken *t = peek();
    if (!t)
      return "";
    if (t->kind == LexToken::Kind::Punct && t->text == "(") {
      if (i + 2 < endIndex &&
          tokens[i + 1].kind == LexToken::Kind::Identifier &&
          tokens[i + 2].kind == LexToken::Kind::Punct &&
          tokens[i + 2].text == ")") {
        const std::string castType = tokens[i + 1].text;
        int dim = 0;
        int rows = 0;
        int cols = 0;
        bool isBuiltinCastType = isVectorType(castType, dim) ||
                                 isScalarType(castType) ||
                                 isMatrixType(castType, rows, cols);
        bool isStructCastType =
            hasStructDeclarationInText(currentText, castType);
        if (isBuiltinCastType || isStructCastType) {
          consume();
          consume();
          consume();
          parseUnary();
          return castType;
        }
      }
      consume();
      std::string inner = parseExpression();
      matchPunct(")");
      return inner;
    }
    if (t->kind == LexToken::Kind::Identifier) {
      std::string word = t->text;

      NumericLiteralParseResult numeric =
          parseNumericLiteralFromTokens(tokens, i);
      if (numeric.matched && !numeric.type.empty()) {
        i += std::max<size_t>(numeric.tokenSpan, 1);
        return numeric.type;
      }

      consume();

      int dim = 0;
      int rows = 0;
      int cols = 0;
      if (isVectorType(word, dim) || isScalarType(word) ||
          isMatrixType(word, rows, cols)) {
        const LexToken *next = peek();
        if (next && next->kind == LexToken::Kind::Punct && next->text == "(") {
          skipBalanced(tokens, i, endIndex, "(", ")");
        }
        return word;
      }

      std::string literal = inferLiteralType(word);
      if (!literal.empty())
        return literal;

      const LexToken *next = peek();
      if (next && next->kind == LexToken::Kind::Punct && next->text == "(") {
        size_t callStart = i;
        std::vector<std::string> args = parseCallArgumentTypes();
        std::string builtin = inferBuiltinReturnType(word, args);
        if (!builtin.empty())
          return builtin;
        i = callStart;
        skipBalanced(tokens, i, endIndex, "(", ")");
        auto cached = symbolCache.typeBySymbol.find(word);
        if (cached != symbolCache.typeBySymbol.end())
          return cached->second;
        std::string indexed;
    if (workspaceSummaryRuntimeGetSymbolType(word, indexed) &&
        !indexed.empty()) {
          symbolCache.typeBySymbol.emplace(word, indexed);
          return indexed;
        }
        bool attempted = false;
        auto attemptIt = symbolCache.attemptedInText.find(word);
        if (attemptIt != symbolCache.attemptedInText.end())
          attempted = attemptIt->second;
        if (attempted)
          return "";
        symbolCache.attemptedInText[word] = true;
        std::string inText = resolveSymbolTypeInText(currentText, word);
        if (!inText.empty()) {
          symbolCache.typeBySymbol.emplace(word, inText);
          return inText;
        }
        return "";
      }

      auto it = locals.find(word);
      if (it != locals.end())
        return it->second;
      std::string inText = resolveSymbolTypeInText(currentText, word);
      if (!inText.empty())
        return inText;
      return resolveSymbolTypeByWorkspaceSummary(word, symbolCache);
    }
    if (t->kind == LexToken::Kind::Punct) {
      NumericLiteralParseResult numeric =
          parseNumericLiteralFromTokens(tokens, i);
      if (numeric.matched && !numeric.type.empty()) {
        i += std::max<size_t>(numeric.tokenSpan, 1);
        return numeric.type;
      }
      return "";
    }
    std::string literal = inferLiteralType(t->text);
    consume();
    return literal;
  }

  std::string parsePostfix(std::string baseType) {
    while (true) {
      const LexToken *t = peek();
      if (!t)
        break;
      if (t->kind == LexToken::Kind::Punct && t->text == ".") {
        consume();
        const LexToken *name = peek();
        if (!name || name->kind != LexToken::Kind::Identifier)
          break;
        std::string member = name->text;
        consume();
        const LexToken *after = peek();
        if (after && after->kind == LexToken::Kind::Punct &&
            after->text == "(") {
          parseCallArgumentTypes();
          HlslBuiltinMethodRule methodRule;
          if (lookupHlslBuiltinMethodRule(member, baseType, methodRule) &&
              !methodRule.returnType.empty() &&
              normalizeTypeToken(methodRule.returnType) != "void") {
            baseType = normalizeTypeToken(methodRule.returnType);
            continue;
          }
          continue;
        }
        if (isSwizzleToken(member)) {
          std::string swizzled = applySwizzleType(baseType, member);
          if (!swizzled.empty())
            baseType = swizzled;
          continue;
        }
        if (!baseType.empty()) {
          std::string memberType =
              resolveStructMemberType(baseType, member, currentUri, currentText,
                                      scanRoots, workspaceFolders, includePaths,
                                      scanExtensions, defines, structCache);
          if (!memberType.empty()) {
            baseType = memberType;
            continue;
          }
        }
        continue;
      }
      if (t->kind == LexToken::Kind::Punct && t->text == "[") {
        consume();
        skipBalanced(tokens, i, endIndex, "[", "]");
        baseType = applyIndexAccessType(baseType);
        continue;
      }
      break;
    }
    return baseType;
  }
};

std::string inferExpressionTypeFromTokens(
    const std::vector<LexToken> &tokens, size_t startIndex,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentUri,
    const std::string &currentText, const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &scanExtensions,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    StructTypeCache &structCache, SymbolTypeCache &symbolCache,
    std::unordered_map<std::string, std::string> *fileTextCache) {
  size_t endIndex = tokens.size();
  int parenDepth = 0;
  int bracketDepth = 0;
  for (size_t i = startIndex; i < tokens.size(); i++) {
    if (tokens[i].kind != LexToken::Kind::Punct)
      continue;
    const std::string &p = tokens[i].text;
    if (p == "(") {
      parenDepth++;
      continue;
    }
    if (p == ")") {
      if (parenDepth > 0)
        parenDepth--;
      continue;
    }
    if (p == "[") {
      bracketDepth++;
      continue;
    }
    if (p == "]") {
      if (bracketDepth > 0)
        bracketDepth--;
      continue;
    }
    if (parenDepth == 0 && bracketDepth == 0 && (p == ";" || p == ",")) {
      endIndex = i;
      break;
    }
  }
  return inferExpressionTypeFromTokensRange(
      tokens, startIndex, endIndex, locals, currentUri, currentText, scanRoots,
      workspaceFolders, includePaths, scanExtensions, shaderExtensions, defines,
      structCache, symbolCache, fileTextCache);
}

std::string inferExpressionTypeFromTokensRange(
    const std::vector<LexToken> &tokens, size_t startIndex, size_t endIndex,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentUri,
    const std::string &currentText, const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &scanExtensions,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    StructTypeCache &structCache, SymbolTypeCache &symbolCache,
    std::unordered_map<std::string, std::string> *fileTextCache) {
  endIndex = std::min(endIndex, tokens.size());
  ExprParser parser{tokens,      endIndex,     startIndex,     locals,
                    currentUri,  currentText,  scanRoots,      workspaceFolders,
                    includePaths, scanExtensions, shaderExtensions, defines,
                    structCache, symbolCache, fileTextCache};
  return parser.parseExpression();
}

