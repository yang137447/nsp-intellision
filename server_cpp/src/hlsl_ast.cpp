#include "hlsl_ast.hpp"

#include "nsf_lexer.hpp"
#include "server_parse.hpp"
#include "text_utils.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

static ExpandedSource makeIdentityExpandedSource(const std::string &text) {
  ExpandedSource expanded;
  expanded.text = text;

  int lineIndex = 0;
  size_t lineStart = 0;
  if (text.empty())
    return expanded;
  while (lineStart < text.size()) {
    expanded.sourceMap.outputLineToSourceLine.push_back(lineIndex);
    const size_t lineEnd = text.find('\n', lineStart);
    if (lineEnd == std::string::npos)
      break;
    lineStart = lineEnd + 1;
    lineIndex++;
  }
  return expanded;
}

static int sourceLineForOutputLine(const ExpandedSource &expandedSource,
                                   int outputLine) {
  if (outputLine < 0)
    return outputLine;
  if (outputLine <
      static_cast<int>(expandedSource.sourceMap.outputLineToSourceLine.size())) {
    return expandedSource.sourceMap.outputLineToSourceLine[outputLine];
  }
  return outputLine;
}

static size_t lineStartOffsetForLine(const std::string &text, int targetLine) {
  if (targetLine <= 0)
    return 0;
  int line = 0;
  for (size_t i = 0; i < text.size(); i++) {
    if (text[i] != '\n')
      continue;
    line++;
    if (line == targetLine)
      return i + 1;
  }
  return text.size();
}

static int lineIndexForOffset(const std::string &text, size_t offset) {
  offset = std::min(offset, text.size());
  int line = 0;
  for (size_t i = 0; i < offset; i++) {
    if (text[i] == '\n')
      line++;
  }
  return line;
}

static void fillLocationFromExpandedOffset(const ExpandedSource &expandedSource,
                                           size_t offset, int &lineOut,
                                           int &characterOut) {
  offset = std::min(offset, expandedSource.text.size());
  const int outputLine = lineIndexForOffset(expandedSource.text, offset);
  const size_t lineStart =
      lineStartOffsetForLine(expandedSource.text, outputLine);
  const std::string lineText = getLineAt(expandedSource.text, outputLine);
  lineOut = sourceLineForOutputLine(expandedSource, outputLine);
  characterOut = byteOffsetInLineToUtf16(
      lineText, static_cast<int>(offset - std::min(lineStart, offset)));
}

static std::string trimCopy(std::string value) {
  value = trimLeftCopy(value);
  value = trimRightCopy(value);
  return value;
}

static std::string
extractReturnTypeFromFunctionLabel(const std::string &label,
                                   const std::string &name) {
  size_t namePos = label.rfind(name);
  if (namePos == std::string::npos)
    return std::string();
  std::string prefix = trimRightCopy(label.substr(0, namePos));
  return prefix;
}

static HlslAstParameter
parseParameterDecl(const std::string &text,
                   const ExpandedSource *expandedSource = nullptr,
                   size_t absoluteStartOffset = 0,
                   const std::string &displayText = std::string()) {
  HlslAstParameter parameter;
  parameter.text = displayText.empty() ? trimCopy(text) : displayText;
  if (parameter.text.empty() || parameter.text == "void")
    return parameter;

  auto tokens = lexLineTokens(text);
  std::string typeName;
  std::string name;
  const LexToken *nameToken = nullptr;
  int angleDepth = 0;
  int parenDepth = 0;
  int bracketDepth = 0;
  for (const auto &token : tokens) {
    const std::string &tokenText = token.text;
    if (tokenText == "<")
      angleDepth++;
    else if (tokenText == ">" && angleDepth > 0)
      angleDepth--;
    else if (tokenText == "(")
      parenDepth++;
    else if (tokenText == ")" && parenDepth > 0)
      parenDepth--;
    else if (tokenText == "[")
      bracketDepth++;
    else if (tokenText == "]" && bracketDepth > 0)
      bracketDepth--;
    if (angleDepth != 0 || parenDepth != 0 || bracketDepth != 0)
      continue;
    if (tokenText == ":" || tokenText == "=")
      break;
    if (token.kind != LexToken::Kind::Identifier)
      continue;
    if (isQualifierToken(tokenText))
      continue;
    if (typeName.empty()) {
      typeName = tokenText;
      continue;
    }
    name = tokenText;
    nameToken = &token;
  }

  parameter.type = typeName;
  parameter.name = name;
  if (nameToken && expandedSource) {
    parameter.offset = absoluteStartOffset + nameToken->start;
    fillLocationFromExpandedOffset(*expandedSource, parameter.offset,
                                   parameter.line, parameter.character);
  }
  return parameter;
}

