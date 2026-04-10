#include "document_runtime.hpp"

#include "global_context_runtime.hpp"
#include "resource_registry.hpp"
#include "lsp_helpers.hpp"
#include "server_documents.hpp"
#include "text_utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

std::mutex gDocumentRuntimeMutex;
std::unordered_map<std::string, DocumentRuntime> gDocumentRuntimes;

const std::shared_ptr<const InteractiveSnapshot> &
currentDocSemanticSnapshotView(const DocumentRuntime &runtime) {
  return runtime.currentDocSemanticSnapshot;
}

const std::shared_ptr<const InteractiveSnapshot> &
lastGoodCurrentDocSemanticSnapshotView(const DocumentRuntime &runtime) {
  return runtime.lastGoodCurrentDocSemanticSnapshot;
}

void syncLegacyInteractiveSnapshotMirror(DocumentRuntime &runtime) {
  runtime.interactiveSnapshot = runtime.currentDocSemanticSnapshot;
  runtime.lastGoodInteractiveSnapshot =
      runtime.lastGoodCurrentDocSemanticSnapshot;
}

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

std::string trimLeftCopy(const std::string &value) {
  size_t index = 0;
  while (index < value.size() &&
         std::isspace(static_cast<unsigned char>(value[index]))) {
    index++;
  }
  return value.substr(index);
}

