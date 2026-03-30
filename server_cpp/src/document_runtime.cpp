#include "document_runtime.hpp"

#include "active_unit.hpp"
#include "include_resolver.hpp"
#include "preprocessor_view.hpp"
#include "resource_registry.hpp"
#include "server_documents.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

std::mutex gDocumentRuntimeMutex;
std::unordered_map<std::string, DocumentRuntime> gDocumentRuntimes;
std::atomic<uint64_t> gWorkspaceSummaryVersion{1};

struct RuntimeContextFingerprints {
  std::string workspaceFoldersFingerprint;
  std::string definesFingerprint;
  std::string includePathsFingerprint;
  std::string shaderExtensionsFingerprint;
};

struct ResourceFileStamp {
  std::string normalizedPath;
  bool exists = false;
  uintmax_t fileSize = 0;
  std::filesystem::file_time_type lastWriteTime{};
};

uint64_t fnv1aStep(uint64_t hash, const std::string &value) {
  for (unsigned char ch : value) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string toHex(uint64_t value) {
  std::ostringstream oss;
  oss << std::hex << value;
  return oss.str();
}

std::string fingerprintStringList(const std::vector<std::string> &values) {
  std::vector<std::string> ordered = values;
  std::sort(ordered.begin(), ordered.end());
  uint64_t hash = 1469598103934665603ull;
  for (const auto &value : ordered) {
    hash = fnv1aStep(hash, std::to_string(value.size()));
    hash = fnv1aStep(hash, ":");
    hash = fnv1aStep(hash, value);
    hash = fnv1aStep(hash, ";");
  }
  return toHex(hash);
}

std::string
fingerprintDefines(const std::unordered_map<std::string, int> &defines) {
  std::vector<std::pair<std::string, int>> ordered(defines.begin(),
                                                   defines.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.first < rhs.first;
            });
  uint64_t hash = 1469598103934665603ull;
  for (const auto &entry : ordered) {
    hash = fnv1aStep(hash, entry.first);
    hash = fnv1aStep(hash, "=");
    hash = fnv1aStep(hash, std::to_string(entry.second));
    hash = fnv1aStep(hash, ";");
  }
  return toHex(hash);
}

RuntimeContextFingerprints
buildRuntimeContextFingerprints(const DocumentRuntimeUpdateOptions &options) {
  RuntimeContextFingerprints fingerprints;
  fingerprints.workspaceFoldersFingerprint =
      fingerprintStringList(options.workspaceFolders);
  fingerprints.definesFingerprint = fingerprintDefines(options.defines);
  fingerprints.includePathsFingerprint =
      fingerprintStringList(options.includePaths);
  fingerprints.shaderExtensionsFingerprint =
      fingerprintStringList(options.shaderExtensions);
  return fingerprints;
}

ResourceFileStamp makeResourceFileStamp(const std::filesystem::path &path) {
  ResourceFileStamp stamp;
  stamp.normalizedPath = path.lexically_normal().string();
  std::error_code ec;
  stamp.exists = std::filesystem::exists(path, ec);
  if (ec || !stamp.exists)
    return stamp;
  stamp.fileSize = std::filesystem::file_size(path, ec);
  if (ec)
    stamp.fileSize = 0;
  stamp.lastWriteTime = std::filesystem::last_write_time(path, ec);
  if (ec)
    stamp.lastWriteTime = std::filesystem::file_time_type{};
  return stamp;
}

bool resourceFileStampsEqual(const ResourceFileStamp &lhs,
                             const ResourceFileStamp &rhs) {
  return lhs.normalizedPath == rhs.normalizedPath && lhs.exists == rhs.exists &&
         lhs.fileSize == rhs.fileSize &&
         lhs.lastWriteTime == rhs.lastWriteTime;
}

std::vector<std::string>
collectActiveUnitIncludeClosure(const std::string &rootUri,
                                const std::string &rootText,
                                const DocumentRuntimeUpdateOptions &options) {
  if (rootUri.empty() || rootText.empty())
    return {};

  PreprocessorIncludeContext includeContext;
  includeContext.currentUri = rootUri;
  includeContext.workspaceFolders = options.workspaceFolders;
  includeContext.includePaths = options.includePaths;
  includeContext.shaderExtensions = options.shaderExtensions;
  includeContext.loadText = [](const std::string &uri,
                               std::string &textOut) -> bool {
    const std::string path = uriToPath(uri);
    return !path.empty() && readFileText(path, textOut);
  };
  PreprocessorView preprocessorView =
      buildPreprocessorView(rootText, options.defines, includeContext);
  return preprocessorView.activeIncludeUris;
}