static bool isLikelyFunctionNameToken(const std::vector<LexToken> &tokens,
                                      size_t index) {
  if (index + 1 >= tokens.size())
    return false;
  if (tokens[index].kind != LexToken::Kind::Identifier)
    return false;
  if (tokens[index + 1].kind != LexToken::Kind::Punct ||
      tokens[index + 1].text != "(") {
    return false;
  }
  const std::string &name = tokens[index].text;
  if (name == "if" || name == "for" || name == "while" || name == "switch" ||
      name == "return" || name == "struct" || name == "cbuffer" ||
      name == "typedef") {
    return false;
  }
  if (index > 0) {
    const std::string &prev = tokens[index - 1].text;
    if (prev == "." || prev == "->" || prev == "::" || prev == "=")
      return false;
  }
  for (size_t i = 0; i < index; i++) {
    if (tokens[i].kind == LexToken::Kind::Punct && tokens[i].text == "=")
      return false;
  }
  return true;
}

static bool parseFunctionDeclAtLine(const ExpandedSource &expandedSource,
                                    int outputLine,
                                    HlslAstFunctionDecl &functionOut) {
  functionOut = HlslAstFunctionDecl{};
  const std::string lineText = getLineAt(expandedSource.text, outputLine);
  const auto tokens = lexLineTokens(lineText);
  for (size_t i = 0; i + 1 < tokens.size(); i++) {
    if (!isLikelyFunctionNameToken(tokens, i))
      continue;

    const std::string &name = tokens[i].text;
    const int nameCharacter =
        byteOffsetInLineToUtf16(lineText, static_cast<int>(tokens[i].start));
    ParsedFunctionSignatureTextInfo signatureText;
    if (!extractFunctionSignatureFromTextShared(expandedSource.text, outputLine,
                                                nameCharacter, name,
                                                signatureText) ||
        signatureText.label.empty()) {
      continue;
    }

    functionOut.name = name;
    functionOut.line = sourceLineForOutputLine(expandedSource, outputLine);
    functionOut.character = nameCharacter;
    functionOut.label = std::move(signatureText.label);
    functionOut.returnType =
        extractReturnTypeFromFunctionLabel(functionOut.label, name);
    functionOut.signatureEndLine = sourceLineForOutputLine(
        expandedSource, signatureText.signatureEndLine >= 0
                            ? signatureText.signatureEndLine
                            : outputLine);
    functionOut.bodyStartLine =
        sourceLineForOutputLine(expandedSource, signatureText.bodyStartLine);
    functionOut.bodyEndLine =
        sourceLineForOutputLine(expandedSource, signatureText.bodyEndLine);
    functionOut.hasBody = signatureText.hasBody;
    functionOut.parameters.reserve(signatureText.parameters.size());
    for (size_t parameterIndex = 0;
         parameterIndex < signatureText.parameters.size(); parameterIndex++) {
      const std::string &displayParameter =
          signatureText.parameters[parameterIndex];
      if (parameterIndex < signatureText.parameterSpans.size()) {
        const auto &span = signatureText.parameterSpans[parameterIndex];
        functionOut.parameters.push_back(parseParameterDecl(
            span.text, &expandedSource, span.startOffset, displayParameter));
      } else {
        functionOut.parameters.push_back(parseParameterDecl(displayParameter));
      }
    }
    return true;
  }
  return false;
}

