#pragma once

#include <string>
#include <vector>

struct ParsedDeclarationInfo {
  std::string type;
  std::string name;
  size_t start = 0;
  size_t end = 0;
};

struct ParsedFunctionSignatureTextInfo {
  std::string label;
  std::vector<std::string> parameters;
  int signatureEndLine = -1;
  int bodyStartLine = -1;
  int bodyEndLine = -1;
  bool hasBody = false;
};

bool extractIncludePath(const std::string &lineText, std::string &includePath);

bool extractMemberAccessBase(const std::string &lineText, int character,
                             std::string &base);

bool extractStructNameInLine(const std::string &line,
                             std::string &structNameOut);

bool extractCBufferNameInLineShared(const std::string &line,
                                    std::string &nameOut);

bool extractTypedefDeclInLineShared(const std::string &line,
                                    std::string &aliasOut,
                                    std::string &underlyingTypeOut);

bool extractUiMetadataDeclarationHeaderShared(const std::string &line,
                                              std::string &typeOut,
                                              std::string &nameOut);

bool extractFunctionSignatureFromTextShared(
    const std::string &text, int lineIndex, int nameCharacter,
    const std::string &name, ParsedFunctionSignatureTextInfo &resultOut);

std::vector<std::string> extractDeclaredNamesFromLine(const std::string &line);

std::vector<ParsedDeclarationInfo>
extractDeclarationsInLineShared(const std::string &line);

bool findTypeOfIdentifierInDeclarationLineShared(const std::string &line,
                                                 const std::string &identifier,
                                                 std::string &typeNameOut);
