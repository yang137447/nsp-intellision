#pragma once

#include "diagnostics_symbol_type.hpp"
#include "nsf_lexer.hpp"
#include "type_relation.hpp"

#include <string>
#include <unordered_map>
#include <vector>

struct PreprocessorView;

enum class BuiltinElemKind {
  Unknown,
  Bool,
  Int,
  UInt,
  Int64,
  UInt64,
  Half,
  Float,
  Double
};

struct BuiltinTypeInfo {
  enum class ShapeKind { Unknown, Scalar, Vector, Matrix, Object };
  ShapeKind shape = ShapeKind::Unknown;
  BuiltinElemKind elem = BuiltinElemKind::Unknown;
  int dim = 0;
  int rows = 0;
  int cols = 0;
};

struct BuiltinResolveResult {
  bool ok = false;
  bool indeterminate = false;
  BuiltinTypeInfo ret;
  std::vector<TypeRelationResult> conversions;
};

struct NumericLiteralParseResult {
  bool matched = false;
  bool valid = false;
  size_t tokenSpan = 0;
  std::string type;
  char invalidSuffix = 0;
  size_t invalidSuffixTokenIndex = 0;
  std::string deprecatedSuffix;
  std::string recommendedSuffix;
  size_t deprecatedSuffixTokenIndex = 0;
};

// Diagnostics-side expression typing contract.
// Responsibilities: normalize type tokens, classify literals, model builtin
// type rules, and infer expression result types for semantic diagnostics.
// Builtin modeling covers common scalar/vector/matrix intrinsics in one shared
// path; unsupported or unavailable argument types should surface as
// indeterminate diagnostics instead of feature-local fallback guesses. The
// expression model also recognizes project 64-bit integer scalar aliases,
// preserves declaration array suffixes through indexing, and keeps bitwise
// shift/or results in that shared type path.
// `mul(matrix, vector)` also models the confirmed project shadercompiler
// lowering where a matrix with more columns than the vector dimension is
// accepted and returns a vector with the matrix row count; the reverse
// vector-matrix form and vector dimensions larger than matrix columns remain
// invalid.
// Builtins with out parameters are modeled at the call-shape layer when their
// public HLSL contract is type-only: `sincos` is call-only with same-shape
// numeric outputs, while `modf` returns the first floating argument type and
// requires the second out argument to have that exact scalar/vector/matrix type.
// Numeric literal parsing is token-span aware because the shared lexer splits
// decimal points and exponent signs into punctuation tokens. The accepted
// decimal/octal/hex literal grammar follows official HLSL numeric literal forms,
// including exponent notation, leading/trailing decimal points, h/H/f/F/l/L
// float suffixes, and u/U/l/L integer suffix orderings. Consumers should use
// parseNumericLiteralFromTokens before falling back to per-token handling.
// Implementation-only ll/ull integer suffixes are accepted as legacy forms so
// diagnostics can warn and recommend the standard l/ul spelling.
// Optional PreprocessorView inputs let callers provide active object-like macro
// replacements for the same source line. This helper may infer expression
// replacements through the same parser, normalize declaration-side type tokens
// through active macro replacements, and treat macro-like constructor calls as
// their active replacement type. It does not expand function-like macros or
// guess beyond the shared type_desc shape when replacement context is missing.
std::string normalizeTypeToken(std::string value);

std::string normalizeTypeTokenWithPreprocessor(
    std::string value, const PreprocessorView *preprocessorView,
    int sourceLine);

bool isVectorType(const std::string &type, int &dimensionOut);

bool isScalarType(const std::string &type);

bool isNumericScalarType(const std::string &type);

bool isMatrixType(const std::string &type, std::string &scalarOut,
                  int &rowsOut, int &colsOut);

bool isMatrixType(const std::string &type, int &rowsOut, int &colsOut);

bool parseVectorOrScalarType(const std::string &type, std::string &scalarOut,
                             int &dimensionOut);

std::string makeVectorOrScalarType(const std::string &scalar, int dim);

std::string makeMatrixType(const std::string &scalar, int rows, int cols);

std::string inferLiteralType(const std::string &token);

NumericLiteralParseResult parseNumericLiteralFromTokens(
    const std::vector<LexToken> &tokens, size_t index);

size_t numericLiteralTokenSpan(const std::vector<LexToken> &tokens,
                               size_t index);

std::string inferNumericLiteralTypeFromTokens(
    const std::vector<LexToken> &tokens, size_t index);

bool isNarrowingPrecisionAssignment(const std::string &lhsType,
                                    const std::string &rhsType);

bool isHalfFamilyType(const std::string &type);

std::string inferNarrowingFallbackRhsTypeFromTokens(
    const std::vector<LexToken> &tokens, size_t startIndex, size_t endIndex);

bool isSignedDigitsWithOptionalSuffixToken(const std::string &token,
                                           char &suffixOut);

bool isSignedDigitsWithSingleAlphaSuffixToken(const std::string &token,
                                              char &suffixOut);

bool isSwizzleToken(const std::string &token);

std::string applySwizzleType(const std::string &baseType,
                             const std::string &swizzle);

std::string applyIndexAccessType(const std::string &baseType);

BuiltinTypeInfo parseBuiltinTypeInfo(std::string type);

BuiltinResolveResult resolveBuiltinCall(const std::string &name,
                                        const std::vector<BuiltinTypeInfo> &args);

std::string inferExpressionTypeFromTokens(
    const std::vector<LexToken> &tokens, size_t startIndex,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentUri, const std::string &currentText,
    const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &scanExtensions,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    StructTypeCache &structCache, SymbolTypeCache &symbolCache,
    std::unordered_map<std::string, std::string> *fileTextCache,
    const PreprocessorView *preprocessorView = nullptr, int sourceLine = -1);

std::string inferExpressionTypeFromTokensRange(
    const std::vector<LexToken> &tokens, size_t startIndex, size_t endIndex,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentUri, const std::string &currentText,
    const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &scanExtensions,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    StructTypeCache &structCache, SymbolTypeCache &symbolCache,
    std::unordered_map<std::string, std::string> *fileTextCache,
    const PreprocessorView *preprocessorView = nullptr, int sourceLine = -1);