static void collectFieldDeclsFromLine(const ExpandedSource &expandedSource,
                                      const std::string &line, int outputLine,
                                      std::vector<HlslAstFieldDecl> &fieldsOut) {
  if (line.find(';') == std::string::npos || line.find('(') != std::string::npos)
    return;
  const auto declarations = extractDeclarationsInLineShared(line);
  const size_t lineStart =
      lineStartOffsetForLine(expandedSource.text, outputLine);
  for (const auto &decl : declarations) {
    HlslAstFieldDecl field;
    field.name = decl.name;
    field.type = decl.type;
    field.offset = lineStart + decl.start;
    fillLocationFromExpandedOffset(expandedSource, field.offset, field.line,
                                   field.character);
    fieldsOut.push_back(std::move(field));
  }
}

static bool findIdentifierSpanInLine(const std::string &line,
                                     const std::string &name,
                                     size_t &startOut, size_t &endOut) {
  if (name.empty())
    return false;
  size_t searchFrom = 0;
  while (searchFrom <= line.size()) {
    const size_t pos = line.find(name, searchFrom);
    if (pos == std::string::npos)
      return false;
    const size_t end = pos + name.size();
    const bool leftBoundary =
        pos == 0 || !isIdentifierChar(line[pos - 1]);
    const bool rightBoundary =
        end >= line.size() || !isIdentifierChar(line[end]);
    if (leftBoundary && rightBoundary) {
      startOut = pos;
      endOut = end;
      return true;
    }
    searchFrom = pos + 1;
  }
  return false;
}

static void fillGlobalLocationFromLine(const ExpandedSource &expandedSource,
                                       int outputLine, size_t nameStart,
                                       HlslAstGlobalVariableDecl &decl) {
  const size_t lineStart =
      lineStartOffsetForLine(expandedSource.text, outputLine);
  decl.offset = lineStart + nameStart;
  fillLocationFromExpandedOffset(expandedSource, decl.offset, decl.line,
                                 decl.character);
}

static void updateBraceDepthFromLine(const std::string &lineText, int &braceDepth,
                                     bool &inBlockComment) {
  for (size_t i = 0; i < lineText.size(); i++) {
    const char ch = lineText[i];
    const char next = i + 1 < lineText.size() ? lineText[i + 1] : '\0';
    if (inBlockComment) {
      if (ch == '*' && next == '/') {
        inBlockComment = false;
        i++;
      }
      continue;
    }
    if (ch == '/' && next == '*') {
      inBlockComment = true;
      i++;
      continue;
    }
    if (ch == '/' && next == '/')
      break;
    if (ch == '{') {
      braceDepth++;
    } else if (ch == '}' && braceDepth > 0) {
      braceDepth--;
    }
  }
}

static void appendTopLevelDecl(HlslAstDocument &document,
                               HlslTopLevelDeclKind kind,
                               const std::string &name, int sourceLine) {
  HlslAstTopLevelDecl decl;
  decl.kind = kind;
  decl.name = name;
  decl.line = sourceLine;
  document.topLevelDecls.push_back(std::move(decl));
}

} // namespace

HlslAstDocument buildHlslAstDocument(const std::string &text) {
  return buildHlslAstDocument(makeIdentityExpandedSource(text));
}

