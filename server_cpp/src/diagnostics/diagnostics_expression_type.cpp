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
  return "";
}

BuiltinTypeInfo parseBuiltinTypeInfo(std::string type) {
  type = normalizeTypeToken(type);
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

static bool isBuiltinNumericElem(BuiltinElemKind k) {
  return k == BuiltinElemKind::Int || k == BuiltinElemKind::UInt ||
         k == BuiltinElemKind::Half || k == BuiltinElemKind::Float;
}

static BuiltinElemKind promoteBuiltinNumericElem(BuiltinElemKind a,
                                                 BuiltinElemKind b,
                                                 bool &signednessMixOut) {
  signednessMixOut = false;
  if (a == BuiltinElemKind::Unknown || b == BuiltinElemKind::Unknown)
    return BuiltinElemKind::Unknown;
  if (!isBuiltinNumericElem(a) || !isBuiltinNumericElem(b))
    return BuiltinElemKind::Unknown;
  if (a == BuiltinElemKind::Float || b == BuiltinElemKind::Float)
    return BuiltinElemKind::Float;
  if (a == BuiltinElemKind::Half || b == BuiltinElemKind::Half)
    return BuiltinElemKind::Half;
  if ((a == BuiltinElemKind::Int && b == BuiltinElemKind::UInt) ||
      (a == BuiltinElemKind::UInt && b == BuiltinElemKind::Int)) {
    signednessMixOut = true;
    return BuiltinElemKind::Unknown;
  }
  return a;
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

  auto unifyElem = [&](BuiltinElemKind &outElem) -> bool {
    outElem = args[0].elem;
    for (size_t i = 1; i < args.size(); i++) {
      bool mix = false;
      BuiltinElemKind promoted =
          promoteBuiltinNumericElem(outElem, args[i].elem, mix);
      if (mix) {
        r.warnMixedSignedness = true;
        return false;
      }
      if (promoted == BuiltinElemKind::Unknown)
        return false;
      outElem = promoted;
    }
    return true;
  };
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

  if (name == "length" || name == "distance") {
    if (args.size() < 1)
      return r;
    if (!isBuiltinNumericElem(args[0].elem))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (name == "length") {
      outElem = args[0].elem;
    } else {
      if (args.size() < 2)
        return r;
      if (!isBuiltinNumericElem(args[1].elem))
        return r;
      if (!exactShapeEq(args[0], args[1]))
        return r;
      if (!unifyElem(outElem))
        return r;
    }
    r.ok = true;
    r.ret.shape = BuiltinTypeInfo::ShapeKind::Scalar;
    r.ret.elem = outElem;
    r.ret.dim = 1;
    return r;
  }

  if (name == "dot") {
    if (args.size() < 2)
      return r;
    if (!isBuiltinNumericElem(args[0].elem) ||
        !isBuiltinNumericElem(args[1].elem))
      return r;
    bool okShape = false;
    if (args[0].shape == BuiltinTypeInfo::ShapeKind::Vector &&
        args[1].shape == BuiltinTypeInfo::ShapeKind::Vector &&
        args[0].dim == args[1].dim) {
      okShape = true;
    }
    if (args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar &&
        args[1].shape == BuiltinTypeInfo::ShapeKind::Scalar) {
      okShape = true;
    }
    if (!okShape)
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret.shape = BuiltinTypeInfo::ShapeKind::Scalar;
    r.ret.elem = outElem;
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
    if (!exactShapeEq(args[0], args[1]))
      return r;
    if (!(args[2].shape == BuiltinTypeInfo::ShapeKind::Scalar ||
          exactShapeEq(args[2], args[0])))
      return r;
    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = args[0];
    r.ret.elem = outElem;
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
    BuiltinTypeInfo::ShapeKind outShape = args[0].shape;
    if (args[0].shape == BuiltinTypeInfo::ShapeKind::Scalar)
      outShape = args[1].shape;
    else if (args[1].shape == BuiltinTypeInfo::ShapeKind::Scalar)
      outShape = args[0].shape;
    else if (!exactShapeEq(args[0], args[1]))
      return r;

    BuiltinElemKind outElem = BuiltinElemKind::Unknown;
    if (!unifyElem(outElem))
      return r;
    r.ok = true;
    r.ret = outShape == BuiltinTypeInfo::ShapeKind::Scalar ? args[0] : args[0];
    r.ret.shape = outShape;
    r.ret.elem = outElem;
    return r;
  }

  if (name == "mul" && args.size() >= 2) {
    const auto &a = args[0];
    const auto &b = args[1];
    if (!isBuiltinNumericElem(a.elem) || !isBuiltinNumericElem(b.elem))
      return r;
    bool mix = false;
    BuiltinElemKind outElem = promoteBuiltinNumericElem(a.elem, b.elem, mix);
    if (mix) {
      r.warnMixedSignedness = true;
      return r;
    }
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
  if (base == "float" || base == "half" || base == "int" || base == "uint" ||
      base == "bool") {
    dimensionOut = dim;
    return true;
  }
  return false;
}

bool isScalarType(const std::string &type) {
  return type == "float" || type == "half" || type == "int" || type == "uint" ||
         type == "bool";
}

bool isNumericScalarType(const std::string &type) {
  return type == "float" || type == "half" || type == "int" || type == "uint";
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
  if (base != "float" && base != "half" && base != "int" && base != "uint")
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
  auto parseHexIntegerTokenSuffix = [](const std::string &value,
                                       char &suffixOut,
                                       bool &invalidSuffixOut) -> bool {
    suffixOut = 0;
    invalidSuffixOut = false;
    if (value.empty())
      return false;
    size_t start = 0;
    if (value[0] == '+' || value[0] == '-')
      start = 1;
    if (start + 2 >= value.size())
      return false;
    if (value[start] != '0' ||
        (value[start + 1] != 'x' && value[start + 1] != 'X'))
      return false;
    size_t i = start + 2;
    const size_t digitsStart = i;
    while (i < value.size() &&
           std::isxdigit(static_cast<unsigned char>(value[i]))) {
      i++;
    }
    if (i == digitsStart)
      return false;
    if (i == value.size())
      return true;
    if (i + 1 == value.size() &&
        std::isalpha(static_cast<unsigned char>(value[i]))) {
      suffixOut = value[i];
      invalidSuffixOut = !(suffixOut == 'u' || suffixOut == 'U');
      return true;
    }
    return false;
  };
  {
    char suffix = 0;
    bool invalid = false;
    if (parseHexIntegerTokenSuffix(token, suffix, invalid)) {
      if (suffix == 'u' || suffix == 'U')
        return "uint";
      return "int";
    }
  }
  bool hasDigit = false;
  bool hasDot = false;
  size_t start = 0;
  if (token[0] == '+' || token[0] == '-')
    start = 1;
  for (size_t i = start; i < token.size(); i++) {
    unsigned char ch = static_cast<unsigned char>(token[i]);
    if (std::isdigit(ch)) {
      hasDigit = true;
      continue;
    }
    if (token[i] == '.') {
      hasDot = true;
      continue;
    }
    return "";
  }
  if (!hasDigit)
    return "";
  return hasDot ? "float" : "int";
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

size_t numericLiteralTokenSpan(const std::vector<LexToken> &tokens,
                                      size_t index) {
  if (index >= tokens.size())
    return 0;
  if (tokens[index].kind != LexToken::Kind::Identifier)
    return 0;
  {
    char suffix = 0;
    bool invalid = false;
    const std::string &value = tokens[index].text;
    size_t start = 0;
    if (!value.empty() && (value[0] == '+' || value[0] == '-'))
      start = 1;
    if (start + 2 < value.size() && value[start] == '0' &&
        (value[start + 1] == 'x' || value[start + 1] == 'X')) {
      size_t i = start + 2;
      const size_t digitsStart = i;
      while (i < value.size() &&
             std::isxdigit(static_cast<unsigned char>(value[i]))) {
        i++;
      }
      if (i > digitsStart) {
        if (i == value.size())
          return 1;
        if (i + 1 == value.size() &&
            std::isalpha(static_cast<unsigned char>(value[i]))) {
          suffix = value[i];
          invalid = !(suffix == 'u' || suffix == 'U');
          return invalid ? 0 : 1;
        }
      }
    }
  }
  char suffix0 = 0;
  if (!isSignedDigitsWithOptionalSuffixToken(tokens[index].text, suffix0))
    return 0;
  if (suffix0 != 0)
    return 1;
  if (index + 2 < tokens.size() &&
      tokens[index + 1].kind == LexToken::Kind::Punct &&
      tokens[index + 1].text == "." &&
      tokens[index + 2].kind == LexToken::Kind::Identifier) {
    char suffix1 = 0;
    if (isSignedDigitsWithOptionalSuffixToken(tokens[index + 2].text, suffix1))
      return 3;
  }
  return 1;
}

std::string
inferNumericLiteralTypeFromTokens(const std::vector<LexToken> &tokens,
                                  size_t index) {
  if (index >= tokens.size())
    return "";
  if (tokens[index].kind != LexToken::Kind::Identifier)
    return "";
  {
    const std::string &value = tokens[index].text;
    size_t start = 0;
    if (!value.empty() && (value[0] == '+' || value[0] == '-'))
      start = 1;
    if (start + 2 < value.size() && value[start] == '0' &&
        (value[start + 1] == 'x' || value[start + 1] == 'X')) {
      size_t i = start + 2;
      const size_t digitsStart = i;
      while (i < value.size() &&
             std::isxdigit(static_cast<unsigned char>(value[i]))) {
        i++;
      }
      if (i > digitsStart) {
        if (i == value.size())
          return "int";
        if (i + 1 == value.size() && (value[i] == 'u' || value[i] == 'U'))
          return "uint";
        return "int";
      }
    }
  }
  char suffix0 = 0;
  if (!isSignedDigitsWithOptionalSuffixToken(tokens[index].text, suffix0))
    return "";
  if (index + 2 < tokens.size() &&
      tokens[index + 1].kind == LexToken::Kind::Punct &&
      tokens[index + 1].text == "." &&
      tokens[index + 2].kind == LexToken::Kind::Identifier && true) {
    char suffix1 = 0;
    if (!isSignedDigitsWithOptionalSuffixToken(tokens[index + 2].text,
                                               suffix1)) {
      char badSuffix = 0;
      if (isSignedDigitsWithSingleAlphaSuffixToken(tokens[index + 2].text,
                                                   badSuffix))
        return "float";
      return "int";
    }
    if (suffix1 == 'h')
      return "half";
    return "float";
  }
  if (suffix0 == 'h')
    return "half";
  if (suffix0 == 'f' || suffix0 == 'F')
    return "float";
  if (suffix0 == 'u' || suffix0 == 'U')
    return "uint";
  return "int";
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
    const std::string literal = inferLiteralType(tokens[i].text);
    if (literal == "float")
      sawFloatLiteral = true;
    else if (literal == "half")
      sawHalfLiteral = true;
    char suffix = 0;
    if (isSignedDigitsWithOptionalSuffixToken(tokens[i].text, suffix)) {
      if (suffix == 'f' || suffix == 'F')
        sawFloatLiteral = true;
      else if (suffix == 'h')
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
        std::string outScalar =
            leftMatScalar == rightMatScalar ? leftMatScalar : "float";
        return makeMatrixType(outScalar, leftRows, leftCols);
      }
      if (leftIsMat) {
        if (isNumericScalarType(rightType))
          return makeMatrixType(leftMatScalar, leftRows, leftCols);
        return leftType;
      }
      if (isNumericScalarType(leftType))
        return makeMatrixType(rightMatScalar, rightRows, rightCols);
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
    std::string outScalar = leftScalar == rightScalar ? leftScalar : "float";
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
      std::string outScalar =
          leftMatScalar == rightMatScalar ? leftMatScalar : "float";
      return makeMatrixType(outScalar, leftRows, rightCols);
    }

    if (leftIsVec && rightIsMat) {
      if (leftDim != rightRows)
        return makeVectorOrScalarType("float", rightCols);
      std::string outScalar =
          leftScalar == rightMatScalar ? leftScalar : "float";
      return makeVectorOrScalarType(outScalar, rightCols);
    }

    if (leftIsMat && rightIsVec) {
      if (leftCols != rightDim)
        return makeVectorOrScalarType("float", leftRows);
      std::string outScalar =
          leftMatScalar == rightScalar ? leftMatScalar : "float";
      return makeVectorOrScalarType(outScalar, leftRows);
    }

    if (leftIsMat && isNumericScalarType(rightType)) {
      return makeMatrixType(leftMatScalar, leftRows, leftCols);
    }
    if (rightIsMat && isNumericScalarType(leftType)) {
      return makeMatrixType(rightMatScalar, rightRows, rightCols);
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
    if (rr.warnMixedSignedness)
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

      std::string numeric = inferNumericLiteralTypeFromTokens(tokens, i - 1);
      if (!numeric.empty())
        return numeric;
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

