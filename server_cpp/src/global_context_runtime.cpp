#include "global_context_runtime.hpp"

#include "active_unit.hpp"
#include "preprocessor_view.hpp"
#include "server_documents.hpp"
#include "text_utils.hpp"
#include "unit_macro_profile_provider.hpp"
#include "uri_utils.hpp"
#include "workspace_summary_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace {

struct RuntimeContextFingerprints {
  std::string workspaceFoldersFingerprint;
  std::string definesFingerprint;
  std::string includePathsFingerprint;
  std::string shaderCompilerPathFingerprint;
  std::string shaderExtensionsFingerprint;
};

struct RuntimeDefineContext {
  UnitMacroProfileSnapshot profileSnapshot;
  std::vector<ArtDefaultZeroMacro> artDefaultZeroMacros;
  std::unordered_map<std::string, int> effectiveDefines;
};

struct GlobalContextRuntimeState {
  std::shared_ptr<const GlobalContextSnapshot> snapshot;
  std::string activeUnitText;
  uint64_t nextLogicalId = 1;
};

std::mutex gGlobalContextRuntimeMutex;
GlobalContextRuntimeState gGlobalContextRuntimeState;

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
buildRuntimeContextFingerprints(const GlobalContextRuntimeOptions &options,
                                const RuntimeDefineContext &defineContext) {
  RuntimeContextFingerprints fingerprints;
  fingerprints.workspaceFoldersFingerprint =
      fingerprintStringList(options.workspaceFolders);
  fingerprints.definesFingerprint =
      fingerprintDefines(defineContext.effectiveDefines) + "|" +
      getConfiguredPreprocessorMacrosFingerprint();
  fingerprints.includePathsFingerprint =
      fingerprintStringList(options.includePaths);
  fingerprints.shaderCompilerPathFingerprint =
      fingerprintStringList({options.shaderCompilerPath});
  fingerprints.shaderExtensionsFingerprint =
      fingerprintStringList(options.shaderExtensions);
  return fingerprints;
}

