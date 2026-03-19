#include "fast_ast.hpp"

#include "ast_signature_index.hpp"
#include "nsf_lexer.hpp"
#include "text_utils.hpp"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace {

struct FastAstLocalTypeEntry {
  std::string name;
  std::string type;
  size_t offset = 0;
  int depth = 0;
};

struct FastAstCacheEntry {
  uint64_t epoch = 0;
  uint64_t fingerprint = 0;
  bool hasEpoch = false;
  std::vector<AstFunctionSignatureEntry> functions;
  std::unordered_map<std::string, std::vector<size_t>> byName;
  std::vector<FastAstLocalTypeEntry> locals;
  std::unordered_map<std::string, std::vector<size_t>> localsByName;
};

std::mutex gFastAstMutex;
std::unordered_map<std::string, FastAstCacheEntry> gFastAstByUri;
FastAstMetricsSnapshot gFastAstMetrics;

uint64_t hashTextFnv1a(const std::string &text) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char ch : text) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

FastAstCacheEntry buildFastAst(const std::string &text) {
  FastAstCacheEntry built;
  std::istringstream stream(text);
  std::string lineText;
  size_t lineStartOffset = 0;
  int currentDepth = 0;
  bool inBlockComment = false;
  indexFunctionSignatures(text, built.functions, built.byName);
  while (std::getline(stream, lineText)) {
    std::string code = lineText;
    size_t lineComment = code.find("//");
    if (lineComment != std::string::npos)
      code = code.substr(0, lineComment);

    if (!code.empty() && code.find(';') != std::string::npos &&
        code.find('(') == std::string::npos) {
      const auto declTokens = lexLineTokens(code);
      std::string baseType;
      int angleDepth = 0;
      int parenDepth = 0;
      int bracketDepth = 0;
      for (size_t i = 0; i < declTokens.size(); i++) {
        const auto &tok = declTokens[i];
        const std::string &tokenText = tok.text;
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
        if (tok.kind != LexToken::Kind::Identifier)
          continue;
        if (isQualifierToken(tokenText))
          continue;
        if (baseType.empty()) {
          baseType = tokenText;
          continue;
        }
        bool isMemberName = false;
        if (i > 0) {
          const std::string &prev = declTokens[i - 1].text;
          if (prev == "." || prev == "->" || prev == "::")
            isMemberName = true;
        }
        if (isMemberName)
          continue;
        FastAstLocalTypeEntry local;
        local.name = tokenText;
        local.type = baseType;
        local.offset = lineStartOffset + tok.start;
        local.depth = currentDepth;
        const size_t localIndex = built.locals.size();
        built.locals.push_back(std::move(local));
        built.localsByName[tokenText].push_back(localIndex);
      }
    }

    for (size_t i = 0; i < lineText.size(); i++) {
      char ch = lineText[i];
      char next = i + 1 < lineText.size() ? lineText[i + 1] : '\0';
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
      if (ch == '{') {
        currentDepth++;
      } else if (ch == '}' && currentDepth > 0) {
        currentDepth--;
      }
    }
    lineStartOffset += lineText.size() + 1;
  }
  return built;
}

bool findSignatureInCache(const FastAstCacheEntry &entry,
                          const std::string &name, int lineIndex,
                          int nameCharacter, std::string &labelOut,
                          std::vector<std::string> &parametersOut) {
  auto it = entry.byName.find(name);
  if (it == entry.byName.end() || it->second.empty())
    return false;
  const auto &indices = it->second;
  for (size_t index : indices) {
    const auto &item = entry.functions[index];
    if (item.line == lineIndex && item.character == nameCharacter) {
      labelOut = item.label;
      parametersOut = item.parameters;
      return true;
    }
  }
  for (size_t index : indices) {
    const auto &item = entry.functions[index];
    if (item.line == lineIndex) {
      labelOut = item.label;
      parametersOut = item.parameters;
      return true;
    }
  }
  const auto &fallback = entry.functions[indices.front()];
  labelOut = fallback.label;
  parametersOut = fallback.parameters;
  return true;
}

bool findLocalTypeInCache(const FastAstCacheEntry &entry,
                          const std::string &name, size_t maxOffset,
                          std::string &typeNameOut) {
  auto it = entry.localsByName.find(name);
  if (it == entry.localsByName.end() || it->second.empty())
    return false;
  const FastAstLocalTypeEntry *best = nullptr;
  for (size_t index : it->second) {
    const auto &local = entry.locals[index];
    if (local.offset > maxOffset)
      continue;
    if (!best || local.depth > best->depth ||
        (local.depth == best->depth && local.offset >= best->offset)) {
      best = &local;
    }
  }
  if (!best)
    return false;
  typeNameOut = best->type;
  return true;
}

} // namespace

