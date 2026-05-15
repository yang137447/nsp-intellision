#pragma once
#include <string>

std::string uriDecode(const std::string &text);
std::string uriToPath(const std::string &uri);
std::string pathToUri(const std::string &path);

// Returns a stable comparison key for a file URI or path. The key is intended
// for identity checks only: it decodes file URIs, normalizes `.` / `..`
// segments, uses Windows separators, and lower-cases the result.
std::string normalizeUriComparisonKey(const std::string &uriOrPath);

// Compares two file URIs or paths using the same canonicalization rules as
// include-context replay. Empty inputs are never equivalent unless they are
// byte-identical.
bool uriEquivalent(const std::string &lhs, const std::string &rhs);