std::string buildActiveUnitBranchFingerprint(
    const std::string &activeUnitUri, const std::string &activeUnitText,
    const DocumentRuntimeUpdateOptions &options) {
  if (activeUnitText.empty())
    return std::string();
  PreprocessorIncludeContext includeContext;
  includeContext.currentUri = activeUnitUri;
  includeContext.workspaceFolders = options.workspaceFolders;
  includeContext.includePaths = options.includePaths;
  includeContext.shaderExtensions = options.shaderExtensions;
  const PreprocessorView preprocessorView =
      buildPreprocessorView(activeUnitText, options.defines, includeContext);
  std::ostringstream branchFingerprint;
  branchFingerprint << preprocessorView.lineActive.size() << "|";
  for (size_t line = 0; line < preprocessorView.branchSigs.size(); line++) {
    branchFingerprint << line << ":";
    for (const auto &entry : preprocessorView.branchSigs[line]) {
      branchFingerprint << entry.first << "," << entry.second << ";";
    }
    branchFingerprint << "|";
  }
  return branchFingerprint.str();
}

std::string stripWhitespaceForSemanticNeutralCompare(const std::string &text) {
  std::string stripped;
  stripped.reserve(text.size());
  for (unsigned char ch : text) {
    if (!std::isspace(ch))
      stripped.push_back(static_cast<char>(ch));
  }
  return stripped;
}

bool isSemanticNeutralEditAgainstPrevious(
    const std::string &oldText, const std::string &newText,
    const std::vector<ChangedRange> &changedRanges) {
  if (changedRanges.empty() || changedRanges.size() > 8)
    return false;
  int inspectedLines = 0;
  for (const auto &range : changedRanges) {
    const int oldStartLine = std::max(0, range.startLine);
    const int oldEndLine = std::max(oldStartLine, range.endLine);
    const int newStartLine = std::max(0, range.startLine);
    const int newEndLine = std::max(newStartLine, range.startLine + range.newEndLine);
    inspectedLines += (oldEndLine - oldStartLine + 1);
    inspectedLines += (newEndLine - newStartLine + 1);
    if (inspectedLines > 24)
      return false;

    const size_t oldStartOffset =
        positionToOffsetUtf16(oldText, range.startLine, range.startCharacter);
    const size_t oldEndOffset =
        positionToOffsetUtf16(oldText, range.endLine, range.endCharacter);
    const size_t newStartOffset =
        positionToOffsetUtf16(newText, range.startLine, range.startCharacter);
    const size_t newEndOffset = positionToOffsetUtf16(
        newText, range.startLine + range.newEndLine, range.newEndCharacter);

    const size_t oldLo = std::min(oldStartOffset, oldEndOffset);
    const size_t oldHi = std::min(std::max(oldStartOffset, oldEndOffset),
                                  oldText.size());
    const size_t newLo = std::min(newStartOffset, newEndOffset);
    const size_t newHi = std::min(std::max(newStartOffset, newEndOffset),
                                  newText.size());

    const std::string oldWindow = oldText.substr(oldLo, oldHi - oldLo);
    const std::string newWindow = newText.substr(newLo, newHi - newLo);
    if (stripWhitespaceForSemanticNeutralCompare(oldWindow) !=
        stripWhitespaceForSemanticNeutralCompare(newWindow)) {
      return false;
    }
  }
  return inspectedLines > 0;
}

bool isSyntaxOnlyChar(char ch) {
  switch (ch) {
  case ' ':
  case '\t':
  case '\r':
  case '\n':
  case '(':
  case ')':
  case '[':
  case ']':
  case '{':
  case '}':
  case ';':
  case ',':
  case '.':
  case '#':
  case '/':
  case '*':
  case ':':
  case '<':
  case '>':
  case '!':
  case '=':
  case '+':
  case '-':
    return true;
  default:
    return false;
  }
}

bool windowContainsOnlySyntaxChars(const std::string &text) {
  return std::all_of(text.begin(), text.end(),
                     [](char ch) { return isSyntaxOnlyChar(ch); });
}

