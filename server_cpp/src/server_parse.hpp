#pragma once

#include <string>
#include <vector>

bool extractIncludePath(const std::string &lineText, std::string &includePath);

bool extractMemberAccessBase(const std::string &lineText, int character,
                             std::string &base);

bool extractStructNameInLine(const std::string &line,
                             std::string &structNameOut);

std::vector<std::string> extractDeclaredNamesFromLine(const std::string &line);

bool findTypeOfIdentifierInDeclarationLineShared(const std::string &line,
                                                 const std::string &identifier,
                                                 std::string &typeNameOut);
