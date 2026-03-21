#pragma once

#include "expanded_source.hpp"

#include <string>
#include <vector>

enum class HlslTopLevelDeclKind {
  Unknown,
  Include,
  Function,
  Struct,
  CBuffer,
  Typedef,
  GlobalVariable,
};

struct HlslAstFieldDecl {
  std::string name;
  std::string type;
  int line = -1;
};

struct HlslAstParameter {
  std::string text;
  std::string type;
  std::string name;
};

struct HlslAstIncludeDecl {
  std::string path;
  int line = -1;
};

struct HlslAstFunctionDecl {
  std::string name;
  int line = -1;
  int character = -1;
  std::string label;
  std::string returnType;
  std::vector<HlslAstParameter> parameters;
  int signatureEndLine = -1;
  int bodyStartLine = -1;
  int bodyEndLine = -1;
  bool hasBody = false;
};

struct HlslAstStructDecl {
  std::string name;
  int line = -1;
  bool hasBody = false;
  std::vector<HlslAstFieldDecl> fields;
  std::vector<HlslAstIncludeDecl> inlineIncludes;
};

struct HlslAstCBufferDecl {
  std::string name;
  int line = -1;
  bool hasBody = false;
  std::vector<HlslAstFieldDecl> fields;
};

struct HlslAstTypedefDecl {
  std::string alias;
  std::string underlyingType;
  int line = -1;
};

struct HlslAstGlobalVariableDecl {
  std::string name;
  std::string type;
  int line = -1;
};

struct HlslAstTopLevelDecl {
  HlslTopLevelDeclKind kind = HlslTopLevelDeclKind::Unknown;
  std::string name;
  int line = -1;
};

struct HlslAstDocument {
  ExpandedSource expandedSource;
  std::vector<HlslAstTopLevelDecl> topLevelDecls;
  std::vector<HlslAstIncludeDecl> includes;
  std::vector<HlslAstFunctionDecl> functions;
  std::vector<HlslAstStructDecl> structs;
  std::vector<HlslAstCBufferDecl> cbuffers;
  std::vector<HlslAstTypedefDecl> typedefs;
  std::vector<HlslAstGlobalVariableDecl> globalVariables;
};

HlslAstDocument buildHlslAstDocument(const std::string &text);

HlslAstDocument buildHlslAstDocument(const ExpandedSource &expandedSource);