bool lineLooksSemanticNeutral(const std::string &lineText) {
  const std::string trimmed = trimRightCopy(trimLeftCopy(lineText));
  if (trimmed.empty())
    return true;
  return trimmed.rfind("//", 0) == 0 || trimmed.rfind("/*", 0) == 0 ||
         trimmed.rfind("*", 0) == 0 || trimmed.rfind("*/", 0) == 0;
}

bool isCommentOnlyEditInNewText(const std::string &newText,
                                const std::vector<ChangedRange> &changedRanges) {
  if (changedRanges.empty() || changedRanges.size() > 8)
    return false;
  int inspectedLines = 0;
  for (const auto &range : changedRanges) {
    const int startLine = std::max(0, range.startLine);
    const int endLine = std::max(startLine, range.startLine + range.newEndLine);
    inspectedLines += (endLine - startLine + 1);
    if (inspectedLines > 24)
      return false;
    for (int line = startLine; line <= endLine; line++) {
      if (!lineLooksSemanticNeutral(getLineAt(newText, line)))
        return false;
    }
  }
  return inspectedLines > 0;
}

bool isSyntaxOnlyEditAgainstPrevious(const std::string &oldText,
                                     const std::string &newText,
                                     const std::vector<ChangedRange> &changedRanges) {
  if (changedRanges.empty() || changedRanges.size() > 8)
    return false;
  int inspectedLines = 0;
  for (const auto &range : changedRanges) {
    const int startLine = std::max(0, range.startLine);
    const int oldEndLine = std::max(startLine, range.endLine);
    const int newEndLine = std::max(startLine, range.startLine + range.newEndLine);
    inspectedLines += (oldEndLine - startLine + 1);
    inspectedLines += (newEndLine - startLine + 1);
    if (inspectedLines > 24)
      return false;

    const size_t oldStartOffset =
        positionToOffsetUtf16(oldText, range.startLine, range.startCharacter);
    const size_t oldEndOffset =
        positionToOffsetUtf16(oldText, range.endLine, range.endCharacter);
    const size_t newStartOffset =
        positionToOffsetUtf16(newText, range.startLine, range.startCharacter);
    const size_t newEndOffset = positionToOffsetUtf16(
        newText, range.startLine + range.newEndLine, range.newEndCharacter);

    const size_t oldLo = std::min(oldStartOffset, oldEndOffset);
    const size_t oldHi = std::min(std::max(oldStartOffset, oldEndOffset),
                                  oldText.size());
    const size_t newLo = std::min(newStartOffset, newEndOffset);
    const size_t newHi = std::min(std::max(newStartOffset, newEndOffset),
                                  newText.size());

    if (!windowContainsOnlySyntaxChars(oldText.substr(oldLo, oldHi - oldLo)) ||
        !windowContainsOnlySyntaxChars(newText.substr(newLo, newHi - newLo))) {
      return false;
    }
  }
  return inspectedLines > 0;
}

ActiveUnitSnapshot buildActiveUnitSnapshot(
    const DocumentRuntimeUpdateOptions &options,
    const std::string &activeUnitUri, const std::string &activeUnitPath,
    const RuntimeContextFingerprints &contextFingerprints) {
  ActiveUnitSnapshot snapshot;
  snapshot.uri = activeUnitUri;
  snapshot.path = activeUnitPath;
  snapshot.documentVersion = options.activeUnitDocumentVersion;
  snapshot.documentEpoch = options.activeUnitDocumentEpoch;
  snapshot.workspaceFolders = options.workspaceFolders;
  snapshot.includePaths = options.includePaths;
  snapshot.shaderExtensions = options.shaderExtensions;
  snapshot.defines = options.defines;
  snapshot.workspaceFoldersFingerprint =
      contextFingerprints.workspaceFoldersFingerprint;
  snapshot.definesFingerprint = contextFingerprints.definesFingerprint;
  snapshot.includePathsFingerprint =
      contextFingerprints.includePathsFingerprint;
  snapshot.shaderExtensionsFingerprint =
      contextFingerprints.shaderExtensionsFingerprint;
  snapshot.workspaceSummaryVersion = options.workspaceSummaryVersion;
  std::string activeUnitText = options.activeUnitText;
  if (activeUnitText.empty() && !snapshot.path.empty())
    readFileText(snapshot.path, activeUnitText);
  snapshot.includeClosureUris =
      collectActiveUnitIncludeClosure(snapshot.uri, activeUnitText, options);
  snapshot.includeClosureFingerprint =
      fingerprintStringList(snapshot.includeClosureUris);
  snapshot.activeBranchFingerprint =
      buildActiveUnitBranchFingerprint(snapshot.uri, activeUnitText, options);
  return snapshot;
}

