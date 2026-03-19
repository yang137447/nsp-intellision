#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct Document {
  std::string uri;
  std::string text;
  int version = 0;
  uint64_t epoch = 0;
};

std::string normalizeDocumentText(const std::string &text);

bool readFileText(const std::string &path, std::string &text);

bool loadDocumentText(
    const std::string &uri,
    const std::unordered_map<std::string, Document> &documents,
    std::string &text);

void prefetchDocumentTexts(
    const std::vector<std::string> &uris,
    const std::unordered_map<std::string, Document> &documents);

void setDocumentPrefetchMaxConcurrency(int value);

void primeDocumentTextCache(const std::string &uri, const std::string &text);

void invalidateDocumentTextCacheByUri(const std::string &uri);

void invalidateDocumentTextCacheByPath(const std::string &path);

void invalidateDocumentTextCacheByUris(const std::vector<std::string> &uris);