bool tryParseIntegralHint(const std::string &text, int &out) {
  if (text.empty())
    return false;
  try {
    size_t consumed = 0;
    const int parsed = std::stoi(text, &consumed, 0);
    if (consumed != text.size())
      return false;
    out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool isSimpleIdentifier(const std::string &text) {
  if (text.empty())
    return false;
  const unsigned char first = static_cast<unsigned char>(text[0]);
  if (!(std::isalpha(first) || first == '_'))
    return false;
  for (size_t i = 1; i < text.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (!(std::isalnum(ch) || ch == '_'))
      return false;
  }
  return true;
}

bool resolveConfiguredMacroNumericValue(
    const std::string &name, const ConfiguredPreprocessorMacros &configured,
    const std::unordered_map<std::string, int> &defines,
    std::unordered_map<std::string, int> &memo,
    std::unordered_set<std::string> &resolving, int &out) {
  auto memoIt = memo.find(name);
  if (memoIt != memo.end()) {
    out = memoIt->second;
    return true;
  }
  auto defineIt = defines.find(name);
  if (defineIt != defines.end()) {
    out = defineIt->second;
    memo[name] = out;
    return true;
  }
  auto configuredIt = configured.find(name);
  if (configuredIt == configured.end())
    return false;

  if (!resolving.insert(name).second)
    return false;

  int parsed = 0;
  const std::string replacement = configuredIt->second;
  if (tryParseIntegralHint(replacement, parsed)) {
    out = parsed;
    memo[name] = out;
    resolving.erase(name);
    return true;
  }
  if (isSimpleIdentifier(replacement) &&
      resolveConfiguredMacroNumericValue(replacement, configured, defines, memo,
                                         resolving, parsed)) {
    out = parsed;
    memo[name] = out;
    resolving.erase(name);
    return true;
  }

  resolving.erase(name);
  return false;
}

std::unordered_map<std::string, int> buildUnitProfileSelectionHints(
    const GlobalContextRuntimeOptions &options) {
  std::unordered_map<std::string, int> hints = options.defines;
  const ConfiguredPreprocessorMacros configured = getConfiguredPreprocessorMacros();
  std::unordered_map<std::string, int> memo;
  memo.reserve(configured.size());
  for (const auto &entry : configured) {
    int value = 0;
    std::unordered_set<std::string> resolving;
    if (!resolveConfiguredMacroNumericValue(entry.first, configured,
                                            options.defines, memo, resolving,
                                            value))
      continue;
    hints[entry.first] = value;
  }
  return hints;
}

RuntimeDefineContext buildRuntimeDefineContext(
    const GlobalContextRuntimeOptions &options, const std::string &activeUnitPath) {
  RuntimeDefineContext context;
  workspaceSummaryRuntimeCollectArtDefaultZeroMacros(
      context.artDefaultZeroMacros, 4096);
  const std::unordered_map<std::string, int> selectionHints =
      buildUnitProfileSelectionHints(options);
  resolveUnitMacroProfileSnapshot(activeUnitPath, options.workspaceFolders,
                                  options.includePaths,
                                  options.shaderCompilerPath, selectionHints,
                                  context.profileSnapshot);
  context.effectiveDefines = context.profileSnapshot.defines;
  for (const auto &entry : options.defines)
    context.effectiveDefines[entry.first] = entry.second;
  return context;
}

bool buildActiveUnitPreprocessorView(
    const std::string &rootUri, const std::string &rootText,
    const GlobalContextRuntimeOptions &options,
    const RuntimeDefineContext &defineContext, PreprocessorView &out) {
  out = PreprocessorView{};
  if (rootUri.empty() || rootText.empty())
    return false;
  PreprocessorIncludeContext includeContext;
  includeContext.currentUri = rootUri;
  includeContext.workspaceFolders = options.workspaceFolders;
  includeContext.includePaths = options.includePaths;
  includeContext.shaderExtensions = options.shaderExtensions;
  includeContext.artDefaultZeroMacros = defineContext.artDefaultZeroMacros;
  includeContext.loadText = [](const std::string &uri,
                               std::string &textOut) -> bool {
    const std::string path = uriToPath(uri);
    return !path.empty() && readFileText(path, textOut);
  };
  out = buildPreprocessorView(rootText, defineContext.effectiveDefines,
                              includeContext);
  return true;
}

std::string buildActiveUnitBranchFingerprint(
    const PreprocessorView &preprocessorView) {
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

bool lineMayAffectPreprocessorState(const std::string &lineText) {
  const std::string trimmed = trimRightCopy(trimLeftCopy(lineText));
  if (trimmed.empty())
    return false;
  if (trimmed[0] == '#')
    return true;
  return !trimmed.empty() && trimmed.back() == '\\';
}

bool lineContinuesPreprocessorDirective(const std::string &lineText) {
  const std::string trimmed = trimRightCopy(trimLeftCopy(lineText));
  return !trimmed.empty() && trimmed.back() == '\\';
}

bool isPreprocessorNeutralEditAgainstPrevious(
    const std::string &oldText, const std::string &newText,
    const std::vector<ChangedRange> &changedRanges) {
  if (changedRanges.empty() || changedRanges.size() > 8)
    return false;
  int inspectedLines = 0;
  for (const auto &range : changedRanges) {
    const int startLine = std::max(0, range.startLine);
    const int oldEndLine = std::max(startLine, range.endLine);
    const int newEndLine =
        std::max(startLine, range.startLine + range.newEndLine);
    const int scanEndLine = std::max(oldEndLine, newEndLine);
    inspectedLines += (scanEndLine - startLine + 1) * 2;
    if (inspectedLines > 48)
      return false;
    if (startLine > 0 &&
        (lineContinuesPreprocessorDirective(getLineAt(oldText, startLine - 1)) ||
         lineContinuesPreprocessorDirective(getLineAt(newText, startLine - 1)))) {
      return false;
    }
    for (int line = startLine; line <= scanEndLine; line++) {
      if (lineMayAffectPreprocessorState(getLineAt(oldText, line)) ||
          lineMayAffectPreprocessorState(getLineAt(newText, line))) {
        return false;
      }
    }
  }
  return true;
}

ActiveUnitSnapshot buildActiveUnitSnapshot(
    const GlobalContextRuntimeOptions &options,
    const std::string &activeUnitUri, const std::string &activeUnitPath,
    const std::string &activeUnitText,
    const RuntimeDefineContext &defineContext,
    const RuntimeContextFingerprints &contextFingerprints) {
  ActiveUnitSnapshot snapshot;
  snapshot.uri = activeUnitUri;
  snapshot.path = activeUnitPath;
  snapshot.documentVersion = options.activeUnitDocumentVersion;
  snapshot.documentEpoch = options.activeUnitDocumentEpoch;
  snapshot.workspaceFolders = options.workspaceFolders;
  snapshot.includePaths = options.includePaths;
  snapshot.shaderCompilerPath = options.shaderCompilerPath;
  snapshot.shaderExtensions = options.shaderExtensions;
  snapshot.profileDefines = defineContext.profileSnapshot.defines;
  snapshot.profileShaderKey = defineContext.profileSnapshot.shaderKey;
  snapshot.profileSourcePath = defineContext.profileSnapshot.sourcePath;
  snapshot.profileSourceKind = defineContext.profileSnapshot.sourceKind;
  snapshot.profileTotalRowCount =
      defineContext.profileSnapshot.profileTotalRowCount;
  snapshot.profileSelectedRowCount =
      defineContext.profileSnapshot.profileSelectedRowCount;
  snapshot.profileSelectedRowSignature =
      defineContext.profileSnapshot.profileSelectedRowSignature;
  snapshot.profileSelectionHintSourcePath =
      defineContext.profileSnapshot.profileSelectionHintSourcePath;
  snapshot.profileUnresolvedMacroNames =
      defineContext.profileSnapshot.unresolvedMacroNames;
  snapshot.artDefaultZeroMacros = defineContext.artDefaultZeroMacros;
  snapshot.defines = defineContext.effectiveDefines;
  snapshot.workspaceFoldersFingerprint =
      contextFingerprints.workspaceFoldersFingerprint;
  snapshot.definesFingerprint = contextFingerprints.definesFingerprint;
  snapshot.includePathsFingerprint =
      contextFingerprints.includePathsFingerprint;
  snapshot.shaderCompilerPathFingerprint =
      contextFingerprints.shaderCompilerPathFingerprint;
  snapshot.shaderExtensionsFingerprint =
      contextFingerprints.shaderExtensionsFingerprint;
  snapshot.workspaceSummaryVersion = options.workspaceSummaryVersion;
  PreprocessorView activeUnitPreprocessorView;
  if (buildActiveUnitPreprocessorView(activeUnitUri, activeUnitText, options,
                                      defineContext,
                                      activeUnitPreprocessorView)) {
    snapshot.activeLineStates = activeUnitPreprocessorView.lineActive;
    snapshot.includeClosureUris = activeUnitPreprocessorView.activeIncludeUris;
    snapshot.includeClosureFingerprint =
        fingerprintStringList(snapshot.includeClosureUris);
    snapshot.activeBranchFingerprint =
        buildActiveUnitBranchFingerprint(activeUnitPreprocessorView);
  }
  return snapshot;
}

InteractiveVisibilityKey
buildInteractiveVisibilityKey(const ActiveUnitSnapshot &activeUnitSnapshot) {
  InteractiveVisibilityKey key;
  key.activeUnitPath = activeUnitSnapshot.path;
  key.includeClosureFingerprint = activeUnitSnapshot.includeClosureFingerprint;
  key.activeBranchFingerprint = activeUnitSnapshot.activeBranchFingerprint;
  key.definesFingerprint = activeUnitSnapshot.definesFingerprint;
  key.workspaceSummaryVersion = activeUnitSnapshot.workspaceSummaryVersion;
  key.fullFingerprint = key.activeUnitPath + "|" + key.includeClosureFingerprint +
                        "|" + key.activeBranchFingerprint + "|" +
                        key.definesFingerprint + "|" +
                        std::to_string(key.workspaceSummaryVersion);
  return key;
}

bool changedDocumentIsActiveUnit(const std::string &changedDocumentUri,
                                 const std::string &activeUnitUri,
                                 const std::string &activeUnitPath) {
  if (!changedDocumentUri.empty() && changedDocumentUri == activeUnitUri)
    return true;
  if (changedDocumentUri.empty() || activeUnitPath.empty())
    return false;
  const std::string changedPath = uriToPath(changedDocumentUri);
  return !changedPath.empty() && changedPath == activeUnitPath;
}

bool shouldReuseExistingGlobalContextSnapshot(
    const GlobalContextSnapshot &existing, const std::string &existingText,
    const std::string &activeUnitUri, const std::string &activeUnitPath,
    const std::string &activeUnitText,
    const RuntimeContextFingerprints &contextFingerprints,
    const GlobalContextRuntimeOptions &options,
    const std::string &changedDocumentUri,
    const std::vector<ChangedRange> *changedRanges) {
  const bool sameStableContext =
      existing.activeUnitSnapshot.uri == activeUnitUri &&
      existing.activeUnitSnapshot.path == activeUnitPath &&
      existing.activeUnitSnapshot.workspaceSummaryVersion ==
          options.workspaceSummaryVersion &&
      existing.activeUnitSnapshot.workspaceFoldersFingerprint ==
          contextFingerprints.workspaceFoldersFingerprint &&
      existing.activeUnitSnapshot.definesFingerprint ==
          contextFingerprints.definesFingerprint &&
      existing.activeUnitSnapshot.includePathsFingerprint ==
          contextFingerprints.includePathsFingerprint &&
      existing.activeUnitSnapshot.shaderCompilerPathFingerprint ==
          contextFingerprints.shaderCompilerPathFingerprint &&
      existing.activeUnitSnapshot.shaderExtensionsFingerprint ==
          contextFingerprints.shaderExtensionsFingerprint;
  if (!sameStableContext)
    return false;

  if (existing.activeUnitSnapshot.documentVersion ==
          options.activeUnitDocumentVersion &&
      existing.activeUnitSnapshot.documentEpoch ==
          options.activeUnitDocumentEpoch &&
      existingText == activeUnitText) {
    return true;
  }

  if (!changedDocumentIsActiveUnit(changedDocumentUri, activeUnitUri,
                                   activeUnitPath) ||
      !changedRanges) {
    return false;
  }

  return isPreprocessorNeutralEditAgainstPrevious(existingText, activeUnitText,
                                                  *changedRanges);
}

std::string resolveCurrentActiveUnitText(
    const GlobalContextRuntimeOptions &options, const std::string &activeUnitPath) {
  std::string activeUnitText = options.activeUnitText;
  if (activeUnitText.empty() && !activeUnitPath.empty())
    readFileText(activeUnitPath, activeUnitText);
  return activeUnitText;
}

bool snapshotActiveUnitVersionMatches(
    const GlobalContextSnapshot &snapshot,
    const GlobalContextRuntimeOptions &options,
    const std::string &activeUnitText, const std::string &cachedActiveUnitText) {
  return snapshot.activeUnitSnapshot.documentVersion ==
             options.activeUnitDocumentVersion &&
         snapshot.activeUnitSnapshot.documentEpoch ==
             options.activeUnitDocumentEpoch &&
         cachedActiveUnitText == activeUnitText;
}

std::shared_ptr<const GlobalContextSnapshot> makeUpdatedSnapshotPreservingIdentity(
    const GlobalContextSnapshot &existing,
    const GlobalContextRuntimeOptions &options) {
  auto updated = std::make_shared<GlobalContextSnapshot>(existing);
  updated->activeUnitSnapshot.documentVersion =
      options.activeUnitDocumentVersion;
  updated->activeUnitSnapshot.documentEpoch = options.activeUnitDocumentEpoch;
  return updated;
}

bool hasSameLogicalGlobalContext(const GlobalContextSnapshot &lhs,
                                 const GlobalContextSnapshot &rhs) {
  return lhs.activeUnitSnapshot.path == rhs.activeUnitSnapshot.path &&
         lhs.activeUnitSnapshot.includeClosureFingerprint ==
             rhs.activeUnitSnapshot.includeClosureFingerprint &&
         lhs.activeUnitSnapshot.activeBranchFingerprint ==
             rhs.activeUnitSnapshot.activeBranchFingerprint &&
         lhs.activeUnitSnapshot.workspaceFoldersFingerprint ==
             rhs.activeUnitSnapshot.workspaceFoldersFingerprint &&
         lhs.activeUnitSnapshot.definesFingerprint ==
             rhs.activeUnitSnapshot.definesFingerprint &&
         lhs.activeUnitSnapshot.includePathsFingerprint ==
             rhs.activeUnitSnapshot.includePathsFingerprint &&
         lhs.activeUnitSnapshot.shaderCompilerPathFingerprint ==
             rhs.activeUnitSnapshot.shaderCompilerPathFingerprint &&
         lhs.activeUnitSnapshot.shaderExtensionsFingerprint ==
             rhs.activeUnitSnapshot.shaderExtensionsFingerprint &&
         lhs.activeUnitSnapshot.workspaceSummaryVersion ==
             rhs.activeUnitSnapshot.workspaceSummaryVersion;
}

} // namespace

std::shared_ptr<const GlobalContextSnapshot> globalContextRuntimeRefresh(
    const GlobalContextRuntimeOptions &options,
    const std::string &changedDocumentUri,
    const std::vector<ChangedRange> *changedRanges) {
  const std::string activeUnitUri = getActiveUnitUri();
  const std::string activeUnitPath = getActiveUnitPath();
  const std::string activeUnitText =
      resolveCurrentActiveUnitText(options, activeUnitPath);
  const RuntimeDefineContext defineContext =
      buildRuntimeDefineContext(options, activeUnitPath);
  const RuntimeContextFingerprints contextFingerprints =
      buildRuntimeContextFingerprints(options, defineContext);

  std::lock_guard<std::mutex> lock(gGlobalContextRuntimeMutex);
  if (gGlobalContextRuntimeState.snapshot &&
      shouldReuseExistingGlobalContextSnapshot(
          *gGlobalContextRuntimeState.snapshot,
          gGlobalContextRuntimeState.activeUnitText, activeUnitUri,
          activeUnitPath, activeUnitText, contextFingerprints, options,
          changedDocumentUri, changedRanges)) {
    if (!snapshotActiveUnitVersionMatches(*gGlobalContextRuntimeState.snapshot,
                                          options, activeUnitText,
                                          gGlobalContextRuntimeState.activeUnitText)) {
      gGlobalContextRuntimeState.snapshot =
          makeUpdatedSnapshotPreservingIdentity(
              *gGlobalContextRuntimeState.snapshot, options);
    }
    gGlobalContextRuntimeState.activeUnitText = activeUnitText;
    return gGlobalContextRuntimeState.snapshot;
  }

  auto rebuilt = std::make_shared<GlobalContextSnapshot>();
  rebuilt->activeUnitSnapshot = buildActiveUnitSnapshot(
      options, activeUnitUri, activeUnitPath, activeUnitText, defineContext,
      contextFingerprints);
  rebuilt->interactiveVisibilityKey =
      buildInteractiveVisibilityKey(rebuilt->activeUnitSnapshot);
  if (gGlobalContextRuntimeState.snapshot &&
      hasSameLogicalGlobalContext(*gGlobalContextRuntimeState.snapshot,
                                  *rebuilt)) {
    rebuilt->debugLogicalId = gGlobalContextRuntimeState.snapshot->debugLogicalId;
  } else {
    rebuilt->debugLogicalId = gGlobalContextRuntimeState.nextLogicalId++;
  }
  gGlobalContextRuntimeState.snapshot = rebuilt;
  gGlobalContextRuntimeState.activeUnitText = activeUnitText;
  return rebuilt;
}

bool globalContextRuntimeIsReady(const GlobalContextSnapshot &snapshot) {
  const bool hasActiveUnitPath = !snapshot.activeUnitSnapshot.path.empty() ||
                                 !snapshot.activeUnitSnapshot.uri.empty();
  if (!hasActiveUnitPath)
    return true;
  return workspaceSummaryRuntimeIsReady() &&
         !snapshot.activeUnitSnapshot.includeClosureFingerprint.empty() &&
         !snapshot.activeUnitSnapshot.activeBranchFingerprint.empty();
}
