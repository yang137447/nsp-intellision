#pragma once

#include "definition_location.hpp"
#include "nsf_lexer.hpp"

#include <string>
#include <unordered_map>
#include <vector>

struct StructTypeCache {
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      membersByStruct;
  std::unordered_map<std::string, bool> attempted;
};

struct SymbolTypeCache {
  std::unordered_map<std::string, std::string> typeBySymbol;
  std::unordered_map<std::string, bool> attempted;
  std::unordered_map<std::string, bool> attemptedInText;
};

// Diagnostics-local symbol/struct type helpers.
// Responsibilities: discover struct declarations/member types and resolve
// visible symbol types from current text or workspace summary caches.
bool hasStructDeclarationInText(const std::string &text,
                                const std::string &structName);

std::string resolveSymbolTypeInText(const std::string &text,
                                    const std::string &symbol);

std::string resolveSymbolTypeByWorkspaceSummary(const std::string &symbol,
                                                SymbolTypeCache &cache);

std::string resolveVisibleSymbolType(
    const std::string &symbol,
    const std::unordered_map<std::string, std::string> &locals,
    const std::string &currentText, SymbolTypeCache &cache);

std::string resolveStructMemberType(
    const std::string &structName, const std::string &memberName,
    const std::string &currentUri, const std::string &currentText,
    const std::vector<std::string> &scanRoots,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &scanExtensions,
    const std::unordered_map<std::string, int> &defines,
    StructTypeCache &cache);
