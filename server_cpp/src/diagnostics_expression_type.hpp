#pragma once

#include "diagnostics_symbol_type.hpp"
#include "nsf_lexer.hpp"

#include <string>
#include <unordered_map>
#include <vector>

enum class BuiltinElemKind { Unknown, Bool, Int, UInt, Half, Float };

struct BuiltinTypeInfo {
  enum class ShapeKind { Unknown, Scalar, Vector, Matrix };
  ShapeKind shape = ShapeKind::Unknown;
  BuiltinElemKind elem = BuiltinElemKind::Unknown;
  int dim = 0;
  int rows = 0;
  int cols = 0;
};

struct BuiltinResolveResult {
  bool ok = false;
  bool warnMixedSignedness = false;
  bool indeterminate = false;
  BuiltinTypeInfo ret;
};

// Diagnostics-side expression typing contract.
// Responsibilities: normalize type tokens, classify literals, model builtin
// type rules, and infer expression result types for semantic diagnostics.
std::string normalizeTypeToken(std::string value);

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
    std::unordered_map<std::string, std::string> *fileTextCache);

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
    std::unordered_map<std::string, std::string> *fileTextCache);