uint64_t currentTimeMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool isSnapshotStillCurrent(const AnalysisSnapshotKey &candidate,
                            const AnalysisSnapshotKey &current) {
  return candidate.fullFingerprint == current.fullFingerprint;
}

bool isSnapshotStaleEligible(const AnalysisSnapshotKey &candidate,
                             const AnalysisSnapshotKey &current) {
  return candidate.stableContextFingerprint == current.stableContextFingerprint;
}

bool activeUnitContextMatches(
    const ActiveUnitSnapshot &snapshot, const std::string &activeUnitUri,
    const std::string &activeUnitPath,
    const RuntimeContextFingerprints &contextFingerprints,
    const DocumentRuntimeUpdateOptions &options) {
  return snapshot.uri == activeUnitUri && snapshot.path == activeUnitPath &&
         snapshot.documentVersion == options.activeUnitDocumentVersion &&
         snapshot.documentEpoch == options.activeUnitDocumentEpoch &&
         snapshot.workspaceSummaryVersion == options.workspaceSummaryVersion &&
         snapshot.workspaceFoldersFingerprint ==
             contextFingerprints.workspaceFoldersFingerprint &&
         snapshot.definesFingerprint ==
             contextFingerprints.definesFingerprint &&
         snapshot.includePathsFingerprint ==
             contextFingerprints.includePathsFingerprint &&
         snapshot.shaderExtensionsFingerprint ==
             contextFingerprints.shaderExtensionsFingerprint;
}

bool shouldReuseExistingActiveUnitSnapshot(
    const DocumentRuntime &existing, const Document &document,
    const std::string &activeUnitUri, const std::string &activeUnitPath,
    const RuntimeContextFingerprints &contextFingerprints,
    const DocumentRuntimeUpdateOptions &options) {
  if (!activeUnitContextMatches(existing.activeUnitSnapshot, activeUnitUri,
                                activeUnitPath, contextFingerprints, options)) {
    return false;
  }

  const std::string documentPath = uriToPath(document.uri);
  const bool documentIsActiveUnit =
      (!activeUnitUri.empty() && document.uri == activeUnitUri) ||
      (!activeUnitPath.empty() && !documentPath.empty() &&
       documentPath == activeUnitPath);
  if (!documentIsActiveUnit)
    return true;

  return existing.version == document.version && existing.epoch == document.epoch;
}

} // namespace

AnalysisSnapshotKey buildAnalysisSnapshotKey(
    const std::string &uri, int version, uint64_t epoch,
    const ActiveUnitSnapshot &activeUnitSnapshot,
    const std::string &resourceModelHash) {
  AnalysisSnapshotKey key;
  key.documentUri = uri;
  key.documentVersion = version;
  key.documentEpoch = epoch;
  key.activeUnitPath = activeUnitSnapshot.path;
  key.activeUnitIncludeClosureFingerprint =
      activeUnitSnapshot.includeClosureFingerprint;
  key.activeUnitBranchFingerprint = activeUnitSnapshot.activeBranchFingerprint;
  key.workspaceFoldersFingerprint =
      activeUnitSnapshot.workspaceFoldersFingerprint;
  key.definesFingerprint = activeUnitSnapshot.definesFingerprint;
  key.includePathsFingerprint = activeUnitSnapshot.includePathsFingerprint;
  key.shaderExtensionsFingerprint =
      activeUnitSnapshot.shaderExtensionsFingerprint;
  key.resourceModelHash = resourceModelHash;
  key.workspaceSummaryVersion = activeUnitSnapshot.workspaceSummaryVersion;

  uint64_t stableHash = 1469598103934665603ull;
  stableHash = fnv1aStep(stableHash, key.documentUri);
  stableHash = fnv1aStep(stableHash, "|");
  stableHash = fnv1aStep(stableHash, key.activeUnitPath);
  stableHash = fnv1aStep(stableHash, "|");
  stableHash = fnv1aStep(stableHash, key.activeUnitIncludeClosureFingerprint);
  stableHash = fnv1aStep(stableHash, "|");
  stableHash = fnv1aStep(stableHash, key.activeUnitBranchFingerprint);
  stableHash = fnv1aStep(stableHash, "|");
  stableHash = fnv1aStep(stableHash, key.workspaceFoldersFingerprint);
  stableHash = fnv1aStep(stableHash, "|");
  stableHash = fnv1aStep(stableHash, key.definesFingerprint);
  stableHash = fnv1aStep(stableHash, "|");
  stableHash = fnv1aStep(stableHash, key.includePathsFingerprint);
  stableHash = fnv1aStep(stableHash, "|");
  stableHash = fnv1aStep(stableHash, key.shaderExtensionsFingerprint);
  stableHash = fnv1aStep(stableHash, "|");
  stableHash = fnv1aStep(stableHash, key.resourceModelHash);
  stableHash = fnv1aStep(stableHash, "|");
  stableHash = fnv1aStep(stableHash,
                         std::to_string(key.workspaceSummaryVersion));
  key.stableContextFingerprint = toHex(stableHash);

  uint64_t fullHash = stableHash;
  fullHash = fnv1aStep(fullHash, "|");
  fullHash = fnv1aStep(fullHash, std::to_string(version));
  fullHash = fnv1aStep(fullHash, "|");
  fullHash = fnv1aStep(fullHash, std::to_string(epoch));
  key.fullFingerprint = toHex(fullHash);
  return key;
}

