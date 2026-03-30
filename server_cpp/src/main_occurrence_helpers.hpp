#pragma once

#include "preprocessor_view.hpp"
#include "server_documents.hpp"
#include "server_occurrences.hpp"

#include <string>
#include <unordered_map>
#include <vector>

// Entry-layer occurrence/definition helper surface.
// Responsibilities: locate local declarations and collect occurrence sets for
// references/rename flows without changing request-layer behavior.
bool findMacroDefinitionInLine(const std::string &line, const std::string &word,
                               size_t &posOut);

bool findDeclaredIdentifierInDeclarationLine(const std::string &line,
                                             const std::string &word,
                                             size_t &posOut);

bool findParameterDeclarationInText(const std::string &text,
                                    const std::string &word, int &defLine,
                                    int &defStart, int &defEnd);

bool findFxBlockDeclarationInLine(const std::string &line,
                                  const std::string &word, size_t &posOut);

bool findNamedDefinitionInText(const std::string &text, const std::string &word,
                               const PreprocessorView *preprocessorView,
                               int &defLine, int &defStart, int &defEnd);

bool findNamedDefinitionInText(const std::string &text, const std::string &word,
                               int &defLine, int &defStart, int &defEnd);

std::vector<LocatedOccurrence> collectOccurrencesForSymbol(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines);

std::vector<LocatedOccurrence> collectOccurrencesForSymbol(
    const std::string &uri, const std::string &word,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions);