HlslAstDocument buildHlslAstDocument(const ExpandedSource &expandedSource) {
  HlslAstDocument document;
  document.expandedSource = expandedSource;

  std::istringstream stream(expandedSource.text);
  std::string lineText;
  int outputLine = 0;
  int braceDepth = 0;
  bool inBlockComment = false;
  enum class AggregateKind { None, Struct, CBuffer };
  AggregateKind activeAggregateKind = AggregateKind::None;
  size_t activeAggregateIndex = 0;
  int activeAggregateBodyDepth = -1;
  bool activeAggregateAwaitingBodyOpen = false;
  bool pendingUiMetadataDecl = false;
  bool inUiMetadataBlock = false;
  std::string pendingUiMetadataType;
  std::string pendingUiMetadataName;
  int pendingUiMetadataLine = -1;

  while (std::getline(stream, lineText)) {
    const int sourceLine = sourceLineForOutputLine(expandedSource, outputLine);
    const int braceDepthBeforeLine = braceDepth;
    const bool atTopLevel = braceDepth == 0;

    std::string trimmedLine = trimCopy(lineText);
    if (pendingUiMetadataDecl && atTopLevel && trimmedLine == "<") {
      HlslAstGlobalVariableDecl globalDecl;
      globalDecl.name = pendingUiMetadataName;
      globalDecl.type = pendingUiMetadataType;
      globalDecl.line = pendingUiMetadataLine >= 0 ? pendingUiMetadataLine
                                                   : sourceLine;
      size_t nameStart = 0;
      size_t nameEnd = 0;
      if (findIdentifierSpanInLine(lineText, globalDecl.name, nameStart,
                                   nameEnd)) {
        fillGlobalLocationFromLine(expandedSource, outputLine, nameStart,
                                   globalDecl);
      }
      document.globalVariables.push_back(globalDecl);
      appendTopLevelDecl(document, HlslTopLevelDeclKind::GlobalVariable,
                         globalDecl.name, globalDecl.line);
      inUiMetadataBlock = true;
      pendingUiMetadataDecl = false;
      pendingUiMetadataType.clear();
      pendingUiMetadataName.clear();
      pendingUiMetadataLine = -1;
      outputLine++;
      continue;
    }

    if (inUiMetadataBlock) {
      if (trimmedLine == ">" || trimmedLine.rfind(">", 0) == 0)
        inUiMetadataBlock = false;
      outputLine++;
      continue;
    }

    if (atTopLevel) {
      std::string includePath;
      if (extractIncludePath(lineText, includePath)) {
        HlslAstIncludeDecl includeDecl;
        includeDecl.path = includePath;
        includeDecl.line = sourceLine;
        document.includes.push_back(includeDecl);
        appendTopLevelDecl(document, HlslTopLevelDeclKind::Include,
                           includeDecl.path, sourceLine);
      }

      HlslAstFunctionDecl functionDecl;
      if (parseFunctionDeclAtLine(expandedSource, outputLine, functionDecl)) {
        document.functions.push_back(functionDecl);
        appendTopLevelDecl(document, HlslTopLevelDeclKind::Function,
                           functionDecl.name, functionDecl.line);
      } else {
        std::string structName;
        if (extractStructNameInLine(lineText, structName)) {
          HlslAstStructDecl structDecl;
          structDecl.name = structName;
          structDecl.line = sourceLine;
          structDecl.hasBody = lineText.find('{') != std::string::npos;
          document.structs.push_back(structDecl);
          appendTopLevelDecl(document, HlslTopLevelDeclKind::Struct,
                             structDecl.name, structDecl.line);
          activeAggregateKind = AggregateKind::Struct;
          activeAggregateIndex = document.structs.size() - 1;
          activeAggregateBodyDepth = braceDepthBeforeLine + 1;
          activeAggregateAwaitingBodyOpen = !structDecl.hasBody;
        } else {
          std::string cbufferName;
          if (extractCBufferNameInLineShared(lineText, cbufferName)) {
            HlslAstCBufferDecl cbufferDecl;
            cbufferDecl.name = cbufferName;
            cbufferDecl.line = sourceLine;
            cbufferDecl.hasBody = lineText.find('{') != std::string::npos;
            document.cbuffers.push_back(cbufferDecl);
            appendTopLevelDecl(document, HlslTopLevelDeclKind::CBuffer,
                               cbufferDecl.name, cbufferDecl.line);
            activeAggregateKind = AggregateKind::CBuffer;
            activeAggregateIndex = document.cbuffers.size() - 1;
            activeAggregateBodyDepth = braceDepthBeforeLine + 1;
            activeAggregateAwaitingBodyOpen = !cbufferDecl.hasBody;
          } else {
            HlslAstTypedefDecl typedefDecl;
            if (extractTypedefDeclInLineShared(lineText, typedefDecl.alias,
                                               typedefDecl.underlyingType)) {
              typedefDecl.line = sourceLine;
              document.typedefs.push_back(typedefDecl);
              appendTopLevelDecl(document, HlslTopLevelDeclKind::Typedef,
                                 typedefDecl.alias, typedefDecl.line);
            } else if (extractFxBlockDeclarationHeaderShared(
                           lineText, pendingUiMetadataType,
                           pendingUiMetadataName)) {
              HlslAstGlobalVariableDecl globalDecl;
              globalDecl.name = pendingUiMetadataName;
              globalDecl.type = pendingUiMetadataType;
              globalDecl.line = sourceLine;
              size_t nameStart = 0;
              size_t nameEnd = 0;
              if (findIdentifierSpanInLine(lineText, globalDecl.name,
                                           nameStart, nameEnd)) {
                fillGlobalLocationFromLine(expandedSource, outputLine,
                                           nameStart, globalDecl);
              }
              document.globalVariables.push_back(globalDecl);
              appendTopLevelDecl(document, HlslTopLevelDeclKind::GlobalVariable,
                                 globalDecl.name, globalDecl.line);
            } else if (extractMetadataDeclarationHeaderShared(
                           lineText, pendingUiMetadataType,
                           pendingUiMetadataName)) {
              pendingUiMetadataDecl = true;
              pendingUiMetadataLine = sourceLine;
            } else if (lineText.find(';') != std::string::npos) {
              const auto declarations = extractDeclarationsInLineShared(lineText);
              const size_t lineStart =
                  lineStartOffsetForLine(expandedSource.text, outputLine);
              for (const auto &decl : declarations) {
                HlslAstGlobalVariableDecl globalDecl;
                globalDecl.name = decl.name;
                globalDecl.type = decl.type;
                globalDecl.offset = lineStart + decl.start;
                fillLocationFromExpandedOffset(
                    expandedSource, globalDecl.offset, globalDecl.line,
                    globalDecl.character);
                document.globalVariables.push_back(globalDecl);
                appendTopLevelDecl(document,
                                   HlslTopLevelDeclKind::GlobalVariable,
                                   globalDecl.name, globalDecl.line);
              }
            }
          }
        }
      }
    }

    if (activeAggregateKind == AggregateKind::Struct &&
        !activeAggregateAwaitingBodyOpen &&
        braceDepthBeforeLine == activeAggregateBodyDepth &&
        activeAggregateIndex < document.structs.size()) {
      std::string includePath;
      if (extractIncludePath(lineText, includePath)) {
        HlslAstIncludeDecl includeDecl;
        includeDecl.path = includePath;
        includeDecl.line = sourceLine;
        document.structs[activeAggregateIndex].inlineIncludes.push_back(
            std::move(includeDecl));
      } else {
        collectFieldDeclsFromLine(expandedSource, lineText, outputLine,
                                  document.structs[activeAggregateIndex].fields);
      }
    } else if (activeAggregateKind == AggregateKind::CBuffer &&
               !activeAggregateAwaitingBodyOpen &&
               braceDepthBeforeLine == activeAggregateBodyDepth &&
               activeAggregateIndex < document.cbuffers.size()) {
      collectFieldDeclsFromLine(expandedSource, lineText, outputLine,
                                document.cbuffers[activeAggregateIndex].fields);
    }

    updateBraceDepthFromLine(lineText, braceDepth, inBlockComment);
    if (activeAggregateKind != AggregateKind::None &&
        activeAggregateAwaitingBodyOpen) {
      if (braceDepth >= activeAggregateBodyDepth) {
        activeAggregateAwaitingBodyOpen = false;
      } else if (trimmedLine.find(';') != std::string::npos) {
        activeAggregateKind = AggregateKind::None;
        activeAggregateIndex = 0;
        activeAggregateBodyDepth = -1;
        activeAggregateAwaitingBodyOpen = false;
      }
    }
    if (activeAggregateKind != AggregateKind::None &&
        !activeAggregateAwaitingBodyOpen &&
        braceDepth < activeAggregateBodyDepth) {
      activeAggregateKind = AggregateKind::None;
      activeAggregateIndex = 0;
      activeAggregateBodyDepth = -1;
      activeAggregateAwaitingBodyOpen = false;
    }
    outputLine++;
  }

  return document;
}