std::string getDocumentRuntimeResourceModelHash() {
  static const std::vector<std::string> kBundleKeys = {
      "builtins/intrinsics",  "language/keywords", "language/directives",
      "language/semantics",   "methods/object_methods",
      "types/object_types",   "types/object_families",
      "types/type_overrides"};
  static std::mutex gResourceModelHashMutex;
  static std::vector<ResourceFileStamp> gCachedStamps;
  static std::string gCachedHash;

  std::vector<ResourceFileStamp> currentStamps;
  currentStamps.reserve(kBundleKeys.size() * 3);
  for (const auto &bundleKey : kBundleKeys) {
    const ResourceBundlePaths paths = resolveResourceBundlePaths(bundleKey);
    for (const auto &path :
         {paths.basePath, paths.overridePath, paths.schemaPath}) {
      currentStamps.push_back(makeResourceFileStamp(path));
    }
  }

  {
    std::lock_guard<std::mutex> lock(gResourceModelHashMutex);
    if (!gCachedHash.empty() && gCachedStamps.size() == currentStamps.size() &&
        std::equal(gCachedStamps.begin(), gCachedStamps.end(),
                   currentStamps.begin(), resourceFileStampsEqual)) {
      return gCachedHash;
    }
  }

  uint64_t value = 1469598103934665603ull;
  size_t stampIndex = 0;
  for (const auto &bundleKey : kBundleKeys) {
    value = fnv1aStep(value, bundleKey);
    value = fnv1aStep(value, "|");
    const ResourceBundlePaths paths = resolveResourceBundlePaths(bundleKey);
    for (const auto &path :
         {paths.basePath, paths.overridePath, paths.schemaPath}) {
      const ResourceFileStamp &stamp = currentStamps[stampIndex++];
      value = fnv1aStep(value, stamp.normalizedPath);
      value = fnv1aStep(value, ":");
      std::string text;
      if (stamp.exists && readFileText(stamp.normalizedPath, text)) {
        value = fnv1aStep(value, text);
      } else {
        value = fnv1aStep(value, "!missing");
      }
      value = fnv1aStep(value, ";");
    }
  }

  const std::string hash = toHex(value);
  {
    std::lock_guard<std::mutex> lock(gResourceModelHashMutex);
    gCachedStamps = std::move(currentStamps);
    gCachedHash = hash;
  }
  return hash;
}

uint64_t documentRuntimeGetWorkspaceSummaryVersion() {
  return gWorkspaceSummaryVersion.load(std::memory_order_relaxed);
}

uint64_t documentRuntimeBumpWorkspaceSummaryVersion() {
  return gWorkspaceSummaryVersion.fetch_add(1, std::memory_order_relaxed) + 1;
}