std::string trimRightCopy(const std::string &value) {
  size_t end = value.size();
  while (end > 0 &&
         std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    end--;
  }
  return value.substr(0, end);
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


bool isSnapshotStillCurrent(const AnalysisSnapshotKey &candidate,
                            const AnalysisSnapshotKey &current) {
  return candidate.fullFingerprint == current.fullFingerprint;
}

bool isSnapshotStaleEligible(const AnalysisSnapshotKey &candidate,
                             const AnalysisSnapshotKey &current) {
  return candidate.stableContextFingerprint == current.stableContextFingerprint;
}

bool isLocalStructuralContextStillCurrent(const AnalysisSnapshotKey &candidate,
                                          const AnalysisSnapshotKey &current) {
  return candidate.stableContextFingerprint ==
         current.stableContextFingerprint;
}

std::string diagnosticsPublishFingerprintForLayer(
    const std::string &layer, const AnalysisSnapshotKey &key) {
  if (layer == "local-structural")
    return key.stableContextFingerprint;
  return key.fullFingerprint;
}

static int countLines(const std::string &text) {
  const int crCount =
      static_cast<int>(std::count(text.begin(), text.end(), '\n'));
  return std::max(1, 1 + crCount);
}

static std::pair<int, int> computeDeferredChangedWindow(
    const std::string &text, const std::vector<ChangedRange> &changedRanges) {
  const int lineCount = countLines(text);
  if (changedRanges.empty())
    return {0, lineCount - 1};

  int startLine = lineCount - 1;
  int endLine = 0;
  for (const auto &range : changedRanges) {
    const int normalizedStart = std::max(0, range.startLine);
    startLine = std::min(startLine, normalizedStart);
    const int candidateEnd =
        std::max(range.endLine, range.startLine + range.newEndLine);
    const int normalizedEnd =
        std::min(lineCount - 1, std::max(candidateEnd, 0));
    endLine = std::max(endLine, normalizedEnd);
  }
  if (endLine < startLine)
    endLine = startLine;
  return {startLine, endLine};
}

static void invalidateOverlappingDeferredRanges(
    std::vector<DeferredRangeCacheEntry> &entries, int changedStartLine,
    int changedEndLine) {
  if (entries.empty())
    return;
  entries.erase(
      std::remove_if(entries.begin(), entries.end(),
                     [&](const DeferredRangeCacheEntry &entry) {
                       return !(entry.endLine < changedStartLine ||
                                entry.startLine > changedEndLine);
                     }),
      entries.end());
}

std::shared_ptr<const GlobalContextSnapshot> resolveGlobalContextSnapshot(
    const DocumentRuntimeUpdateOptions &options,
    const std::shared_ptr<const GlobalContextSnapshot> &providedSnapshot,
    const std::string &changedDocumentUri = std::string(),
    const std::vector<ChangedRange> *changedRanges = nullptr) {
  if (providedSnapshot)
    return providedSnapshot;
  return globalContextRuntimeRefresh(options.globalContextOptions,
                                     changedDocumentUri, changedRanges);
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

void documentRuntimeUpsert(const Document &document,
                           const std::vector<ChangedRange> &changedRanges,
                           const DocumentRuntimeUpdateOptions &options) {
  const auto globalContextSnapshot =
      resolveGlobalContextSnapshot(options, nullptr, document.uri,
                                   &changedRanges);

  DocumentRuntime updated;
  updated.uri = document.uri;
  updated.text = document.text;
  updated.version = document.version;
  updated.epoch = document.epoch;
  updated.globalContextSnapshot = globalContextSnapshot;
  if (globalContextSnapshot) {
    updated.activeUnitSnapshot = globalContextSnapshot->activeUnitSnapshot;
    updated.interactiveVisibilityKey =
        globalContextSnapshot->interactiveVisibilityKey;
  }
  updated.analysisSnapshotKey =
      buildAnalysisSnapshotKey(document.uri, document.version, document.epoch,
                               updated.activeUnitSnapshot,
                               options.resourceModelHash);
  updated.changedRanges = changedRanges;
  updated.localStructuralSnapshot = LocalStructuralSnapshot{};

  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  auto existingIt = gDocumentRuntimes.find(document.uri);
  if (existingIt != gDocumentRuntimes.end()) {
    const DocumentRuntime &existing = existingIt->second;
    const auto &existingCurrentSemantic =
        currentDocSemanticSnapshotView(existing);
    const auto &existingLastGoodSemantic =
        lastGoodCurrentDocSemanticSnapshotView(existing);
    if (existing.lastDiagnosticsPublishEpoch == document.epoch &&
        existing.lastDiagnosticsPublishVersion == document.version &&
        existing.lastDiagnosticsPublishFingerprint ==
            diagnosticsPublishFingerprintForLayer(
                existing.lastDiagnosticsPublishLayer,
                updated.analysisSnapshotKey)) {
      updated.lastDiagnosticsPublishLayer =
          existing.lastDiagnosticsPublishLayer;
      updated.lastDiagnosticsPublishEpoch = existing.lastDiagnosticsPublishEpoch;
      updated.lastDiagnosticsPublishVersion =
          existing.lastDiagnosticsPublishVersion;
      updated.lastDiagnosticsPublishFingerprint =
          existing.lastDiagnosticsPublishFingerprint;
    }
    updated.semanticNeutralEditHint = isSemanticNeutralEditAgainstPrevious(
        existing.text, document.text, changedRanges);
    updated.syntaxOnlyEditHint =
        isSyntaxOnlyEditAgainstPrevious(existing.text, document.text,
                                        changedRanges);
    if (existing.localStructuralSnapshot.documentEpoch == document.epoch &&
        existing.localStructuralSnapshot.documentVersion == document.version &&
        existing.localStructuralSnapshot.contextFingerprint ==
            updated.analysisSnapshotKey.stableContextFingerprint) {
      updated.localStructuralSnapshot = existing.localStructuralSnapshot;
    }
    if (isSnapshotStaleEligible(existing.analysisSnapshotKey,
                                updated.analysisSnapshotKey)) {
      updated.lastGoodCurrentDocSemanticSnapshot =
          existingCurrentSemantic ? existingCurrentSemantic
                                  : existingLastGoodSemantic;
      if (existing.deferredDocSnapshot &&
          isSnapshotStaleEligible(existing.deferredDocSnapshot->key,
                                  updated.analysisSnapshotKey)) {
        auto writable =
            std::make_shared<DeferredDocSnapshot>(*existing.deferredDocSnapshot);
        const auto [changedStartLine, changedEndLine] =
            computeDeferredChangedWindow(document.text, changedRanges);
        invalidateOverlappingDeferredRanges(writable->semanticTokensRangeCache,
                                            changedStartLine, changedEndLine);
        invalidateOverlappingDeferredRanges(writable->inlayHintsRangeCache,
                                            changedStartLine, changedEndLine);
        writable->semanticTokensFull = makeArray();
        writable->hasSemanticTokensFull = false;
        writable->inlayHintsFull = makeArray();
        writable->hasInlayHintsFull = false;
        updated.deferredDocSnapshot = std::move(writable);
      }
    }
    if (existingCurrentSemantic &&
        isSnapshotStillCurrent(existingCurrentSemantic->key,
                               updated.analysisSnapshotKey)) {
      updated.currentDocSemanticSnapshot = existingCurrentSemantic;
      updated.lastGoodCurrentDocSemanticSnapshot = existingCurrentSemantic;
    }
    if (existing.deferredDocSnapshot &&
        isSnapshotStillCurrent(existing.deferredDocSnapshot->key,
                               updated.analysisSnapshotKey)) {
      updated.deferredDocSnapshot = existing.deferredDocSnapshot;
    }
  }
  syncLegacyInteractiveSnapshotMirror(updated);
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

bool documentRuntimeAnyUsesInteractiveVisibilityFingerprint(
    const std::string &fullFingerprint) {
  if (fullFingerprint.empty())
    return false;
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  for (const auto &entry : gDocumentRuntimes) {
    if (entry.second.interactiveVisibilityKey.fullFingerprint ==
        fullFingerprint) {
      return true;
    }
  }
  return false;
}

void documentRuntimeErase(const std::string &uri) {
  if (uri.empty())
    return;
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  gDocumentRuntimes.erase(uri);
}

void refreshRuntimeAnalysisKey(DocumentRuntime &runtime,
                               const DocumentRuntimeUpdateOptions &options,
                               const std::shared_ptr<const GlobalContextSnapshot>
                                   &globalContextSnapshot) {
  ActiveUnitSnapshot nextActive;
  InteractiveVisibilityKey nextVisibilityKey;
  if (globalContextSnapshot) {
    nextActive = globalContextSnapshot->activeUnitSnapshot;
    nextVisibilityKey = globalContextSnapshot->interactiveVisibilityKey;
  }
  const AnalysisSnapshotKey nextKey =
      buildAnalysisSnapshotKey(runtime.uri, runtime.version, runtime.epoch,
                               nextActive, options.resourceModelHash);
  if (!runtime.lastDiagnosticsPublishLayer.empty() &&
      runtime.lastDiagnosticsPublishLayer != "local-structural" &&
      runtime.lastDiagnosticsPublishFingerprint !=
          diagnosticsPublishFingerprintForLayer(
              runtime.lastDiagnosticsPublishLayer, nextKey)) {
    runtime.lastDiagnosticsPublishLayer.clear();
    runtime.lastDiagnosticsPublishEpoch = 0;
    runtime.lastDiagnosticsPublishVersion = 0;
    runtime.lastDiagnosticsPublishFingerprint.clear();
  }
  if (!isSnapshotStaleEligible(runtime.analysisSnapshotKey, nextKey)) {
    runtime.localStructuralSnapshot = LocalStructuralSnapshot{};
    runtime.currentDocSemanticSnapshot.reset();
    runtime.lastGoodCurrentDocSemanticSnapshot.reset();
    runtime.deferredDocSnapshot.reset();
  } else {
    if (runtime.localStructuralSnapshot.contextFingerprint !=
        nextKey.stableContextFingerprint) {
      runtime.localStructuralSnapshot = LocalStructuralSnapshot{};
    }
    if (currentDocSemanticSnapshotView(runtime) &&
        !isSnapshotStillCurrent(currentDocSemanticSnapshotView(runtime)->key,
                               nextKey)) {
      runtime.lastGoodCurrentDocSemanticSnapshot =
          currentDocSemanticSnapshotView(runtime);
      runtime.currentDocSemanticSnapshot.reset();
    }
    if (lastGoodCurrentDocSemanticSnapshotView(runtime) &&
        !isSnapshotStaleEligible(
            lastGoodCurrentDocSemanticSnapshotView(runtime)->key, nextKey)) {
      runtime.lastGoodCurrentDocSemanticSnapshot.reset();
    }
    if (runtime.deferredDocSnapshot &&
        !isSnapshotStaleEligible(runtime.deferredDocSnapshot->key, nextKey)) {
      runtime.deferredDocSnapshot.reset();
    }
  }
  syncLegacyInteractiveSnapshotMirror(runtime);
  runtime.globalContextSnapshot = globalContextSnapshot;
  runtime.analysisSnapshotKey = nextKey;
  runtime.interactiveVisibilityKey = nextVisibilityKey;
  runtime.activeUnitSnapshot = nextActive;
}

void documentRuntimeRefreshAnalysisKeys(
    const DocumentRuntimeUpdateOptions &options,
    const std::shared_ptr<const GlobalContextSnapshot> &globalContextSnapshot) {
  const auto resolvedSnapshot =
      resolveGlobalContextSnapshot(options, globalContextSnapshot);
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  for (auto &entry : gDocumentRuntimes) {
    refreshRuntimeAnalysisKey(entry.second, options, resolvedSnapshot);
  }
}

void documentRuntimeRefreshAnalysisKeysForUris(
    const std::vector<std::string> &uris,
    const DocumentRuntimeUpdateOptions &options,
    const std::shared_ptr<const GlobalContextSnapshot> &globalContextSnapshot) {
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
  const auto resolvedSnapshot =
      resolveGlobalContextSnapshot(options, globalContextSnapshot);
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  for (const auto &uri : targets) {
    auto it = gDocumentRuntimes.find(uri);
    if (it == gDocumentRuntimes.end())
      continue;
    refreshRuntimeAnalysisKey(it->second, options, resolvedSnapshot);
  }
}

void documentRuntimeUpdateImmediateSyntaxSnapshot(
    const std::string &uri, const ImmediateSyntaxSnapshot &snapshot) {
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  auto it = gDocumentRuntimes.find(uri);
  if (it == gDocumentRuntimes.end())
    return;
  if (it->second.epoch != snapshot.documentEpoch ||
      it->second.version != snapshot.documentVersion ||
      snapshot.contextFingerprint.empty() ||
      snapshot.contextFingerprint !=
          it->second.analysisSnapshotKey.stableContextFingerprint) {
    return;
  }
  it->second.localStructuralSnapshot = snapshot;
}

void documentRuntimeUpdateLastDiagnosticsPublishLayer(
    const std::string &uri, uint64_t documentEpoch, int documentVersion,
    const std::string &layer) {
  std::lock_guard<std::mutex> lock(gDocumentRuntimeMutex);
  auto it = gDocumentRuntimes.find(uri);
  if (it == gDocumentRuntimes.end())
    return;
  if (it->second.epoch != documentEpoch ||
      it->second.version != documentVersion) {
    return;
  }
  it->second.lastDiagnosticsPublishLayer = layer;
  it->second.lastDiagnosticsPublishEpoch = documentEpoch;
  it->second.lastDiagnosticsPublishVersion = documentVersion;
  it->second.lastDiagnosticsPublishFingerprint =
      diagnosticsPublishFingerprintForLayer(layer,
                                            it->second.analysisSnapshotKey);
}

void documentRuntimeStoreCurrentDocSemanticSnapshot(
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
  it->second.currentDocSemanticSnapshot = snapshot;
  it->second.lastGoodCurrentDocSemanticSnapshot = snapshot;
  syncLegacyInteractiveSnapshotMirror(it->second);
}

void documentRuntimeStoreInteractiveSnapshot(
    const std::string &uri,
    const std::shared_ptr<const InteractiveSnapshot> &snapshot) {
  documentRuntimeStoreCurrentDocSemanticSnapshot(uri, snapshot);
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

void documentRuntimeMergeAndStoreDeferredSnapshot(
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

  auto merged = std::make_shared<DeferredDocSnapshot>(*snapshot);
  const auto &current = it->second.deferredDocSnapshot;
  if (current && current->key.fullFingerprint == merged->key.fullFingerprint &&
      current->documentEpoch == merged->documentEpoch &&
      current->documentVersion == merged->documentVersion) {
    auto mergeRangeCacheEntries =
        [](std::vector<DeferredRangeCacheEntry> &target,
           const std::vector<DeferredRangeCacheEntry> &source) {
          for (const auto &entry : source) {
            bool replaced = false;
            for (auto &existing : target) {
              if (existing.startLine == entry.startLine &&
                  existing.endLine == entry.endLine) {
                existing.value = entry.value;
                replaced = true;
                break;
              }
            }
            if (!replaced) {
              target.push_back(entry);
            }
          }
        };

    if (!merged->astDocument && current->astDocument)
      merged->astDocument = current->astDocument;
    if (!merged->semanticSnapshot && current->semanticSnapshot)
      merged->semanticSnapshot = current->semanticSnapshot;
    if (!merged->hasFullDiagnostics && current->hasFullDiagnostics) {
      merged->fullDiagnostics = current->fullDiagnostics;
      merged->hasFullDiagnostics = true;
      merged->fullDiagnosticsFingerprint = current->fullDiagnosticsFingerprint;
    }
    if (!merged->hasSemanticTokensFull && current->hasSemanticTokensFull) {
      merged->semanticTokensFull = current->semanticTokensFull;
      merged->hasSemanticTokensFull = true;
    }
    if (!merged->hasInlayHintsFull && current->hasInlayHintsFull) {
      merged->inlayHintsFull = current->inlayHintsFull;
      merged->hasInlayHintsFull = true;
    }
    if (!merged->hasDocumentSymbols && current->hasDocumentSymbols) {
      merged->documentSymbols = current->documentSymbols;
      merged->hasDocumentSymbols = true;
    }
    mergeRangeCacheEntries(merged->semanticTokensRangeCache,
                           current->semanticTokensRangeCache);
    mergeRangeCacheEntries(merged->inlayHintsRangeCache,
                           current->inlayHintsRangeCache);
    merged->builtAtMs = std::max(merged->builtAtMs, current->builtAtMs);
  }

  it->second.deferredDocSnapshot = std::move(merged);
}