bool queryFastAstFunctionSignature(const std::string &uri,
                                   const std::string &text, uint64_t epoch,
                                   const std::string &name, int lineIndex,
                                   int nameCharacter, std::string &labelOut,
                                   std::vector<std::string> &parametersOut) {
  labelOut.clear();
  parametersOut.clear();
  if (uri.empty() || name.empty())
    return false;

  const uint64_t fingerprint = hashTextFnv1a(text);
  bool needRebuild = false;
  {
    std::lock_guard<std::mutex> lock(gFastAstMutex);
    gFastAstMetrics.lookups++;
    auto it = gFastAstByUri.find(uri);
    if (it == gFastAstByUri.end()) {
      needRebuild = true;
    } else {
      const bool epochMatches =
          it->second.hasEpoch && epoch > 0 && it->second.epoch == epoch;
      const bool textMatches = it->second.fingerprint == fingerprint;
      if (!epochMatches && !textMatches) {
        needRebuild = true;
      } else {
        gFastAstMetrics.cacheReused++;
      }
    }
  }

  if (needRebuild) {
    FastAstCacheEntry rebuilt = buildFastAst(text);
    rebuilt.epoch = epoch;
    rebuilt.hasEpoch = epoch > 0;
    rebuilt.fingerprint = fingerprint;
    std::lock_guard<std::mutex> lock(gFastAstMutex);
    gFastAstMetrics.rebuilds++;
    gFastAstMetrics.functionsIndexed += rebuilt.functions.size();
    gFastAstByUri[uri] = std::move(rebuilt);
  }

  std::lock_guard<std::mutex> lock(gFastAstMutex);
  auto it = gFastAstByUri.find(uri);
  if (it == gFastAstByUri.end())
    return false;
  if (!findSignatureInCache(it->second, name, lineIndex, nameCharacter,
                            labelOut, parametersOut)) {
    return false;
  }
  gFastAstMetrics.cacheHits++;
  return true;
}

bool queryFastAstLocalType(const std::string &uri, const std::string &text,
                           uint64_t epoch, const std::string &name,
                           size_t maxOffset, std::string &typeNameOut) {
  typeNameOut.clear();
  if (uri.empty() || name.empty())
    return false;

  const uint64_t fingerprint = hashTextFnv1a(text);
  bool needRebuild = false;
  {
    std::lock_guard<std::mutex> lock(gFastAstMutex);
    gFastAstMetrics.lookups++;
    auto it = gFastAstByUri.find(uri);
    if (it == gFastAstByUri.end()) {
      needRebuild = true;
    } else {
      const bool epochMatches =
          it->second.hasEpoch && epoch > 0 && it->second.epoch == epoch;
      const bool textMatches = it->second.fingerprint == fingerprint;
      if (!epochMatches && !textMatches) {
        needRebuild = true;
      } else {
        gFastAstMetrics.cacheReused++;
      }
    }
  }
  if (needRebuild) {
    FastAstCacheEntry rebuilt = buildFastAst(text);
    rebuilt.epoch = epoch;
    rebuilt.hasEpoch = epoch > 0;
    rebuilt.fingerprint = fingerprint;
    std::lock_guard<std::mutex> lock(gFastAstMutex);
    gFastAstMetrics.rebuilds++;
    gFastAstMetrics.functionsIndexed += rebuilt.functions.size();
    gFastAstByUri[uri] = std::move(rebuilt);
  }

  std::lock_guard<std::mutex> lock(gFastAstMutex);
  auto it = gFastAstByUri.find(uri);
  if (it == gFastAstByUri.end())
    return false;
  if (!findLocalTypeInCache(it->second, name, maxOffset, typeNameOut))
    return false;
  gFastAstMetrics.cacheHits++;
  return true;
}

void invalidateFastAstByUri(const std::string &uri) {
  if (uri.empty())
    return;
  std::lock_guard<std::mutex> lock(gFastAstMutex);
  gFastAstByUri.erase(uri);
}

void invalidateFastAstByUris(const std::vector<std::string> &uris) {
  if (uris.empty())
    return;
  std::lock_guard<std::mutex> lock(gFastAstMutex);
  for (const auto &uri : uris) {
    if (!uri.empty())
      gFastAstByUri.erase(uri);
  }
}

FastAstMetricsSnapshot takeFastAstMetricsSnapshot() {
  std::lock_guard<std::mutex> lock(gFastAstMutex);
  FastAstMetricsSnapshot snapshot = gFastAstMetrics;
  gFastAstMetrics = FastAstMetricsSnapshot{};
  return snapshot;
}