void documentRuntimeUpsert(const Document &document,
                           const std::vector<ChangedRange> &changedRanges,
                           const DocumentRuntimeUpdateOptions &options) {
  const std::string activeUnitUri = getActiveUnitUri();
  const std::string activeUnitPath = getActiveUnitPath();
  const RuntimeContextFingerprints contextFingerprints =
      buildRuntimeContextFingerprints(options);

  DocumentRuntime updated;
  updated.uri = document.uri;
  updated.text = document.text;
  updated.version = document.version;
  updated.epoch = document.epoch;
  bool reusedActiveUnitSnapshot = false;
  {
    std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
    auto existingIt = gDocumentRuntimes.find(document.uri);
    if (existingIt != gDocumentRuntimes.end() &&
        shouldReuseExistingActiveUnitSnapshot(existingIt->second, document,
                                             activeUnitUri, activeUnitPath,
                                             contextFingerprints, options)) {
      updated.activeUnitSnapshot = existingIt->second.activeUnitSnapshot;
      reusedActiveUnitSnapshot = true;
    }
  }
  if (!reusedActiveUnitSnapshot) {
    updated.activeUnitSnapshot = buildActiveUnitSnapshot(
        options, activeUnitUri, activeUnitPath, contextFingerprints);
  }
  updated.analysisSnapshotKey =
      buildAnalysisSnapshotKey(document.uri, document.version, document.epoch,
                               updated.activeUnitSnapshot,
                               options.resourceModelHash);
  updated.changedRanges = changedRanges;
  updated.immediateSyntaxSnapshot = ImmediateSyntaxSnapshot{};

  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  auto existingIt = gDocumentRuntimes.find(document.uri);
  if (existingIt != gDocumentRuntimes.end()) {
    const DocumentRuntime &existing = existingIt->second;
    updated.semanticNeutralEditHint = isSemanticNeutralEditAgainstPrevious(
        existing.text, document.text, changedRanges);
    updated.syntaxOnlyEditHint =
        isSyntaxOnlyEditAgainstPrevious(existing.text, document.text,
                                        changedRanges);
    if (isSnapshotStaleEligible(existing.analysisSnapshotKey,
                                updated.analysisSnapshotKey)) {
      updated.lastGoodInteractiveSnapshot = existing.interactiveSnapshot
                                                ? existing.interactiveSnapshot
                                                : existing.lastGoodInteractiveSnapshot;
      if (existing.deferredDocSnapshot &&
          isSnapshotStaleEligible(existing.deferredDocSnapshot->key,
                                  updated.analysisSnapshotKey)) {
        updated.deferredDocSnapshot = existing.deferredDocSnapshot;
      }
      if (existing.immediateSyntaxSnapshot.documentEpoch == document.epoch &&
          existing.immediateSyntaxSnapshot.documentVersion ==
              document.version) {
        updated.immediateSyntaxSnapshot = existing.immediateSyntaxSnapshot;
      }
    }
    if (existing.interactiveSnapshot &&
        isSnapshotStillCurrent(existing.interactiveSnapshot->key,
                               updated.analysisSnapshotKey)) {
      updated.interactiveSnapshot = existing.interactiveSnapshot;
      updated.lastGoodInteractiveSnapshot = existing.interactiveSnapshot;
    }
    if (existing.deferredDocSnapshot &&
        isSnapshotStillCurrent(existing.deferredDocSnapshot->key,
                               updated.analysisSnapshotKey)) {
      updated.deferredDocSnapshot = existing.deferredDocSnapshot;
    }
  }
  gDocumentRuntimes[document.uri] = std::move(updated);
}

bool documentRuntimeGet(const std::string &uri, DocumentRuntime &runtimeOut) {
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  auto it = gDocumentRuntimes.find(uri);
  if (it == gDocumentRuntimes.end())
    return false;
  runtimeOut = it->second;
  return true;
}

void documentRuntimeErase(const std::string &uri) {
  if (uri.empty())
    return;
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  gDocumentRuntimes.erase(uri);
}

