#pragma once

#include "semantic_cache.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct SemanticSnapshotFieldQueryResult {
  std::string type;
  int line = -1;
};

struct SemanticSnapshotStructFieldInfo {
  std::string name;
  std::string type;
  int line = -1;
};

struct SemanticSnapshotFunctionOverloadInfo {
  std::string label;
  std::vector<std::string> parameters;
  std::string returnType;
  int line = -1;
  int character = -1;
  bool hasBody = false;
};

bool querySemanticSnapshotFunctionSignature(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name, int lineIndex, int nameCharacter,
    std::string &labelOut, std::vector<std::string> &parametersOut);

bool querySemanticSnapshotFunctionOverloads(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name,
    std::vector<SemanticSnapshotFunctionOverloadInfo> &overloadsOut);

bool querySemanticSnapshotParameterTypeAtOffset(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name, size_t offset, std::string &typeOut);

bool querySemanticSnapshotLocalTypeAtOffset(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name, size_t offset, std::string &typeOut);

bool querySemanticSnapshotStructFields(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &structName, std::vector<std::string> &fieldsOut);

bool querySemanticSnapshotStructFieldInfos(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &structName,
    std::vector<SemanticSnapshotStructFieldInfo> &fieldsOut);

bool querySemanticSnapshotStructField(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &structName, const std::string &fieldName,
    SemanticSnapshotFieldQueryResult &resultOut);

bool querySemanticSnapshotGlobalType(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name, std::string &typeOut);

bool querySemanticSnapshotSymbolType(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    const std::string &name, std::string &typeOut);
