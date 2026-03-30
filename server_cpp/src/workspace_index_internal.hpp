#pragma once

#include "workspace_index.hpp"

#include <filesystem>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// Internal workspace-index contract shared by split implementation files.
// Scope: persisted summary storage types plus helper functions used by the
// index facade, scan/path helpers, extraction helpers, reverse-include
// aggregation, and cache IO.
struct FileMeta {
  uint64_t mtime = 0;
  uint64_t size = 0;
  std::vector<IndexedDefinition> defs;
  std::vector<IndexedStruct> structs;
  std::vector<std::string> includes;
};

struct CachedFileStamp {
  std::string path;
  uint64_t mtime = 0;
  uint64_t size = 0;
};

struct IndexStore {
  std::string key;
  std::unordered_map<std::string, FileMeta> filesByPath;
  std::unordered_map<std::string, std::vector<IndexedDefinition>> defsBySymbol;
  std::unordered_map<std::string, std::vector<IndexedStructMember>>
      structMembersOrdered;
  std::unordered_map<std::string, DefinitionLocation> bestDefBySymbol;
  std::unordered_map<std::string, DefinitionLocation> bestStructDefByName;
  std::unordered_map<std::string, std::string> bestTypeBySymbol;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
      structMemberType;
};

bool readFileToString(const std::string &path, std::string &out);

std::string toLowerCopy(const std::string &value);

std::string normalizePathForCompare(const std::string &value);

bool isPathUnderOrEqual(const std::string &dir, const std::string &path);

bool extractIncludePathOutsideComments(const std::string &line,
                                       bool &inBlockCommentInOut,
                                       std::string &outIncludePath);

bool isAbsolutePath(const std::string &path);

std::string joinPath(const std::string &base, const std::string &child);

void addUnique(std::vector<std::string> &items, const std::string &value);

uint64_t fnv1a64(const std::string &text);

std::string hex16(uint64_t value);

uint64_t fnv1a64Append(uint64_t hash, const std::string &text);

std::string computeIndexKey(const std::vector<std::string> &folders,
                            const std::vector<std::string> &includePaths,
                            const std::vector<std::string> &shaderExtensions,
                            const std::string &modelsHash);

bool getModelsHash(std::string &hashOut, std::string &errorOut);

bool hasExtension(const fs::path &path,
                  const std::unordered_set<std::string> &exts);

bool buildCodeMaskForLine(const std::string &lineText, bool &inBlockCommentOut,
                          std::vector<char> &maskOut);

void collectIncludeClosureFiles(
    const std::string &entryPath,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    std::vector<std::string> &outPaths);

bool parseFileToMeta(const fs::path &path,
                     const std::unordered_set<std::string> &extSet,
                     const std::vector<std::string> &workspaceFolders,
                     const std::vector<std::string> &includePaths,
                     const std::vector<std::string> &extensions,
                     std::string &keyOut, FileMeta &metaOut);

void extractStructMembers(const std::string &uri, const std::string &text,
                          const std::vector<std::string> &workspaceFolders,
                          const std::vector<std::string> &includePaths,
                          const std::vector<std::string> &shaderExtensions,
                          std::vector<IndexedStruct> &structsOut);

void extractDefinitions(const std::string &uri, const std::string &text,
                        std::vector<IndexedDefinition> &defsOut);

void rebuildGlobals(IndexStore &store);

void buildReverseIncludes(
    const IndexStore &store,
    std::unordered_map<std::string, std::vector<std::string>> &outReverse);

Json serializeFileEntry(const std::string &path, const FileMeta &meta);

bool deserializeFileEntry(const Json &file, std::string &outPath,
                          FileMeta &outMeta);

Json serializeIndex(const IndexStore &store);

bool deserializeIndex(const Json &root, IndexStore &store);

void clearIndexCacheStorage(const std::vector<std::string> &workspaceFolders);

bool loadIndexStoreFromDisk(const std::string &key,
                            const std::vector<std::string> &workspaceFolders,
                            IndexStore &out);

void saveIndexStoreToDisk(const IndexStore &store,
                          const std::vector<std::string> &workspaceFolders);