void refreshRuntimeAnalysisKey(DocumentRuntime &runtime,
                               const DocumentRuntimeUpdateOptions &options,
                               const std::string &activeUnitUri,
                               const std::string &activeUnitPath,
                               const RuntimeContextFingerprints &contextFingerprints) {
  const ActiveUnitSnapshot nextActive = buildActiveUnitSnapshot(
      options, activeUnitUri, activeUnitPath, contextFingerprints);
  const AnalysisSnapshotKey nextKey =
      buildAnalysisSnapshotKey(runtime.uri, runtime.version, runtime.epoch,
                               nextActive, options.resourceModelHash);
  if (!isSnapshotStaleEligible(runtime.analysisSnapshotKey, nextKey)) {
    runtime.interactiveSnapshot.reset();
    runtime.lastGoodInteractiveSnapshot.reset();
    runtime.deferredDocSnapshot.reset();
    runtime.immediateSyntaxSnapshot = ImmediateSyntaxSnapshot{};
  } else {
    if (runtime.interactiveSnapshot &&
        !isSnapshotStillCurrent(runtime.interactiveSnapshot->key, nextKey)) {
      runtime.lastGoodInteractiveSnapshot = runtime.interactiveSnapshot;
      runtime.interactiveSnapshot.reset();
    }
    if (runtime.lastGoodInteractiveSnapshot &&
        !isSnapshotStaleEligible(runtime.lastGoodInteractiveSnapshot->key,
                                 nextKey)) {
      runtime.lastGoodInteractiveSnapshot.reset();
    }
    if (runtime.deferredDocSnapshot &&
        !isSnapshotStaleEligible(runtime.deferredDocSnapshot->key, nextKey)) {
      runtime.deferredDocSnapshot.reset();
    }
    runtime.immediateSyntaxSnapshot = ImmediateSyntaxSnapshot{};
  }
  runtime.analysisSnapshotKey = nextKey;
  runtime.activeUnitSnapshot = nextActive;
}

void documentRuntimeRefreshAnalysisKeys(
    const DocumentRuntimeUpdateOptions &options) {
  const std::string activeUnitUri = getActiveUnitUri();
  const std::string activeUnitPath = getActiveUnitPath();
  const RuntimeContextFingerprints contextFingerprints =
      buildRuntimeContextFingerprints(options);
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  for (auto &entry : gDocumentRuntimes) {
    refreshRuntimeAnalysisKey(entry.second, options, activeUnitUri,
                              activeUnitPath, contextFingerprints);
  }
}

void documentRuntimeRefreshAnalysisKeysForUris(
    const std::vector<std::string> &uris,
    const DocumentRuntimeUpdateOptions &options) {
  if (uris.empty())
    return;
  std::unordered_set<std::string> targets;
  targets.reserve(uris.size());
  for (const auto &uri : uris) {
    if (!uri.empty())
      targets.insert(uri);
  }
  if (targets.empty())
    return;
  const std::string activeUnitUri = getActiveUnitUri();
  const std::string activeUnitPath = getActiveUnitPath();
  const RuntimeContextFingerprints contextFingerprints =
      buildRuntimeContextFingerprints(options);
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  for (const auto &uri : targets) {
    auto it = gDocumentRuntimes.find(uri);
    if (it == gDocumentRuntimes.end())
      continue;
    refreshRuntimeAnalysisKey(it->second, options, activeUnitUri,
                              activeUnitPath, contextFingerprints);
  }
}

void documentRuntimeUpdateImmediateSyntaxSnapshot(
    const std::string &uri, const ImmediateSyntaxSnapshot &snapshot) {
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  auto it = gDocumentRuntimes.find(uri);
  if (it == gDocumentRuntimes.end())
    return;
  if (it->second.epoch != snapshot.documentEpoch ||
      it->second.version != snapshot.documentVersion) {
    return;
  }
  it->second.immediateSyntaxSnapshot = snapshot;
}

void documentRuntimeStoreInteractiveSnapshot(
    const std::string &uri,
    const std::shared_ptr<const InteractiveSnapshot> &snapshot) {
  if (!snapshot)
    return;
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  auto it = gDocumentRuntimes.find(uri);
  if (it == gDocumentRuntimes.end())
    return;
  if (it->second.epoch != snapshot->documentEpoch ||
      it->second.version != snapshot->documentVersion ||
      !isSnapshotStillCurrent(snapshot->key, it->second.analysisSnapshotKey)) {
    return;
  }
  it->second.interactiveSnapshot = snapshot;
  it->second.lastGoodInteractiveSnapshot = snapshot;
}

void documentRuntimeStoreDeferredSnapshot(
    const std::string &uri,
    const std::shared_ptr<const DeferredDocSnapshot> &snapshot) {
  if (!snapshot)
    return;
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  auto it = gDocumentRuntimes.find(uri);
  if (it == gDocumentRuntimes.end())
    return;
  if (it->second.epoch != snapshot->documentEpoch ||
      it->second.version != snapshot->documentVersion ||
      !isSnapshotStillCurrent(snapshot->key, it->second.analysisSnapshotKey)) {
    return;
  }
  it->second.deferredDocSnapshot = snapshot;
}
