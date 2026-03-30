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

struct ParsedMacroDefinitionInfo {
  std::string name;
  std::vector<std::string> parameters;
  std::string replacementText;
  size_t nameStart = 0;
  size_t nameEnd = 0;
  bool isFunctionLike = false;
};

struct TrimmedCodeLineScanSharedResult {
  std::vector<std::string> trimmedLines;
  std::vector<int> parenDepthAfterLine;
  std::vector<int> bracketDepthAfterLine;
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

bool extractMetadataDeclarationHeaderShared(const std::string &line,
                                           std::string &typeOut,
                                           std::string &nameOut);

// Parses a metadata-block declaration header such as:
// - `float u_value`
// - `float4 u_uv_info : UVScale`
// and returns the declared symbol byte span on that header line.
bool findMetadataDeclarationHeaderPosShared(const std::string &line,
                                            std::string &typeOut,
                                            std::string &nameOut,
                                            size_t &nameStartOut,
                                            size_t &nameEndOut);

bool extractFxBlockDeclarationHeaderShared(const std::string &line,
                                           std::string &typeOut,
                                           std::string &nameOut);

bool extractMacroDefinitionInLineShared(const std::string &line,
                                        ParsedMacroDefinitionInfo &resultOut);

bool extractFunctionSignatureFromTextShared(
    const std::string &text, int lineIndex, int nameCharacter,
    const std::string &name, ParsedFunctionSignatureTextInfo &resultOut);

std::vector<std::string> extractDeclaredNamesFromLine(const std::string &line);

std::vector<ParsedDeclarationInfo>
extractDeclarationsInLineShared(const std::string &line);

// Splits text into logical lines while preserving a trailing empty line when the
// source ends with '\n'.
std::vector<std::string> splitLinesShared(const std::string &text);

// Returns comment/string-stripped, trimmed code lines with the same line count
// as the original text. The output is suitable for lightweight line heuristics.
std::vector<std::string> buildTrimmedCodeLinesShared(const std::string &text);

// Returns comment/string-stripped, trimmed code lines plus per-line
// parenthesis/bracket nesting state after scanning that line. When `lineActive`
// is provided, nesting only advances on active, non-preprocessor lines so
// diagnostics can ignore inactive branches without carrying their grouping
// state into active code.
TrimmedCodeLineScanSharedResult buildTrimmedCodeLineScanShared(
    const std::string &text, const std::vector<char> *lineActive = nullptr);

// Shared heuristic used by fast/full diagnostics to decide whether one visible
// code line likely needs a terminating semicolon. `insideOpenGroupingAfterLine`
// should be true when the shared line scan still sees the current line inside a
// multi-line `(`/`[` grouping after the line finishes.
bool shouldReportMissingSemicolonShared(const std::string &trimmed,
                                        const std::string &nextTrimmed,
                                        bool insideOpenGroupingAfterLine);

bool findTypeOfIdentifierInDeclarationLineShared(const std::string &line,
                                                 const std::string &identifier,
                                                 std::string &typeNameOut);
