#include "preprocessor_view.hpp"

#include "compiler_macro_snapshot_provider.hpp"
#include "conditional_ast.hpp"
#include "include_resolver.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

std::mutex gConfiguredPreprocessorMacrosMutex;
ConfiguredPreprocessorMacros gConfiguredPreprocessorMacros;
std::mutex gCompilerPrivateConstantCacheMutex;
std::mutex gCompilerMacroSnapshotCacheMutex;
std::mutex gArtCompanionScopeCacheMutex;

struct InitialMacroSeedStats {
  size_t artDefaultZero = 0;
  size_t compilerPrivateConstants = 0;
  size_t compilerMacroSnapshot = 0;
  size_t configured = 0;
  size_t numericDefines = 0;
};

struct PreprocMacro {
  bool functionLike = false;
  std::vector<std::string> parameters;
  bool variadic = false;
  std::string variadicParameter;
  std::vector<LexToken> replacement;
  std::string replacementText;
  std::string sourceUri;
  int sourceLine = -1;
  int sourceStart = 0;
  int sourceEnd = 0;
  bool sourceSynthesizedZero = false;
  bool sourceIfndefDefault = false;
  bool sourceArtDefaultZero = false;
  bool sourceArtCompanionConstant = false;
  bool sourceCompilerPrivateConstant = false;
  bool sourceCompilerMacroSnapshot = false;
};

struct CompilerPrivateMacroConstant {
  std::string name;
  int value = 0;
  std::string sourceUri;
  int sourceLine = -1;
  int sourceStart = 0;
  int sourceEnd = 0;
};

std::unordered_map<std::string, std::vector<CompilerPrivateMacroConstant>>
    gCompilerPrivateConstantCache;
std::vector<std::string> gCompilerPrivateConstantCacheOrder;
constexpr size_t kMaxCompilerPrivateConstantCacheEntries = 64;

std::unordered_map<std::string, std::vector<CompilerMacroSnapshotMacro>>
    gCompilerMacroSnapshotCache;
std::vector<std::string> gCompilerMacroSnapshotCacheOrder;
constexpr size_t kMaxCompilerMacroSnapshotCacheEntries = 64;

std::unordered_map<std::string, std::vector<ArtDefaultZeroMacro>>
    gArtCompanionScopeCache;
std::vector<std::string> gArtCompanionScopeCacheOrder;
constexpr size_t kMaxArtCompanionScopeCacheEntries = 64;

struct CompilerPrivateMacroCandidate {
  bool hasValue = false;
  bool blocked = false;
  int value = 0;
  std::string sourceUri;
  int sourceLine = -1;
  int sourceStart = 0;
  int sourceEnd = 0;
};

struct ScopedPreprocessorInput {
  PreprocessorIncludeContext includeContext;
  PreprocessorView strictView;
  std::unordered_map<std::string, ConditionalAst> includeAstCache;
};

static bool tokenStartsAt(const std::vector<LexToken> &tokens, size_t index,
                          const std::string &text) {
  return index < tokens.size() && tokens[index].text == text;
}

static bool isEllipsisAt(const std::vector<LexToken> &tokens, size_t index) {
  if (tokenStartsAt(tokens, index, "..."))
    return true;
  return tokenStartsAt(tokens, index, ".") &&
         tokenStartsAt(tokens, index + 1, ".") &&
         tokenStartsAt(tokens, index + 2, ".");
}

static bool isTokenTextValidAfterPaste(const std::string &text,
                                       LexToken::Kind &kindOut) {
  const auto tokens = lexLineTokens(text);
  if (tokens.size() != 1 || tokens[0].start != 0 || tokens[0].end != text.size())
    return false;
  kindOut = tokens[0].kind;
  return true;
}

static LexToken makeSyntheticToken(const std::string &text,
                                   const LexToken &anchor) {
  LexToken token;
  token.kind = LexToken::Kind::Identifier;
  token.text = text;
  token.start = anchor.start;
  token.end = anchor.end;
  return token;
}

static LexToken makeSyntheticPunctToken(const std::string &text,
                                        const LexToken &anchor) {
  LexToken token = makeSyntheticToken(text, anchor);
  token.kind = LexToken::Kind::Punct;
  return token;
}

static std::string tokensToReplacementText(const std::vector<LexToken> &tokens) {
  std::string out;
  for (const auto &token : tokens) {
    if (!out.empty())
      out.push_back(' ');
    out += token.text;
  }
  return out;
}

static PreprocessorMacroReplacement
toPublicMacroReplacement(const PreprocMacro &macro) {
  PreprocessorMacroReplacement out;
  out.functionLike = macro.functionLike;
  out.replacement = macro.replacementText;
  if (out.replacement.empty() && !macro.replacement.empty())
    out.replacement = tokensToReplacementText(macro.replacement);
  out.sourceUri = macro.sourceUri;
  out.sourceLine = macro.sourceLine;
  out.sourceStart = macro.sourceStart;
  out.sourceEnd = macro.sourceEnd;
  out.sourceSynthesizedZero = macro.sourceSynthesizedZero;
  out.sourceIfndefDefault = macro.sourceIfndefDefault;
  out.sourceArtDefaultZero = macro.sourceArtDefaultZero;
  out.sourceArtCompanionConstant = macro.sourceArtCompanionConstant;
  out.sourceCompilerPrivateConstant = macro.sourceCompilerPrivateConstant;
  out.sourceCompilerMacroSnapshot = macro.sourceCompilerMacroSnapshot;
  return out;
}

static bool parseIntToken(const std::string &text, int &out) {
  if (text.empty())
    return false;
  try {
    size_t idx = 0;
    int value = std::stoi(text, &idx, 0);
    if (idx == 0)
      return false;
    out = value;
    return true;
  } catch (...) {
    return false;
  }
}

static bool isFunctionLikeDefineLine(const ConditionalAstLine &line) {
  return line.tokens.size() >= 4 &&
         line.tokens[3].kind == LexToken::Kind::Punct &&
         line.tokens[3].text == "(" &&
         line.tokens[2].end == line.tokens[3].start;
}

static bool parseStrictIntegerToken(const std::string &text, int &out) {
  if (text.empty())
    return false;
  try {
    size_t idx = 0;
    int value = std::stoi(text, &idx, 0);
    if (idx != text.size())
      return false;
    out = value;
    return true;
  } catch (...) {
    return false;
  }
}

static bool extractCompilerPrivateDefineCandidate(
    const ConditionalAstLine &line, std::string &nameOut, int &valueOut,
    bool &blockedOut) {
  nameOut.clear();
  valueOut = 0;
  blockedOut = false;
  if (!line.isDirective || line.directiveKind != ConditionalDirectiveKind::Define)
    return false;
  if (line.tokens.size() < 3 ||
      line.tokens[2].kind != LexToken::Kind::Identifier)
    return false;

  nameOut = line.tokens[2].text;
  if (nameOut.empty())
    return false;

  if (isFunctionLikeDefineLine(line) || line.tokens.size() != 4) {
    blockedOut = true;
    return true;
  }

  int value = 0;
  if (!parseStrictIntegerToken(line.tokens[3].text, value)) {
    blockedOut = true;
    return true;
  }

  valueOut = value;
  return true;
}

static bool extractCompilerPrivateUndefCandidate(const ConditionalAstLine &line,
                                                 std::string &nameOut) {
  nameOut.clear();
  if (!line.isDirective || line.directiveKind != ConditionalDirectiveKind::Undef)
    return false;
  if (line.tokens.size() < 3 ||
      line.tokens[2].kind != LexToken::Kind::Identifier)
    return false;
  nameOut = line.tokens[2].text;
  return !nameOut.empty();
}

static void recordCompilerPrivateDefineCandidate(
    std::unordered_map<std::string, CompilerPrivateMacroCandidate> &candidates,
    const ConditionalAstLine &line, const std::string &sourceUri) {
  std::string name;
  int value = 0;
  bool blocked = false;
  if (!extractCompilerPrivateDefineCandidate(line, name, value, blocked))
    return;

  CompilerPrivateMacroCandidate &candidate = candidates[name];
  if (blocked) {
    candidate.blocked = true;
    return;
  }
  if (candidate.hasValue && candidate.value != value) {
    candidate.blocked = true;
    return;
  }
  if (!candidate.hasValue) {
    candidate.hasValue = true;
    candidate.value = value;
    candidate.sourceUri = sourceUri;
    candidate.sourceLine = line.line;
    candidate.sourceStart = static_cast<int>(line.tokens[2].start);
    candidate.sourceEnd = static_cast<int>(line.tokens[2].end);
  }
}

static void recordCompilerPrivateUndefCandidate(
    std::unordered_map<std::string, CompilerPrivateMacroCandidate> &candidates,
    const ConditionalAstLine &line) {
  std::string name;
  if (!extractCompilerPrivateUndefCandidate(line, name))
    return;
  CompilerPrivateMacroCandidate &candidate = candidates[name];
  if (candidate.blocked)
    return;
  candidate.hasValue = false;
  candidate.value = 0;
  candidate.sourceUri.clear();
  candidate.sourceLine = -1;
  candidate.sourceStart = 0;
  candidate.sourceEnd = 0;
}

static void scanCompilerPrivateConstantsFromRootLines(
    const ConditionalAst &ast, const std::string &sourceUri,
    std::unordered_map<std::string, CompilerPrivateMacroCandidate>
        &candidates) {
  for (size_t nodeIndex : ast.rootNodeIndices) {
    if (nodeIndex >= ast.nodes.size())
      continue;
    const ConditionalAstNode &node = ast.nodes[nodeIndex];
    if (node.kind != ConditionalAstNode::Kind::Line || node.line < 0 ||
        node.line >= static_cast<int>(ast.lines.size()))
      continue;
    const ConditionalAstLine &line = ast.lines[node.line];
    recordCompilerPrivateDefineCandidate(candidates, line, sourceUri);
    recordCompilerPrivateUndefCandidate(candidates, line);
  }
}

static std::vector<CompilerPrivateMacroConstant>
collectCompilerPrivateConstantsFromActiveClosure(
    const ConditionalAst &rootAst,
    const PreprocessorIncludeContext &includeContext,
    const PreprocessorView &strictView,
    const std::unordered_map<std::string, ConditionalAst> &includeAstCache) {
  std::unordered_map<std::string, CompilerPrivateMacroCandidate> candidates;
  scanCompilerPrivateConstantsFromRootLines(rootAst, includeContext.currentUri,
                                            candidates);
  for (const auto &includeUri : strictView.activeIncludeUris) {
    auto it = includeAstCache.find(includeUri);
    if (it == includeAstCache.end())
      continue;
    scanCompilerPrivateConstantsFromRootLines(it->second, includeUri,
                                              candidates);
  }

  std::unordered_set<std::string> artCompanionNames;
  for (const auto &macro : includeContext.artDefaultZeroMacros) {
    for (const auto &constant : macro.companionConstants) {
      if (!constant.name.empty())
        artCompanionNames.insert(constant.name);
    }
  }

  std::vector<CompilerPrivateMacroConstant> constants;
  constants.reserve(candidates.size());
  for (const auto &entry : candidates) {
    if (artCompanionNames.find(entry.first) != artCompanionNames.end())
      continue;
    const CompilerPrivateMacroCandidate &candidate = entry.second;
    if (!candidate.hasValue || candidate.blocked)
      continue;
    CompilerPrivateMacroConstant constant;
    constant.name = entry.first;
    constant.value = candidate.value;
    constant.sourceUri = candidate.sourceUri;
    constant.sourceLine = candidate.sourceLine;
    constant.sourceStart = candidate.sourceStart;
    constant.sourceEnd = candidate.sourceEnd;
    constants.push_back(std::move(constant));
  }
  std::sort(constants.begin(), constants.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.name < rhs.name;
            });
  return constants;
}

static PreprocMacro makeNumericPreprocMacro(int value) {
  const std::string text = std::to_string(value);
  PreprocMacro macro;
  macro.replacementText = text;
  macro.replacement.push_back(
      LexToken{LexToken::Kind::Identifier, text, 0, text.size()});
  return macro;
}

static PreprocMacro makeReplacementPreprocMacro(const std::string &replacement) {
  PreprocMacro macro;
  macro.replacementText = replacement;
  macro.replacement = lexLineTokens(replacement);
  return macro;
}

static std::string artMacroUriKey(const std::string &uri) {
  std::string path = uriToPath(uri);
  if (path.empty())
    path = uri;
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return path;
}

static std::vector<ArtDefaultZeroMacro> stripArtCompanionConstants(
    const std::vector<ArtDefaultZeroMacro> &macros) {
  std::vector<ArtDefaultZeroMacro> stripped;
  stripped.reserve(macros.size());
  for (auto macro : macros) {
    macro.companionConstants.clear();
    stripped.push_back(std::move(macro));
  }
  return stripped;
}

static std::unordered_map<std::string, int>
buildCompanionValueMap(const ArtDefaultZeroMacro &macro) {
  std::unordered_map<std::string, int> values;
  for (const auto &constant : macro.companionConstants) {
    if (!constant.name.empty())
      values.emplace(constant.name, constant.value);
  }
  return values;
}

static bool companionSetsEquivalent(const ArtDefaultZeroMacro &lhs,
                                    const ArtDefaultZeroMacro &rhs) {
  return buildCompanionValueMap(lhs) == buildCompanionValueMap(rhs);
}

static void removeConflictingCompanionConstants(
    std::vector<ArtDefaultZeroMacro> &macros) {
  struct CompanionValueState {
    bool hasValue = false;
    bool conflict = false;
    int value = 0;
  };
  std::unordered_map<std::string, CompanionValueState> states;
  for (const auto &macro : macros) {
    for (const auto &constant : macro.companionConstants) {
      if (constant.name.empty())
        continue;
      auto &state = states[constant.name];
      if (!state.hasValue) {
        state.hasValue = true;
        state.value = constant.value;
      } else if (state.value != constant.value) {
        state.conflict = true;
      }
    }
  }
  for (auto &macro : macros) {
    macro.companionConstants.erase(
        std::remove_if(macro.companionConstants.begin(),
                       macro.companionConstants.end(),
                       [&](const ArtCompanionConstant &constant) {
                         auto it = states.find(constant.name);
                         return it != states.end() && it->second.conflict;
                       }),
        macro.companionConstants.end());
  }
}

static std::vector<ArtDefaultZeroMacro> scopeArtCompanionConstantsToView(
    const std::vector<ArtDefaultZeroMacro> &allMacros,
    const std::string &currentUri, const PreprocessorView &view) {
  std::unordered_set<std::string> activeUris;
  if (!currentUri.empty())
    activeUris.insert(artMacroUriKey(currentUri));
  for (const auto &uri : view.activeIncludeUris) {
    if (!uri.empty())
      activeUris.insert(artMacroUriKey(uri));
  }

  std::vector<ArtDefaultZeroMacro> scoped = stripArtCompanionConstants(allMacros);
  std::unordered_map<std::string, size_t> indexByName;
  indexByName.reserve(scoped.size());
  for (size_t i = 0; i < scoped.size(); ++i) {
    if (!scoped[i].name.empty())
      indexByName.emplace(scoped[i].name, i);
  }

  struct SelectedProvider {
    ArtDefaultZeroMacro macro;
    bool conflict = false;
  };
  std::unordered_map<std::string, SelectedProvider> selectedByName;
  for (const auto &macro : allMacros) {
    if (macro.name.empty() || macro.companionConstants.empty())
      continue;
    if (activeUris.find(artMacroUriKey(macro.uri)) == activeUris.end())
      continue;
    auto &selected = selectedByName[macro.name];
    if (selected.macro.name.empty()) {
      selected.macro = macro;
    } else if (!companionSetsEquivalent(selected.macro, macro)) {
      selected.conflict = true;
    }
  }

  for (const auto &entry : selectedByName) {
    if (entry.second.conflict)
      continue;
    auto it = indexByName.find(entry.first);
    if (it == indexByName.end())
      continue;
    scoped[it->second] = entry.second.macro;
  }
  removeConflictingCompanionConstants(scoped);
  return scoped;
}

static bool artMacroInputsEquivalent(
    const std::vector<ArtDefaultZeroMacro> &lhs,
    const std::vector<ArtDefaultZeroMacro> &rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i].name != rhs[i].name || lhs[i].artType != rhs[i].artType ||
        lhs[i].uri != rhs[i].uri || lhs[i].line != rhs[i].line ||
        lhs[i].start != rhs[i].start || lhs[i].end != rhs[i].end) {
      return false;
    }
    if (lhs[i].companionConstants.size() !=
        rhs[i].companionConstants.size()) {
      return false;
    }
    for (size_t j = 0; j < lhs[i].companionConstants.size(); ++j) {
      const auto &lc = lhs[i].companionConstants[j];
      const auto &rc = rhs[i].companionConstants[j];
      if (lc.name != rc.name || lc.value != rc.value || lc.uri != rc.uri ||
          lc.line != rc.line || lc.start != rc.start || lc.end != rc.end) {
        return false;
      }
    }
  }
  return true;
}

static PreprocessorIncludeContext withArtDefaultZeroMacros(
    const PreprocessorIncludeContext &includeContext,
    std::vector<ArtDefaultZeroMacro> artMacros) {
  PreprocessorIncludeContext scoped = includeContext;
  scoped.artDefaultZeroMacros = std::move(artMacros);
  return scoped;
}

static std::vector<LexToken>
joinVariadicArgumentTokens(const std::vector<std::vector<LexToken>> &arguments,
                           size_t startIndex, const LexToken &anchor) {
  std::vector<LexToken> joined;
  for (size_t i = startIndex; i < arguments.size(); i++) {
    if (i > startIndex)
      joined.push_back(makeSyntheticPunctToken(",", anchor));
    joined.insert(joined.end(), arguments[i].begin(), arguments[i].end());
  }
  return joined;
}

static uint64_t fnv1aStep(uint64_t hash, const std::string &value) {
  for (unsigned char ch : value) {
    hash ^= static_cast<uint64_t>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

static std::string toHex(uint64_t value) {
  std::ostringstream oss;
  oss << std::hex << value;
  return oss.str();
}

static ConfiguredPreprocessorMacros getConfiguredPreprocessorMacrosSnapshot() {
  std::lock_guard<std::mutex> lock(gConfiguredPreprocessorMacrosMutex);
  return gConfiguredPreprocessorMacros;
}

static std::string fingerprintConfiguredPreprocessorMacros(
    const ConfiguredPreprocessorMacros &macros) {
  std::vector<std::pair<std::string, std::string>> ordered(macros.begin(),
                                                           macros.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.first < rhs.first;
            });
  uint64_t hash = 1469598103934665603ull;
  for (const auto &entry : ordered) {
    hash = fnv1aStep(hash, entry.first);
    hash = fnv1aStep(hash, "=");
    hash = fnv1aStep(hash, entry.second);
    hash = fnv1aStep(hash, ";");
  }
  return toHex(hash);
}

static void hashDelimited(uint64_t &hash, const std::string &value) {
  hash = fnv1aStep(hash, value);
  hash = fnv1aStep(hash, "\n");
}

static std::string makeCompilerMacroAnalysisCacheKey(
    const std::string &kind,
    const std::string &rootText,
    const std::unordered_map<std::string, int> &defines,
    const PreprocessorIncludeContext &includeContext) {
  if (includeContext.compilerPrivateConstantCacheScope.empty())
    return {};

  uint64_t hash = 1469598103934665603ull;
  hashDelimited(hash, kind);
  hashDelimited(hash, includeContext.compilerPrivateConstantCacheScope);
  hashDelimited(hash, includeContext.currentUri);
  hashDelimited(hash, rootText);
  hashDelimited(hash, getConfiguredPreprocessorMacrosFingerprint());

  std::vector<std::pair<std::string, int>> orderedDefines(defines.begin(),
                                                          defines.end());
  std::sort(orderedDefines.begin(), orderedDefines.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.first < rhs.first;
            });
  for (const auto &entry : orderedDefines) {
    hashDelimited(hash, entry.first);
    hashDelimited(hash, std::to_string(entry.second));
  }

  for (const auto &folder : includeContext.workspaceFolders)
    hashDelimited(hash, folder);
  hashDelimited(hash, "|include-paths|");
  for (const auto &includePath : includeContext.includePaths)
    hashDelimited(hash, includePath);
  hashDelimited(hash, "|shader-extensions|");
  for (const auto &extension : includeContext.shaderExtensions)
    hashDelimited(hash, extension);

  std::vector<ArtDefaultZeroMacro> orderedArtMacros =
      includeContext.artDefaultZeroMacros;
  std::sort(orderedArtMacros.begin(), orderedArtMacros.end(),
            [](const auto &lhs, const auto &rhs) {
              if (lhs.name != rhs.name)
                return lhs.name < rhs.name;
              if (lhs.uri != rhs.uri)
                return lhs.uri < rhs.uri;
              return lhs.line < rhs.line;
            });
  for (const auto &entry : orderedArtMacros) {
    hashDelimited(hash, entry.name);
    hashDelimited(hash, entry.artType);
    hashDelimited(hash, entry.uri);
    hashDelimited(hash, std::to_string(entry.line));
    hashDelimited(hash, std::to_string(entry.start));
    hashDelimited(hash, std::to_string(entry.end));
    for (const auto &constant : entry.companionConstants) {
      hashDelimited(hash, constant.name);
      hashDelimited(hash, std::to_string(constant.value));
      hashDelimited(hash, constant.uri);
      hashDelimited(hash, std::to_string(constant.line));
      hashDelimited(hash, std::to_string(constant.start));
      hashDelimited(hash, std::to_string(constant.end));
    }
  }

  return toHex(hash);
}

static std::string makeCompilerPrivateConstantCacheKey(
    const std::string &rootText,
    const std::unordered_map<std::string, int> &defines,
    const PreprocessorIncludeContext &includeContext) {
  return makeCompilerMacroAnalysisCacheKey(
      "compiler-private-constants-v1", rootText, defines, includeContext);
}

static std::string makeCompilerMacroSnapshotCacheKey(
    const std::string &rootText,
    const std::unordered_map<std::string, int> &defines,
    const PreprocessorIncludeContext &includeContext) {
  return makeCompilerMacroAnalysisCacheKey(
      "compiler-macro-snapshot-v1", rootText, defines, includeContext);
}

static std::string makeArtCompanionScopeCacheKey(
    const std::string &rootText,
    const std::unordered_map<std::string, int> &defines,
    const PreprocessorIncludeContext &includeContext) {
  return makeCompilerMacroAnalysisCacheKey(
      "art-companion-scope-v1", rootText, defines, includeContext);
}

static bool lookupCompilerPrivateConstantCache(
    const std::string &key,
    std::vector<CompilerPrivateMacroConstant> &constantsOut) {
  if (key.empty())
    return false;
  std::lock_guard<std::mutex> lock(gCompilerPrivateConstantCacheMutex);
  auto it = gCompilerPrivateConstantCache.find(key);
  if (it == gCompilerPrivateConstantCache.end())
    return false;
  constantsOut = it->second;
  return true;
}

static void storeCompilerPrivateConstantCache(
    const std::string &key,
    const std::vector<CompilerPrivateMacroConstant> &constants) {
  if (key.empty())
    return;
  std::lock_guard<std::mutex> lock(gCompilerPrivateConstantCacheMutex);
  if (gCompilerPrivateConstantCache.find(key) ==
      gCompilerPrivateConstantCache.end()) {
    gCompilerPrivateConstantCacheOrder.push_back(key);
  }
  gCompilerPrivateConstantCache[key] = constants;
  while (gCompilerPrivateConstantCacheOrder.size() >
         kMaxCompilerPrivateConstantCacheEntries) {
    const std::string evictedKey = gCompilerPrivateConstantCacheOrder.front();
    gCompilerPrivateConstantCacheOrder.erase(
        gCompilerPrivateConstantCacheOrder.begin());
    gCompilerPrivateConstantCache.erase(evictedKey);
  }
}

static bool lookupCompilerMacroSnapshotCache(
    const std::string &key,
    std::vector<CompilerMacroSnapshotMacro> &macrosOut) {
  if (key.empty())
    return false;
  std::lock_guard<std::mutex> lock(gCompilerMacroSnapshotCacheMutex);
  auto it = gCompilerMacroSnapshotCache.find(key);
  if (it == gCompilerMacroSnapshotCache.end())
    return false;
  macrosOut = it->second;
  return true;
}

static void storeCompilerMacroSnapshotCache(
    const std::string &key,
    const std::vector<CompilerMacroSnapshotMacro> &macros) {
  if (key.empty())
    return;
  std::lock_guard<std::mutex> lock(gCompilerMacroSnapshotCacheMutex);
  if (gCompilerMacroSnapshotCache.find(key) ==
      gCompilerMacroSnapshotCache.end()) {
    gCompilerMacroSnapshotCacheOrder.push_back(key);
  }
  gCompilerMacroSnapshotCache[key] = macros;
  while (gCompilerMacroSnapshotCacheOrder.size() >
         kMaxCompilerMacroSnapshotCacheEntries) {
    const std::string evictedKey = gCompilerMacroSnapshotCacheOrder.front();
    gCompilerMacroSnapshotCacheOrder.erase(
        gCompilerMacroSnapshotCacheOrder.begin());
    gCompilerMacroSnapshotCache.erase(evictedKey);
  }
}

static bool lookupArtCompanionScopeCache(
    const std::string &key, std::vector<ArtDefaultZeroMacro> &macrosOut) {
  if (key.empty())
    return false;
  std::lock_guard<std::mutex> lock(gArtCompanionScopeCacheMutex);
  auto it = gArtCompanionScopeCache.find(key);
  if (it == gArtCompanionScopeCache.end())
    return false;
  macrosOut = it->second;
  return true;
}

static void storeArtCompanionScopeCache(
    const std::string &key, const std::vector<ArtDefaultZeroMacro> &macros) {
  if (key.empty())
    return;
  std::lock_guard<std::mutex> lock(gArtCompanionScopeCacheMutex);
  if (gArtCompanionScopeCache.find(key) == gArtCompanionScopeCache.end())
    gArtCompanionScopeCacheOrder.push_back(key);
  gArtCompanionScopeCache[key] = macros;
  while (gArtCompanionScopeCacheOrder.size() >
         kMaxArtCompanionScopeCacheEntries) {
    const std::string evictedKey = gArtCompanionScopeCacheOrder.front();
    gArtCompanionScopeCacheOrder.erase(
        gArtCompanionScopeCacheOrder.begin());
    gArtCompanionScopeCache.erase(evictedKey);
  }
}

static InitialMacroSeedStats seedInitialPreprocessorMacros(
    std::unordered_map<std::string, PreprocMacro> &macros,
    const std::unordered_map<std::string, int> &defines,
    const std::vector<ArtDefaultZeroMacro> &artDefaultZeroMacros,
    const std::vector<CompilerPrivateMacroConstant>
        &compilerPrivateConstants,
    const std::vector<CompilerMacroSnapshotMacro> &compilerMacroSnapshotMacros) {
  InitialMacroSeedStats stats;
  const auto configured = getConfiguredPreprocessorMacrosSnapshot();
  stats.artDefaultZero = artDefaultZeroMacros.size();
  stats.compilerPrivateConstants = compilerPrivateConstants.size();
  stats.configured = configured.size();
  stats.numericDefines = defines.size();
  for (const auto &entry : artDefaultZeroMacros) {
    for (const auto &constant : entry.companionConstants) {
      if (constant.name.empty())
        continue;
      PreprocMacro macro = makeNumericPreprocMacro(constant.value);
      macro.sourceUri = constant.uri;
      macro.sourceLine = constant.line;
      macro.sourceStart = constant.start;
      macro.sourceEnd = constant.end;
      macro.sourceArtCompanionConstant = true;
      macros[constant.name] = std::move(macro);
    }
    if (entry.name.empty())
      continue;
    PreprocMacro macro = makeNumericPreprocMacro(0);
    macro.sourceUri = entry.uri;
    macro.sourceLine = entry.line;
    macro.sourceStart = entry.start;
    macro.sourceEnd = entry.end;
    macro.sourceArtDefaultZero = true;
    macros[entry.name] = std::move(macro);
  }
  for (const auto &entry : compilerPrivateConstants) {
    if (entry.name.empty())
      continue;
    PreprocMacro macro = makeNumericPreprocMacro(entry.value);
    macro.sourceUri = entry.sourceUri;
    macro.sourceLine = entry.sourceLine;
    macro.sourceStart = entry.sourceStart;
    macro.sourceEnd = entry.sourceEnd;
    macro.sourceCompilerPrivateConstant = true;
    macros[entry.name] = std::move(macro);
  }
  for (const auto &entry : configured) {
    macros[entry.first] = makeReplacementPreprocMacro(entry.second);
  }
  for (const auto &entry : compilerMacroSnapshotMacros) {
    if (entry.name.empty())
      continue;
    if (entry.kind == CompilerMacroSnapshotMacroKind::PublicDefault &&
        macros.find(entry.name) != macros.end()) {
      continue;
    }
    PreprocMacro macro = makeReplacementPreprocMacro(entry.replacement);
    macro.sourceUri = entry.sourceUri;
    macro.sourceLine = entry.sourceLine;
    macro.sourceStart = entry.sourceStart;
    macro.sourceEnd = entry.sourceEnd;
    macro.sourceCompilerMacroSnapshot = true;
    macros[entry.name] = std::move(macro);
    stats.compilerMacroSnapshot++;
  }
  for (const auto &entry : defines) {
    macros[entry.first] = makeNumericPreprocMacro(entry.second);
  }
  return stats;
}

class PreprocessorExprParser {
public:
  PreprocessorExprParser(
      const std::vector<LexToken> &tokens, size_t start,
      std::unordered_map<std::string, PreprocMacro> &macros, int line,
      std::vector<PreprocessorConditionDiagnostic> &diagnostics,
      std::function<void(const LexToken &, const PreprocMacro &)> onSynthesize,
      bool inactiveBranch, int branchId, int branchIndex,
      std::string sourceUri = {},
      std::unordered_set<std::string> expansionStack = {})
      : tokens_(tokens), i_(start), macros_(macros), line_(line),
        diagnostics_(diagnostics), onSynthesize_(std::move(onSynthesize)),
        inactiveBranch_(inactiveBranch), branchId_(branchId),
        branchIndex_(branchIndex), sourceUri_(std::move(sourceUri)),
        expansionStack_(std::move(expansionStack)) {}

  int evaluate() { return parseLogicalOr(); }

private:
  const LexToken *peek() const {
    if (i_ >= tokens_.size())
      return nullptr;
    return &tokens_[i_];
  }

  const LexToken *consume() {
    if (i_ >= tokens_.size())
      return nullptr;
    return &tokens_[i_++];
  }

  bool matchPunct(const std::string &punct) {
    const LexToken *token = peek();
    if (!token || token->kind != LexToken::Kind::Punct ||
        token->text != punct) {
      return false;
    }
    consume();
    return true;
  }

  void addDiagnostic(const LexToken &token, const std::string &message,
                     bool synthesizedZero = false, int severity = 1,
                     std::string macroName = {}) {
    if (macroName.empty())
      macroName = token.text;
    diagnostics_.push_back(PreprocessorConditionDiagnostic{
        line_, static_cast<int>(token.start), static_cast<int>(token.end),
        severity, message, std::move(macroName), synthesizedZero,
        inactiveBranch_, branchId_,
        branchIndex_});
  }

  bool parseInvocationArguments(
      std::vector<std::vector<LexToken>> &argumentsOut) {
    argumentsOut.clear();
    if (!matchPunct("("))
      return false;
    int depth = 1;
    std::vector<LexToken> current;
    bool sawAnyArgumentToken = false;
    while (depth > 0) {
      const LexToken *token = consume();
      if (!token) {
        if (!current.empty())
          argumentsOut.push_back(std::move(current));
        return true;
      }
      if (token->kind == LexToken::Kind::Punct) {
        if (token->text == "(") {
          depth++;
          current.push_back(*token);
          sawAnyArgumentToken = true;
          continue;
        }
        if (token->text == ")") {
          depth--;
          if (depth == 0) {
            if (sawAnyArgumentToken || !current.empty())
              argumentsOut.push_back(std::move(current));
            break;
          }
          current.push_back(*token);
          sawAnyArgumentToken = true;
          continue;
        }
        if (token->text == "," && depth == 1) {
          argumentsOut.push_back(std::move(current));
          current = {};
          sawAnyArgumentToken = false;
          continue;
        }
      }
      current.push_back(*token);
      sawAnyArgumentToken = true;
    }
    return true;
  }

  std::vector<LexToken> expandFunctionLikeMacro(
      const LexToken &invocationToken, const PreprocMacro &macro,
      const std::vector<std::vector<LexToken>> &arguments) {
    std::unordered_map<std::string, std::vector<LexToken>> argumentMap;
    const size_t fixedCount = macro.parameters.size();
    for (size_t i = 0; i < fixedCount; i++) {
      if (i < arguments.size())
        argumentMap[macro.parameters[i]] = arguments[i];
      else
        argumentMap[macro.parameters[i]] = {};
    }

    if (macro.variadic) {
      const size_t variadicStart = std::min(fixedCount, arguments.size());
      auto variadicTokens =
          joinVariadicArgumentTokens(arguments, variadicStart, invocationToken);
      argumentMap["__VA_ARGS__"] = variadicTokens;
      if (!macro.variadicParameter.empty())
        argumentMap[macro.variadicParameter] = std::move(variadicTokens);
    } else if (arguments.size() > fixedCount) {
      addDiagnostic(invocationToken,
                    "Macro expansion argument count mismatch in "
                    "preprocessor expression: " +
                        invocationToken.text + ".",
                    false, 2, invocationToken.text);
    }

    std::vector<LexToken> substituted;
    for (size_t index = 0; index < macro.replacement.size(); index++) {
      const LexToken &token = macro.replacement[index];
      if (token.kind == LexToken::Kind::Punct && token.text == "#" &&
          index + 1 < macro.replacement.size()) {
        const LexToken &next = macro.replacement[index + 1];
        if (next.kind == LexToken::Kind::Identifier &&
            argumentMap.find(next.text) != argumentMap.end()) {
          addDiagnostic(invocationToken,
                        "Macro stringization is not numeric in "
                        "preprocessor expression: " +
                            invocationToken.text + ".",
                        false, 2, invocationToken.text);
          substituted.push_back(makeSyntheticToken("0", invocationToken));
          index++;
          continue;
        }
      }

      if (token.kind == LexToken::Kind::Identifier) {
        auto argIt = argumentMap.find(token.text);
        if (argIt != argumentMap.end()) {
          substituted.insert(substituted.end(), argIt->second.begin(),
                             argIt->second.end());
          continue;
        }
      }

      substituted.push_back(token);
    }

    std::vector<LexToken> pasted;
    bool pasteNext = false;
    for (size_t index = 0; index < substituted.size(); index++) {
      const LexToken &token = substituted[index];
      if (token.kind == LexToken::Kind::Punct && token.text == "##") {
        if (pasted.empty()) {
          addDiagnostic(invocationToken,
                        "Macro token paste has no left operand in "
                        "preprocessor expression: " +
                            invocationToken.text + ".",
                        false, 2, invocationToken.text);
        } else {
          pasteNext = true;
        }
        continue;
      }

      if (pasteNext) {
        LexToken &left = pasted.back();
        const std::string combined = left.text + token.text;
        LexToken::Kind combinedKind = LexToken::Kind::Identifier;
        if (!isTokenTextValidAfterPaste(combined, combinedKind)) {
          addDiagnostic(invocationToken,
                        "Macro token paste produced an invalid token in "
                        "preprocessor expression: " +
                            invocationToken.text + ".",
                        false, 2, invocationToken.text);
          left = makeSyntheticToken("0", invocationToken);
        } else {
          left.kind = combinedKind;
          left.text = combined;
          left.end = token.end;
        }
        pasteNext = false;
        continue;
      }

      pasted.push_back(token);
    }
    if (pasteNext) {
      addDiagnostic(invocationToken,
                    "Macro token paste has no right operand in preprocessor "
                    "expression: " +
                        invocationToken.text + ".",
                    false, 2, invocationToken.text);
    }

    return pasted;
  }

  int evaluateExpandedFunctionMacro(
      const LexToken &token, const PreprocMacro &macro,
      const std::vector<std::vector<LexToken>> &arguments) {
    if (expansionStack_.find(token.text) != expansionStack_.end()) {
      addDiagnostic(token,
                    "Recursive macro expansion in preprocessor expression: " +
                        token.text + ".",
                    false, 2, token.text);
      return 0;
    }
    if (expansionStack_.size() >= 32) {
      addDiagnostic(token,
                    "Macro expansion depth limit reached in preprocessor "
                    "expression: " +
                        token.text + ".",
                    false, 2, token.text);
      return 0;
    }

    std::vector<LexToken> expanded =
        expandFunctionLikeMacro(token, macro, arguments);
    if (expanded.empty())
      return 0;

    auto nestedExpansionStack = expansionStack_;
    nestedExpansionStack.insert(token.text);
    PreprocessorExprParser nested(expanded, 0, macros_, line_, diagnostics_,
                                  onSynthesize_, inactiveBranch_, branchId_,
                                  branchIndex_, sourceUri_,
                                  std::move(nestedExpansionStack));
    return nested.evaluate();
  }

  bool nextTokenStartsInvocation() const {
    const LexToken *token = peek();
    return token && token->kind == LexToken::Kind::Punct &&
           token->text == "(";
  }

  void skipFunctionLikeInvocation() {
    std::vector<std::vector<LexToken>> ignored;
    parseInvocationArguments(ignored);
  }

  bool isFunctionLikeInvocation() const {
    return nextTokenStartsInvocation();
  }

  int evaluateFunctionLikeMacro(const LexToken &token,
                                const PreprocMacro &macro) {
    if (!isFunctionLikeInvocation())
      return 0;

    std::vector<std::vector<LexToken>> arguments;
    if (!parseInvocationArguments(arguments)) {
      addDiagnostic(token,
                    "Malformed macro invocation in preprocessor expression: " +
                        token.text + ".",
                    false, 2, token.text);
      return 0;
    }
    return evaluateExpandedFunctionMacro(token, macro, arguments);
  }

  void consumeFunctionLikeInvocation() {
    skipFunctionLikeInvocation();
  }

  int parseDefinedExpr() {
    consume();
    bool hasParen = matchPunct("(");
    std::string name;
    const LexToken *token = peek();
    if (token && token->kind == LexToken::Kind::Identifier) {
      name = token->text;
      consume();
    }
    if (hasParen)
      matchPunct(")");
    if (name.empty())
      return 0;
    return macros_.find(name) != macros_.end() ? 1 : 0;
  }

  int evaluateMacro(const LexToken &token) {
    auto it = macros_.find(token.text);
    if (it == macros_.end()) {
      addDiagnostic(token, "Undefined macro in preprocessor expression: " +
                               token.text + ".",
                    true);
      PreprocMacro synthesized = makeNumericPreprocMacro(0);
      synthesized.sourceUri = sourceUri_;
      synthesized.sourceLine = line_;
      synthesized.sourceStart = static_cast<int>(token.start);
      synthesized.sourceEnd = static_cast<int>(token.end);
      synthesized.sourceSynthesizedZero = true;
      macros_[token.text] = synthesized;
      if (onSynthesize_)
        onSynthesize_(token, synthesized);
      return 0;
    }

    const PreprocMacro &macro = it->second;
    if (macro.functionLike) {
      return evaluateFunctionLikeMacro(token, macro);
    }

    if (macro.replacement.empty())
      return 0;

    if (expansionStack_.find(token.text) != expansionStack_.end()) {
      addDiagnostic(token,
                    "Recursive macro expansion in preprocessor expression: " +
                        token.text + ".");
      return 0;
    }

    auto nestedExpansionStack = expansionStack_;
    nestedExpansionStack.insert(token.text);
    PreprocessorExprParser nested(macro.replacement, 0, macros_, line_,
                                  diagnostics_, onSynthesize_, inactiveBranch_,
                                  branchId_, branchIndex_, sourceUri_,
                                  std::move(nestedExpansionStack));
    return nested.evaluate();
  }

  int parsePrimary() {
    if (matchPunct("(")) {
      int value = parseLogicalOr();
      matchPunct(")");
      return value;
    }

    const LexToken *token = peek();
    if (!token)
      return 0;

    if (token->kind == LexToken::Kind::Identifier && token->text == "defined")
      return parseDefinedExpr();

    if (token->kind == LexToken::Kind::Identifier) {
      LexToken identifier = *consume();
      int parsed = 0;
      if (parseIntToken(identifier.text, parsed))
        return parsed;
      return evaluateMacro(identifier);
    }

    consume();
    return 0;
  }

  int parseUnary() {
    if (matchPunct("!"))
      return parseUnary() == 0 ? 1 : 0;
    if (matchPunct("+"))
      return parseUnary();
    if (matchPunct("-"))
      return -parseUnary();
    if (matchPunct("~"))
      return ~parseUnary();
    return parsePrimary();
  }

  int parseMultiplicative() {
    int left = parseUnary();
    while (true) {
      if (matchPunct("*")) {
        left *= parseUnary();
        continue;
      }
      if (matchPunct("/")) {
        int right = parseUnary();
        left = right == 0 ? 0 : left / right;
        continue;
      }
      if (matchPunct("%")) {
        int right = parseUnary();
        left = right == 0 ? 0 : left % right;
        continue;
      }
      break;
    }
    return left;
  }

  int parseAdditive() {
    int left = parseMultiplicative();
    while (true) {
      if (matchPunct("+")) {
        left += parseMultiplicative();
        continue;
      }
      if (matchPunct("-")) {
        left -= parseMultiplicative();
        continue;
      }
      break;
    }
    return left;
  }

  int parseShift() {
    int left = parseAdditive();
    while (true) {
      if (matchPunct("<<")) {
        left <<= parseAdditive();
        continue;
      }
      if (matchPunct(">>")) {
        left >>= parseAdditive();
        continue;
      }
      break;
    }
    return left;
  }

  int parseRelational() {
    int left = parseShift();
    while (true) {
      if (matchPunct("<=")) {
        left = left <= parseShift() ? 1 : 0;
        continue;
      }
      if (matchPunct(">=")) {
        left = left >= parseShift() ? 1 : 0;
        continue;
      }
      if (matchPunct("<")) {
        left = left < parseShift() ? 1 : 0;
        continue;
      }
      if (matchPunct(">")) {
        left = left > parseShift() ? 1 : 0;
        continue;
      }
      break;
    }
    return left;
  }

  int parseEquality() {
    int left = parseRelational();
    while (true) {
      if (matchPunct("==")) {
        left = left == parseRelational() ? 1 : 0;
        continue;
      }
      if (matchPunct("!=")) {
        left = left != parseRelational() ? 1 : 0;
        continue;
      }
      break;
    }
    return left;
  }

  int parseBitwiseAnd() {
    int left = parseEquality();
    while (matchPunct("&"))
      left &= parseEquality();
    return left;
  }

  int parseBitwiseXor() {
    int left = parseBitwiseAnd();
    while (matchPunct("^"))
      left ^= parseBitwiseAnd();
    return left;
  }

  int parseBitwiseOr() {
    int left = parseBitwiseXor();
    while (matchPunct("|"))
      left |= parseBitwiseXor();
    return left;
  }

  int parseLogicalAnd() {
    int left = parseBitwiseOr();
    while (matchPunct("&&")) {
      const int right = parseBitwiseOr();
      left = (left != 0 && right != 0) ? 1 : 0;
    }
    return left;
  }

  int parseLogicalOr() {
    int left = parseLogicalAnd();
    while (matchPunct("||")) {
      const int right = parseLogicalAnd();
      left = (left != 0 || right != 0) ? 1 : 0;
    }
    return left;
  }

  const std::vector<LexToken> &tokens_;
  size_t i_ = 0;
  std::unordered_map<std::string, PreprocMacro> &macros_;
  int line_ = 0;
  std::vector<PreprocessorConditionDiagnostic> &diagnostics_;
  std::function<void(const LexToken &, const PreprocMacro &)> onSynthesize_;
  bool inactiveBranch_ = false;
  int branchId_ = 0;
  int branchIndex_ = 0;
  std::string sourceUri_;
  std::unordered_set<std::string> expansionStack_;
};

static int evalPreprocessorExpr(
    const std::vector<LexToken> &tokens, size_t start,
    std::unordered_map<std::string, PreprocMacro> &macros, int line,
    std::vector<PreprocessorConditionDiagnostic> &diagnostics,
    std::function<void(const LexToken &, const PreprocMacro &)> onSynthesize,
    bool inactiveBranch, int branchId, int branchIndex,
    const std::string &sourceUri = std::string()) {
  PreprocessorExprParser parser(tokens, start, macros, line, diagnostics,
                                std::move(onSynthesize), inactiveBranch,
                                branchId, branchIndex, sourceUri);
  return parser.evaluate();
}

struct ActiveFrame {
  bool parentActive = true;
  bool parentMacroActive = true;
  bool currentActive = true;
  bool branchChosen = false;
};

struct BranchFrame {
  int id = 0;
  int branchIndex = 0;
  int nextBranchIndex = 1;
};

struct PreprocessorInterpreterState {
  const ConditionalAst &ast;
  std::unordered_map<std::string, PreprocMacro> macros;
  PreprocessorView result;
  std::vector<ActiveFrame> activeStack;
  std::vector<BranchFrame> branchStack;
  std::vector<std::string> defaultGuardStack;
  bool active = true;
  bool macroActive = true;
  bool speculativeInactive = false;
  int nextBranchId = 1;
  const PreprocessorIncludeContext *includeContext = nullptr;
  std::string currentUri;
  int includeDepth = 0;
  std::unordered_map<std::string, ConditionalAst> *includeAstCache = nullptr;
  std::unordered_set<std::string> *includeExpansionStack = nullptr;
  struct IncludedDocumentCapture *includedCapture = nullptr;
};

struct IncludedDocumentCapture {
  std::string targetUri;
  bool found = false;
  PreprocessorView view;
};

static void initializeLineStateStorage(PreprocessorInterpreterState &state) {
  state.result.lineActive.assign(state.ast.lines.size(), 0);
  state.result.branchSigs.resize(state.ast.lines.size());
}

static void snapshotInitialMacroState(PreprocessorInterpreterState &state) {
  state.result.initialMacroReplacements.clear();
  state.result.initialMacroReplacements.reserve(state.macros.size());
  for (const auto &entry : state.macros) {
    state.result.initialMacroReplacements.emplace(
        entry.first, toPublicMacroReplacement(entry.second));
  }
  state.result.macroEvents.clear();
  state.result.macroHealth.initialMacroCount =
      state.result.initialMacroReplacements.size();
}

static int currentBranchId(const PreprocessorInterpreterState &state) {
  return state.branchStack.empty() ? 0 : state.branchStack.back().id;
}

static int currentBranchIndex(const PreprocessorInterpreterState &state) {
  return state.branchStack.empty() ? 0 : state.branchStack.back().branchIndex;
}

static void mergeProbeObservations(PreprocessorInterpreterState &state,
                                   const PreprocessorView &probeView) {
  const size_t lineCount =
      std::min(state.result.branchSigs.size(), probeView.branchSigs.size());
  for (size_t line = 0; line < lineCount; line++) {
    if (!state.result.branchSigs[line].empty() ||
        probeView.branchSigs[line].empty()) {
      continue;
    }
    state.result.branchSigs[line] = probeView.branchSigs[line];
  }
  state.result.conditionDiagnostics.insert(
      state.result.conditionDiagnostics.end(),
      probeView.conditionDiagnostics.begin(),
      probeView.conditionDiagnostics.end());
  state.result.branchMerges.insert(state.result.branchMerges.end(),
                                   probeView.branchMerges.begin(),
                                   probeView.branchMerges.end());
}

static void recordMacroDefine(PreprocessorInterpreterState &state, int line,
                              const std::string &name,
                              const PreprocMacro &macro,
                              bool synthesizedZero = false) {
  PreprocessorMacroEvent event;
  event.line = line;
  event.name = name;
  event.synthesizedZero = synthesizedZero;
  event.ifndefDefault =
      !event.synthesizedZero &&
      std::find(state.defaultGuardStack.begin(), state.defaultGuardStack.end(),
                name) != state.defaultGuardStack.end();
  event.artDefaultZero = macro.sourceArtDefaultZero;
  event.compilerPrivateConstant = macro.sourceCompilerPrivateConstant;
  event.compilerMacroSnapshot = macro.sourceCompilerMacroSnapshot;
  event.replacement = toPublicMacroReplacement(macro);
  event.sourceUri = macro.sourceUri.empty() ? state.currentUri : macro.sourceUri;
  event.sourceLine = macro.sourceLine >= 0 ? macro.sourceLine : line;
  event.sourceStart = macro.sourceLine >= 0 ? macro.sourceStart : 0;
  event.sourceEnd =
      macro.sourceLine >= 0 ? macro.sourceEnd : static_cast<int>(name.size());
  state.result.macroEvents.push_back(std::move(event));
}

static void recordMacroUndef(PreprocessorInterpreterState &state, int line,
                             const std::string &name) {
  PreprocessorMacroEvent event;
  event.line = line;
  event.name = name;
  event.undefined = true;
  event.sourceUri = state.currentUri;
  event.sourceLine = line;
  event.sourceStart = 0;
  event.sourceEnd = static_cast<int>(name.size());
  state.result.macroEvents.push_back(std::move(event));
}

static bool preprocMacroEquivalent(const PreprocMacro &lhs,
                                   const PreprocMacro &rhs) {
  return lhs.functionLike == rhs.functionLike &&
         lhs.parameters == rhs.parameters && lhs.variadic == rhs.variadic &&
         lhs.variadicParameter == rhs.variadicParameter &&
         lhs.sourceUri == rhs.sourceUri && lhs.sourceLine == rhs.sourceLine &&
         lhs.sourceStart == rhs.sourceStart && lhs.sourceEnd == rhs.sourceEnd &&
         lhs.sourceSynthesizedZero == rhs.sourceSynthesizedZero &&
         lhs.sourceIfndefDefault == rhs.sourceIfndefDefault &&
         lhs.sourceArtDefaultZero == rhs.sourceArtDefaultZero &&
         lhs.sourceArtCompanionConstant == rhs.sourceArtCompanionConstant &&
         lhs.sourceCompilerPrivateConstant ==
             rhs.sourceCompilerPrivateConstant &&
         lhs.sourceCompilerMacroSnapshot ==
             rhs.sourceCompilerMacroSnapshot &&
         toPublicMacroReplacement(lhs).replacement ==
             toPublicMacroReplacement(rhs).replacement;
}

static void parseFunctionLikeMacroParameters(const std::vector<LexToken> &tokens,
                                             size_t openParenIndex,
                                             size_t closeParenIndex,
                                             PreprocMacro &macro) {
  macro.parameters.clear();
  macro.variadic = false;
  macro.variadicParameter.clear();
  if (openParenIndex >= closeParenIndex || closeParenIndex > tokens.size())
    return;

  size_t partStart = openParenIndex + 1;
  auto consumeParameterPart = [&](size_t start, size_t end) {
    while (start < end && tokens[start].kind == LexToken::Kind::Punct &&
           tokens[start].text == ",")
      start++;
    while (end > start && tokens[end - 1].kind == LexToken::Kind::Punct &&
           tokens[end - 1].text == ",")
      end--;
    if (start >= end)
      return;

    if (start + 1 == end && isEllipsisAt(tokens, start)) {
      macro.variadic = true;
      macro.variadicParameter = "__VA_ARGS__";
      return;
    }
    if (start + 2 == end && tokens[start].kind == LexToken::Kind::Identifier &&
        isEllipsisAt(tokens, start + 1)) {
      macro.variadic = true;
      macro.variadicParameter = tokens[start].text;
      return;
    }
    if (tokens[start].kind == LexToken::Kind::Identifier) {
      if (start + 1 < end && isEllipsisAt(tokens, start + 1)) {
        macro.variadic = true;
        macro.variadicParameter = tokens[start].text;
        return;
      }
      macro.parameters.push_back(tokens[start].text);
    }
  };

  for (size_t index = openParenIndex + 1; index < closeParenIndex; index++) {
    if (tokens[index].kind == LexToken::Kind::Punct &&
        tokens[index].text == ",") {
      consumeParameterPart(partStart, index);
      partStart = index + 1;
    }
  }
  consumeParameterPart(partStart, closeParenIndex);
}

static void recordMacroDelta(
    PreprocessorInterpreterState &state, int line,
    const std::unordered_map<std::string, PreprocMacro> &before,
    const std::unordered_map<std::string, PreprocMacro> &after) {
  for (const auto &entry : after) {
    auto old = before.find(entry.first);
    if (old != before.end() && preprocMacroEquivalent(old->second, entry.second))
      continue;
    recordMacroDefine(state, line, entry.first, entry.second);
  }
  for (const auto &entry : before) {
    if (after.find(entry.first) != after.end())
      continue;
    recordMacroUndef(state, line, entry.first);
  }
}

static std::string
extractDefaultGuardMacroName(const ConditionalAstLine &line) {
  if (!line.isDirective)
    return std::string();
  if (line.directiveKind == ConditionalDirectiveKind::Ifndef) {
    if (line.tokens.size() >= 3 &&
        line.tokens[2].kind == LexToken::Kind::Identifier) {
      return line.tokens[2].text;
    }
    return std::string();
  }
  if (line.directiveKind != ConditionalDirectiveKind::If ||
      line.tokens.size() < 5) {
    return std::string();
  }
  size_t index = 2;
  if (!(line.tokens[index].kind == LexToken::Kind::Punct &&
        line.tokens[index].text == "!")) {
    return std::string();
  }
  index++;
  if (!(line.tokens[index].kind == LexToken::Kind::Identifier &&
        line.tokens[index].text == "defined")) {
    return std::string();
  }
  index++;
  if (index < line.tokens.size() &&
      line.tokens[index].kind == LexToken::Kind::Punct &&
      line.tokens[index].text == "(") {
    index++;
    if (index < line.tokens.size() &&
        line.tokens[index].kind == LexToken::Kind::Identifier) {
      return line.tokens[index].text;
    }
    return std::string();
  }
  if (index < line.tokens.size() &&
      line.tokens[index].kind == LexToken::Kind::Identifier) {
    return line.tokens[index].text;
  }
  return std::string();
}

static void refreshMacroHealthMetrics(PreprocessorView &view) {
  view.macroHealth.sourceDefineEvents = 0;
  view.macroHealth.ifndefDefaultDefineEvents = 0;
  view.macroHealth.sourceUndefEvents = 0;
  view.macroHealth.synthesizedZeroEvents = 0;
  for (const auto &event : view.macroEvents) {
    if (event.undefined) {
      view.macroHealth.sourceUndefEvents++;
      continue;
    }
    if (event.synthesizedZero) {
      view.macroHealth.synthesizedZeroEvents++;
      continue;
    }
    view.macroHealth.sourceDefineEvents++;
    if (event.ifndefDefault)
      view.macroHealth.ifndefDefaultDefineEvents++;
  }
  view.macroHealth.conditionDiagnosticCount =
      view.conditionDiagnostics.size();
  view.macroHealth.undefinedMacroDiagnosticCount = 0;
  view.macroHealth.expansionWarningDiagnosticCount = 0;
  view.macroHealth.inactiveBranchDiagnosticCount = 0;
  for (const auto &diagnostic : view.conditionDiagnostics) {
    if (diagnostic.synthesizedZero)
      view.macroHealth.undefinedMacroDiagnosticCount++;
    if (diagnostic.severity == 2 && !diagnostic.synthesizedZero)
      view.macroHealth.expansionWarningDiagnosticCount++;
    if (diagnostic.inactiveBranch)
      view.macroHealth.inactiveBranchDiagnosticCount++;
  }
  view.macroHealth.branchMergeCount = view.branchMerges.size();
  view.macroHealth.activeIncludeCount = view.activeIncludeUris.size();
}

static void normalizeDocumentTextInPlace(std::string &text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (char ch : text) {
    if (ch != '\r')
      normalized.push_back(ch);
  }
  text.swap(normalized);
}

static bool readTextFromDiskPath(const std::string &path, std::string &text) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::string content((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof())
    return false;
  text = std::move(content);
  normalizeDocumentTextInPlace(text);
  return true;
}

static bool loadIncludeDocumentText(PreprocessorInterpreterState &state,
                                    const std::string &uri,
                                    std::string &text) {
  text.clear();
  if (state.includeContext && state.includeContext->loadText) {
    if (state.includeContext->loadText(uri, text)) {
      normalizeDocumentTextInPlace(text);
      return true;
    }
  }

  std::string path = uriToPath(uri);
  if (path.empty())
    path = uri;
  if (path.empty())
    return false;
  return readTextFromDiskPath(path, text);
}

static bool loadIncludeConditionalAst(PreprocessorInterpreterState &state,
                                      const std::string &uri,
                                      const ConditionalAst *&astOut) {
  astOut = nullptr;
  if (uri.empty() || !state.includeAstCache)
    return false;
  auto cacheIt = state.includeAstCache->find(uri);
  if (cacheIt != state.includeAstCache->end()) {
    astOut = &cacheIt->second;
    return true;
  }

  std::string text;
  if (!loadIncludeDocumentText(state, uri, text))
    return false;

  auto inserted =
      state.includeAstCache->emplace(uri, buildConditionalAst(text));
  astOut = &inserted.first->second;
  return true;
}

static bool parseIncludePathFromDirective(const ConditionalAstLine &line,
                                          std::string &includePath) {
  includePath.clear();
  if (!line.isDirective || line.directiveKind != ConditionalDirectiveKind::Include)
    return false;
  if (line.tokens.size() < 3)
    return false;
  const size_t start = line.tokens[2].start;
  if (start >= line.text.size())
    return false;

  char open = line.text[start];
  char close = '\0';
  if (open == '"')
    close = '"';
  else if (open == '<')
    close = '>';
  else
    return false;

  const size_t end = line.text.find(close, start + 1);
  if (end == std::string::npos || end <= start + 1)
    return false;
  includePath = line.text.substr(start + 1, end - start - 1);
  return !includePath.empty();
}

static void writeLineState(PreprocessorInterpreterState &state, int line) {
  if (line < 0 || line >= static_cast<int>(state.result.lineActive.size()))
    return;
  PreprocBranchSig sig;
  sig.reserve(state.branchStack.size());
  for (const auto &frame : state.branchStack) {
    sig.push_back({frame.id, frame.branchIndex});
  }
  state.result.branchSigs[line] = std::move(sig);
  state.result.lineActive[line] = state.active ? 1 : 0;
}

static bool evaluateConditionalDirective(
    PreprocessorInterpreterState &state, const ConditionalAstLine &line) {
  if (!state.macroActive)
    return false;

  if (line.directiveKind == ConditionalDirectiveKind::Ifdef ||
      line.directiveKind == ConditionalDirectiveKind::Ifndef) {
    if (line.tokens.size() < 3 ||
        line.tokens[2].kind != LexToken::Kind::Identifier) {
      return false;
    }
    const bool defined = state.macros.find(line.tokens[2].text) !=
                         state.macros.end();
    return line.directiveKind == ConditionalDirectiveKind::Ifdef ? defined
                                                                 : !defined;
  }

  return evalPreprocessorExpr(
             line.tokens, 2, state.macros, line.line,
             state.result.conditionDiagnostics,
             [&](const LexToken &token, const PreprocMacro &macro) {
               recordMacroDefine(state, line.line, token.text, macro, true);
             },
             state.speculativeInactive, currentBranchId(state),
             currentBranchIndex(state), state.currentUri) != 0;
}

static void interpretNode(PreprocessorInterpreterState &state,
                          const ConditionalAstNode &node);
static void interpretNodeList(PreprocessorInterpreterState &state,
                              const std::vector<size_t> &nodeIndices);

static void interpretIncludeDirective(PreprocessorInterpreterState &state,
                                      const ConditionalAstLine &line) {
  if (!state.macroActive || !state.includeContext)
    return;
  const int maxDepth =
      std::max(1, state.includeContext->maxDepth <= 0
                      ? 32
                      : state.includeContext->maxDepth);
  if (state.includeDepth >= maxDepth)
    return;
  if (state.currentUri.empty())
    return;

  std::string includePath;
  if (!parseIncludePathFromDirective(line, includePath))
    return;

  auto candidates =
      resolveIncludeCandidates(state.currentUri, includePath,
                               state.includeContext->workspaceFolders,
                               state.includeContext->includePaths,
                               state.includeContext->shaderExtensions);
  for (const auto &candidatePath : candidates) {
    std::string candidateUri = pathToUri(candidatePath);
    if (candidateUri.empty())
      candidateUri = candidatePath;
    if (candidateUri.empty())
      continue;
    if (state.includeExpansionStack &&
        std::find_if(state.includeExpansionStack->begin(),
                     state.includeExpansionStack->end(),
                     [&](const std::string &seenUri) {
                       return uriEquivalent(seenUri, candidateUri);
                     }) != state.includeExpansionStack->end()) {
      continue;
    }

    const ConditionalAst *includeAst = nullptr;
    if (!loadIncludeConditionalAst(state, candidateUri, includeAst) ||
        !includeAst) {
      continue;
    }
    const auto macrosBeforeInclude = state.macros;
    if (state.active &&
        std::find(state.result.activeIncludeUris.begin(),
                  state.result.activeIncludeUris.end(),
                  candidateUri) == state.result.activeIncludeUris.end()) {
      state.result.activeIncludeUris.push_back(candidateUri);
    }

    PreprocessorInterpreterState includeState{*includeAst};
    includeState.macros = state.macros;
    includeState.includeContext = state.includeContext;
    includeState.currentUri = candidateUri;
    includeState.includeDepth = state.includeDepth + 1;
    includeState.active = state.active;
    includeState.macroActive = state.macroActive;
    includeState.speculativeInactive = state.speculativeInactive;
    includeState.nextBranchId = state.nextBranchId;
    includeState.includeAstCache = state.includeAstCache;
    includeState.includeExpansionStack = state.includeExpansionStack;
    includeState.includedCapture = state.includedCapture;
    initializeLineStateStorage(includeState);
    snapshotInitialMacroState(includeState);
    includeState.result.macroHealth.initialArtDefaultZeroMacroCount =
        state.result.macroHealth.initialArtDefaultZeroMacroCount;
    includeState.result.macroHealth.initialCompilerPrivateConstantCount =
        state.result.macroHealth.initialCompilerPrivateConstantCount;
    includeState.result.macroHealth.initialCompilerMacroSnapshotCount =
        state.result.macroHealth.initialCompilerMacroSnapshotCount;
    includeState.result.macroHealth.initialConfiguredMacroCount =
        state.result.macroHealth.initialConfiguredMacroCount;
    includeState.result.macroHealth.initialNumericDefineCount =
        state.result.macroHealth.initialNumericDefineCount;

    if (includeState.includeExpansionStack)
      includeState.includeExpansionStack->insert(candidateUri);
    interpretNodeList(includeState, includeAst->rootNodeIndices);
    if (includeState.includeExpansionStack)
      includeState.includeExpansionStack->erase(candidateUri);

    if (state.active && state.includedCapture && !state.includedCapture->found &&
        uriEquivalent(candidateUri, state.includedCapture->targetUri)) {
      state.includedCapture->view = includeState.result;
      state.includedCapture->found = true;
    }

    recordMacroDelta(state, line.line, macrosBeforeInclude,
                     includeState.macros);
    state.macros = std::move(includeState.macros);
    if (state.includeContext->collectIncludeConditionDiagnostics &&
        !includeState.result.conditionDiagnostics.empty()) {
      state.result.conditionDiagnostics.insert(
          state.result.conditionDiagnostics.end(),
          includeState.result.conditionDiagnostics.begin(),
          includeState.result.conditionDiagnostics.end());
    }
    for (const auto &includeUri : includeState.result.activeIncludeUris) {
      if (std::find(state.result.activeIncludeUris.begin(),
                    state.result.activeIncludeUris.end(),
                    includeUri) == state.result.activeIncludeUris.end()) {
        state.result.activeIncludeUris.push_back(includeUri);
      }
    }
    return;
  }
}

static void applyDirectiveSideEffects(PreprocessorInterpreterState &state,
                                      const ConditionalAstLine &line) {
  if (!state.macroActive || !line.isDirective)
    return;

  if (line.directiveKind == ConditionalDirectiveKind::Define) {
    if (line.tokens.size() >= 3 &&
        line.tokens[2].kind == LexToken::Kind::Identifier) {
      std::string name = line.tokens[2].text;
      PreprocMacro macro;
      size_t replacementStart = 3;
      if (line.tokens.size() >= 4 &&
          line.tokens[3].kind == LexToken::Kind::Punct &&
          line.tokens[3].text == "(" &&
          line.tokens[2].end == line.tokens[3].start) {
        macro.functionLike = true;
        int depth = 0;
        size_t index = 3;
        size_t closeParenIndex = 3;
        for (; index < line.tokens.size(); index++) {
          const auto &token = line.tokens[index];
          if (token.kind != LexToken::Kind::Punct)
            continue;
          if (token.text == "(") {
            depth++;
            continue;
          }
          if (token.text == ")") {
            depth--;
            if (depth == 0) {
              closeParenIndex = index;
              index++;
              break;
            }
          }
        }
        parseFunctionLikeMacroParameters(line.tokens, 3, closeParenIndex,
                                         macro);
        replacementStart = index;
      }
      if (replacementStart < line.tokens.size()) {
        macro.replacement.assign(line.tokens.begin() + replacementStart,
                                 line.tokens.end());
        macro.replacementText = tokensToReplacementText(macro.replacement);
      }
      macro.sourceUri = state.currentUri;
      macro.sourceLine = line.line;
      macro.sourceStart = static_cast<int>(line.tokens[2].start);
      macro.sourceEnd = static_cast<int>(line.tokens[2].end);
      macro.sourceIfndefDefault =
          std::find(state.defaultGuardStack.begin(),
                    state.defaultGuardStack.end(),
                    name) != state.defaultGuardStack.end();
      recordMacroDefine(state, line.line, name, macro);
      state.macros[name] = std::move(macro);
    }
    return;
  }

  if (line.directiveKind == ConditionalDirectiveKind::Undef) {
    if (line.tokens.size() >= 3 &&
        line.tokens[2].kind == LexToken::Kind::Identifier) {
      recordMacroUndef(state, line.line, line.tokens[2].text);
      state.macros.erase(line.tokens[2].text);
    }
    return;
  }

  if (line.directiveKind == ConditionalDirectiveKind::Include) {
    interpretIncludeDirective(state, line);
  }
}

static void interpretNodeList(PreprocessorInterpreterState &state,
                              const std::vector<size_t> &nodeIndices) {
  for (size_t nodeIndex : nodeIndices) {
    if (nodeIndex >= state.ast.nodes.size())
      continue;
    interpretNode(state, state.ast.nodes[nodeIndex]);
  }
}

static void interpretInactiveBranchProbe(
    PreprocessorInterpreterState &state, const ConditionalAstBranch &branch,
    int branchId, int branchIndex,
    const std::unordered_map<std::string, PreprocMacro> &initialMacros,
    bool evaluateDirective) {
  PreprocessorInterpreterState probe{state.ast};
  probe.macros = initialMacros;
  probe.includeContext = state.includeContext;
  probe.currentUri = state.currentUri;
  probe.includeDepth = state.includeDepth;
  probe.includeAstCache = state.includeAstCache;
  probe.includeExpansionStack = state.includeExpansionStack;
  probe.includedCapture = state.includedCapture;
  probe.nextBranchId = state.nextBranchId;
  probe.active = false;
  probe.macroActive = true;
  probe.speculativeInactive = true;
  probe.activeStack = state.activeStack;
  probe.branchStack = state.branchStack;
  if (probe.branchStack.empty()) {
    BranchFrame frame;
    frame.id = branchId;
    frame.branchIndex = branchIndex;
    frame.nextBranchIndex = branchIndex + 1;
    probe.branchStack.push_back(frame);
  } else {
    probe.branchStack.back().id = branchId;
    probe.branchStack.back().branchIndex = branchIndex;
    probe.branchStack.back().nextBranchIndex =
        std::max(probe.branchStack.back().nextBranchIndex, branchIndex + 1);
  }
  initializeLineStateStorage(probe);
  snapshotInitialMacroState(probe);

  if (evaluateDirective &&
      branch.directiveKind != ConditionalDirectiveKind::Else) {
    evaluateConditionalDirective(probe, state.ast.lines[branch.directiveLine]);
  }
  const std::string guardName =
      extractDefaultGuardMacroName(state.ast.lines[branch.directiveLine]);
  if (!guardName.empty())
    probe.defaultGuardStack.push_back(guardName);
  interpretNodeList(probe, branch.childNodeIndices);
  if (!guardName.empty() && !probe.defaultGuardStack.empty())
    probe.defaultGuardStack.pop_back();

  state.nextBranchId = std::max(state.nextBranchId, probe.nextBranchId);
  refreshMacroHealthMetrics(probe.result);
  mergeProbeObservations(state, probe.result);
}

static void interpretConditionalNode(PreprocessorInterpreterState &state,
                                     const ConditionalAstNode &node) {
  if (node.branches.empty())
    return;

  const auto macrosBeforeConditional = state.macros;

  BranchFrame branchFrame;
  branchFrame.id = state.nextBranchId++;
  branchFrame.branchIndex = 0;
  branchFrame.nextBranchIndex = 1;
  state.branchStack.push_back(branchFrame);

  ActiveFrame activeFrame;
  activeFrame.parentActive = state.active;
  activeFrame.parentMacroActive = state.macroActive;
  state.activeStack.push_back(activeFrame);
  int activeBranchIndex = -1;

  for (size_t i = 0; i < node.branches.size(); i++) {
    const ConditionalAstBranch &branch = node.branches[i];
    if (state.branchStack.empty() || state.activeStack.empty())
      break;

    BranchFrame &currentBranch = state.branchStack.back();
    ActiveFrame &currentActive = state.activeStack.back();
    currentBranch.branchIndex = static_cast<int>(i);
    currentBranch.nextBranchIndex =
        std::max(currentBranch.nextBranchIndex, static_cast<int>(i + 1));

    state.active = currentActive.parentActive;
    state.macroActive = currentActive.parentMacroActive;
    writeLineState(state, branch.directiveLine);

    bool branchActive = false;
    bool directiveEvaluatedOnMainPath = false;
    if (i == 0) {
      if (currentActive.parentMacroActive &&
          (currentActive.parentActive || state.speculativeInactive)) {
        const bool conditionActive = evaluateConditionalDirective(
            state, state.ast.lines[branch.directiveLine]);
        branchActive = currentActive.parentActive && conditionActive;
        directiveEvaluatedOnMainPath = true;
      }
    } else if (branch.directiveKind == ConditionalDirectiveKind::Elif) {
      if (currentActive.parentMacroActive &&
          ((currentActive.parentActive && !currentActive.branchChosen) ||
           state.speculativeInactive)) {
        const bool conditionActive = evaluateConditionalDirective(
            state, state.ast.lines[branch.directiveLine]);
        branchActive = currentActive.parentActive &&
                       !currentActive.branchChosen && conditionActive;
        directiveEvaluatedOnMainPath = true;
      }
    } else if (branch.directiveKind == ConditionalDirectiveKind::Else) {
      branchActive = currentActive.parentActive && !currentActive.branchChosen;
    }
    if (branchActive) {
      currentActive.branchChosen = true;
      activeBranchIndex = static_cast<int>(i);
    }

    currentActive.currentActive = branchActive;
    state.active = branchActive;
    state.macroActive =
        currentActive.parentMacroActive &&
        (branchActive || state.speculativeInactive);

    if (state.macroActive) {
      const std::string guardName =
          extractDefaultGuardMacroName(state.ast.lines[branch.directiveLine]);
      if (!guardName.empty())
        state.defaultGuardStack.push_back(guardName);
      interpretNodeList(state, branch.childNodeIndices);
      if (!guardName.empty() && !state.defaultGuardStack.empty())
        state.defaultGuardStack.pop_back();
    } else if (currentActive.parentMacroActive) {
      const auto probeMacros =
          directiveEvaluatedOnMainPath ? state.macros : macrosBeforeConditional;
      interpretInactiveBranchProbe(
          state, branch, branchFrame.id, static_cast<int>(i), probeMacros,
          !directiveEvaluatedOnMainPath);
    }
  }

  if (node.endifLine >= 0) {
    writeLineState(state, node.endifLine);
    PreprocessorBranchMergeInfo merge;
    merge.line = node.endifLine;
    merge.branchId = branchFrame.id;
    merge.activeBranchIndex = activeBranchIndex;
    merge.branchCount = static_cast<int>(node.branches.size());
    state.result.branchMerges.push_back(std::move(merge));
  }

  bool parentActive = true;
  bool parentMacroActive = true;
  if (!state.activeStack.empty()) {
    parentActive = state.activeStack.back().parentActive;
    parentMacroActive = state.activeStack.back().parentMacroActive;
    state.activeStack.pop_back();
  }
  state.active = parentActive;
  state.macroActive = parentMacroActive;
  if (!state.branchStack.empty())
    state.branchStack.pop_back();
}

static void interpretNode(PreprocessorInterpreterState &state,
                          const ConditionalAstNode &node) {
  if (node.kind == ConditionalAstNode::Kind::Conditional) {
    interpretConditionalNode(state, node);
    return;
  }

  if (node.line < 0 || node.line >= static_cast<int>(state.ast.lines.size()))
    return;
  writeLineState(state, node.line);
  applyDirectiveSideEffects(state, state.ast.lines[node.line]);
}

} // namespace

void setConfiguredPreprocessorMacros(
    const ConfiguredPreprocessorMacros &macros) {
  std::lock_guard<std::mutex> lock(gConfiguredPreprocessorMacrosMutex);
  gConfiguredPreprocessorMacros = macros;
}

ConfiguredPreprocessorMacros getConfiguredPreprocessorMacros() {
  return getConfiguredPreprocessorMacrosSnapshot();
}

std::string getConfiguredPreprocessorMacrosFingerprint() {
  return fingerprintConfiguredPreprocessorMacros(
      getConfiguredPreprocessorMacrosSnapshot());
}

bool lookupActivePreprocessorMacroReplacement(
    const PreprocessorView &view, int line, const std::string &name,
    PreprocessorMacroReplacement &replacementOut) {
  replacementOut = PreprocessorMacroReplacement{};
  if (name.empty())
    return false;

  bool defined = false;
  auto initial = view.initialMacroReplacements.find(name);
  if (initial != view.initialMacroReplacements.end()) {
    replacementOut = initial->second;
    defined = true;
  }

  for (const auto &event : view.macroEvents) {
    if (event.line >= line)
      continue;
    if (event.name != name)
      continue;
    if (event.undefined) {
      replacementOut = PreprocessorMacroReplacement{};
      defined = false;
      continue;
    }
    replacementOut = event.replacement;
    defined = true;
  }

  return defined;
}

bool evaluateActivePreprocessorMacroInteger(const PreprocessorView &view,
                                            int line,
                                            const std::string &name,
                                            int &valueOut) {
  valueOut = 0;
  if (name.empty())
    return false;

  std::unordered_map<std::string, PreprocMacro> macros;
  macros.reserve(view.initialMacroReplacements.size() + view.macroEvents.size());
  for (const auto &entry : view.initialMacroReplacements) {
    macros[entry.first] = makeReplacementPreprocMacro(entry.second.replacement);
    macros[entry.first].functionLike = entry.second.functionLike;
  }
  for (const auto &event : view.macroEvents) {
    if (event.line >= line)
      continue;
    if (event.undefined) {
      macros.erase(event.name);
      continue;
    }
    PreprocMacro macro = makeReplacementPreprocMacro(event.replacement.replacement);
    macro.functionLike = event.replacement.functionLike;
    macros[event.name] = std::move(macro);
  }
  auto targetIt = macros.find(name);
  if (targetIt == macros.end() || targetIt->second.functionLike)
    return false;

  std::vector<LexToken> tokens;
  LexToken token;
  token.kind = LexToken::Kind::Identifier;
  token.text = name;
  token.start = 0;
  token.end = name.size();
  tokens.push_back(std::move(token));
  std::vector<PreprocessorConditionDiagnostic> diagnostics;
  valueOut = evalPreprocessorExpr(
      tokens, 0, macros, line, diagnostics,
      [](const LexToken &, const PreprocMacro &) {}, false, 0, 0);
  return true;
}

PreprocessorView
buildPreprocessorView(const ConditionalAst &ast,
                      const std::unordered_map<std::string, int> &defines) {
  PreprocessorInterpreterState state{ast};
  const InitialMacroSeedStats seedStats =
      seedInitialPreprocessorMacros(state.macros, defines, {}, {}, {});
  initializeLineStateStorage(state);
  snapshotInitialMacroState(state);
  state.result.macroHealth.initialArtDefaultZeroMacroCount =
      seedStats.artDefaultZero;
  state.result.macroHealth.initialCompilerPrivateConstantCount =
      seedStats.compilerPrivateConstants;
  state.result.macroHealth.initialCompilerMacroSnapshotCount =
      seedStats.compilerMacroSnapshot;
  state.result.macroHealth.initialConfiguredMacroCount = seedStats.configured;
  state.result.macroHealth.initialNumericDefineCount = seedStats.numericDefines;

  interpretNodeList(state, ast.rootNodeIndices);
  refreshMacroHealthMetrics(state.result);
  return state.result;
}

PreprocessorView
buildPreprocessorView(const std::string &text,
                      const std::unordered_map<std::string, int> &defines) {
  return buildPreprocessorView(buildConditionalAst(text), defines);
}

static PreprocessorView interpretPreprocessorViewWithIncludeContext(
    const ConditionalAst &ast, const std::unordered_map<std::string, int> &defines,
    const PreprocessorIncludeContext &includeContext,
    const std::vector<CompilerPrivateMacroConstant> &compilerPrivateConstants,
    const std::vector<CompilerMacroSnapshotMacro> &compilerMacroSnapshotMacros,
    std::unordered_map<std::string, ConditionalAst> &includeAstCache,
    IncludedDocumentCapture *includedCapture = nullptr) {
  PreprocessorInterpreterState state{ast};
  const InitialMacroSeedStats seedStats =
      seedInitialPreprocessorMacros(state.macros, defines,
                                    includeContext.artDefaultZeroMacros,
                                    compilerPrivateConstants,
                                    compilerMacroSnapshotMacros);
  initializeLineStateStorage(state);
  snapshotInitialMacroState(state);
  state.result.macroHealth.initialArtDefaultZeroMacroCount =
      seedStats.artDefaultZero;
  state.result.macroHealth.initialCompilerPrivateConstantCount =
      seedStats.compilerPrivateConstants;
  state.result.macroHealth.initialCompilerMacroSnapshotCount =
      seedStats.compilerMacroSnapshot;
  state.result.macroHealth.initialConfiguredMacroCount = seedStats.configured;
  state.result.macroHealth.initialNumericDefineCount = seedStats.numericDefines;

  std::unordered_set<std::string> includeExpansionStack;
  state.includeContext = &includeContext;
  state.currentUri = includeContext.currentUri;
  state.includeDepth = 0;
  state.includeAstCache = &includeAstCache;
  state.includeExpansionStack = &includeExpansionStack;
  state.includedCapture = includedCapture;
  if (!state.currentUri.empty() && state.includeExpansionStack) {
    state.includeExpansionStack->insert(state.currentUri);
  }
  interpretNodeList(state, ast.rootNodeIndices);
  if (!state.currentUri.empty() && state.includeExpansionStack) {
    state.includeExpansionStack->erase(state.currentUri);
  }
  refreshMacroHealthMetrics(state.result);
  return state.result;
}

static ScopedPreprocessorInput prepareScopedPreprocessorInput(
    const ConditionalAst &ast,
    const std::unordered_map<std::string, int> &defines,
    const PreprocessorIncludeContext &includeContext) {
  ScopedPreprocessorInput prepared;
  prepared.includeContext = withArtDefaultZeroMacros(
      includeContext, stripArtCompanionConstants(
                          includeContext.artDefaultZeroMacros));
  prepared.strictView = interpretPreprocessorViewWithIncludeContext(
      ast, defines, prepared.includeContext, {}, {}, prepared.includeAstCache);

  if (includeContext.currentUri.empty() ||
      includeContext.artDefaultZeroMacros.empty()) {
    return prepared;
  }

  for (int pass = 0; pass < 3; ++pass) {
    std::vector<ArtDefaultZeroMacro> scopedArtMacros =
        scopeArtCompanionConstantsToView(includeContext.artDefaultZeroMacros,
                                         includeContext.currentUri,
                                         prepared.strictView);
    if (artMacroInputsEquivalent(scopedArtMacros,
                                 prepared.includeContext.artDefaultZeroMacros)) {
      break;
    }
    prepared.includeContext =
        withArtDefaultZeroMacros(includeContext, std::move(scopedArtMacros));
    prepared.includeAstCache.clear();
    prepared.strictView = interpretPreprocessorViewWithIncludeContext(
        ast, defines, prepared.includeContext, {}, {},
        prepared.includeAstCache);
  }
  return prepared;
}

static std::vector<CompilerMacroSnapshotSource> buildCompilerSnapshotSources(
    const ConditionalAst &ast, const PreprocessorIncludeContext &includeContext,
    const PreprocessorView &strictView,
    const std::unordered_map<std::string, ConditionalAst> &includeAstCache) {
  std::vector<CompilerMacroSnapshotSource> sources;
  if (includeContext.currentUri.empty())
    return sources;
  sources.push_back(CompilerMacroSnapshotSource{includeContext.currentUri, &ast});
  for (const auto &includeUri : strictView.activeIncludeUris) {
    auto it = includeAstCache.find(includeUri);
    if (it == includeAstCache.end())
      continue;
    sources.push_back(CompilerMacroSnapshotSource{includeUri, &it->second});
  }
  return sources;
}

PreprocessorView
buildPreprocessorView(const std::string &text,
                      const std::unordered_map<std::string, int> &defines,
                      const PreprocessorIncludeContext &includeContext) {
  const ConditionalAst ast = buildConditionalAst(text);
  ScopedPreprocessorInput prepared;
  std::vector<ArtDefaultZeroMacro> cachedScopedArtMacros;
  const std::string artScopeCacheKey =
      makeArtCompanionScopeCacheKey(text, defines, includeContext);
  if (!lookupArtCompanionScopeCache(artScopeCacheKey,
                                    cachedScopedArtMacros)) {
    prepared = prepareScopedPreprocessorInput(ast, defines, includeContext);
    storeArtCompanionScopeCache(artScopeCacheKey,
                                prepared.includeContext.artDefaultZeroMacros);
  } else {
    prepared.includeContext =
        withArtDefaultZeroMacros(includeContext,
                                 std::move(cachedScopedArtMacros));
  }
  if (prepared.includeContext.currentUri.empty())
    return prepared.strictView.lineActive.empty()
               ? buildPreprocessorView(ast, defines)
               : prepared.strictView;

  const std::string snapshotCacheKey =
      makeCompilerMacroSnapshotCacheKey(text, defines, prepared.includeContext);
  const std::string privateCacheKey =
      makeCompilerPrivateConstantCacheKey(text, defines, prepared.includeContext);
  std::vector<CompilerMacroSnapshotMacro> compilerSnapshotMacros;
  std::vector<CompilerPrivateMacroConstant> compilerPrivateConstants;
  if (lookupCompilerMacroSnapshotCache(snapshotCacheKey,
                                       compilerSnapshotMacros) &&
      lookupCompilerPrivateConstantCache(privateCacheKey,
                                         compilerPrivateConstants)) {
    std::unordered_map<std::string, ConditionalAst> includeAstCache;
    return interpretPreprocessorViewWithIncludeContext(
        ast, defines, prepared.includeContext, compilerPrivateConstants,
        compilerSnapshotMacros, includeAstCache);
  }

  if (prepared.strictView.lineActive.empty()) {
    prepared.strictView = interpretPreprocessorViewWithIncludeContext(
        ast, defines, prepared.includeContext, {}, {},
        prepared.includeAstCache);
  }

  if (!lookupCompilerMacroSnapshotCache(snapshotCacheKey,
                                        compilerSnapshotMacros)) {
    const CompilerMacroSnapshot compilerSnapshot =
        buildCompilerMacroSnapshotFromSources(buildCompilerSnapshotSources(
            ast, prepared.includeContext, prepared.strictView,
            prepared.includeAstCache));
    compilerSnapshotMacros = compilerSnapshot.macros;
    storeCompilerMacroSnapshotCache(snapshotCacheKey, compilerSnapshotMacros);
  }

  if (!lookupCompilerPrivateConstantCache(privateCacheKey,
                                          compilerPrivateConstants)) {
    compilerPrivateConstants =
        collectCompilerPrivateConstantsFromActiveClosure(
            ast, prepared.includeContext, prepared.strictView,
            prepared.includeAstCache);
    storeCompilerPrivateConstantCache(privateCacheKey,
                                      compilerPrivateConstants);
  }
  if (compilerPrivateConstants.empty() && compilerSnapshotMacros.empty())
    return prepared.strictView;

  return interpretPreprocessorViewWithIncludeContext(
      ast, defines, prepared.includeContext, compilerPrivateConstants,
      compilerSnapshotMacros, prepared.includeAstCache);
}

bool buildIncludedDocumentPreprocessorView(
    const std::string &rootText,
    const std::unordered_map<std::string, int> &defines,
    const PreprocessorIncludeContext &includeContext,
    const std::string &targetUri, PreprocessorView &resultOut) {
  resultOut = PreprocessorView{};
  if (rootText.empty() || includeContext.currentUri.empty() || targetUri.empty())
    return false;

  const ConditionalAst ast = buildConditionalAst(rootText);
  ScopedPreprocessorInput prepared;
  std::vector<ArtDefaultZeroMacro> cachedScopedArtMacros;
  const std::string artScopeCacheKey =
      makeArtCompanionScopeCacheKey(rootText, defines, includeContext);
  if (!lookupArtCompanionScopeCache(artScopeCacheKey,
                                    cachedScopedArtMacros)) {
    prepared = prepareScopedPreprocessorInput(ast, defines, includeContext);
    storeArtCompanionScopeCache(artScopeCacheKey,
                                prepared.includeContext.artDefaultZeroMacros);
  } else {
    prepared.includeContext =
        withArtDefaultZeroMacros(includeContext,
                                 std::move(cachedScopedArtMacros));
  }
  const std::string cachedSnapshotKey =
      makeCompilerMacroSnapshotCacheKey(rootText, defines,
                                        prepared.includeContext);
  const std::string cachedPrivateKey =
      makeCompilerPrivateConstantCacheKey(rootText, defines,
                                          prepared.includeContext);
  std::vector<CompilerMacroSnapshotMacro> cachedSnapshotMacros;
  std::vector<CompilerPrivateMacroConstant> cachedPrivateConstants;
  if (lookupCompilerMacroSnapshotCache(cachedSnapshotKey,
                                       cachedSnapshotMacros) &&
      lookupCompilerPrivateConstantCache(cachedPrivateKey,
                                         cachedPrivateConstants)) {
    std::unordered_map<std::string, ConditionalAst> includeAstCache;
    IncludedDocumentCapture capture;
    capture.targetUri = targetUri;
    interpretPreprocessorViewWithIncludeContext(
        ast, defines, prepared.includeContext, cachedPrivateConstants,
        cachedSnapshotMacros, includeAstCache, &capture);
    if (!capture.found)
      return false;
    refreshMacroHealthMetrics(capture.view);
    resultOut = std::move(capture.view);
    return true;
  }

  if (prepared.strictView.lineActive.empty()) {
    prepared.strictView = interpretPreprocessorViewWithIncludeContext(
        ast, defines, prepared.includeContext, {}, {},
        prepared.includeAstCache);
  }

  const std::string snapshotCacheKey =
      makeCompilerMacroSnapshotCacheKey(rootText, defines,
                                        prepared.includeContext);
  std::vector<CompilerMacroSnapshotMacro> compilerSnapshotMacros;
  if (!lookupCompilerMacroSnapshotCache(snapshotCacheKey,
                                        compilerSnapshotMacros)) {
    const CompilerMacroSnapshot compilerSnapshot =
        buildCompilerMacroSnapshotFromSources(buildCompilerSnapshotSources(
            ast, prepared.includeContext, prepared.strictView,
            prepared.includeAstCache));
    compilerSnapshotMacros = compilerSnapshot.macros;
    storeCompilerMacroSnapshotCache(snapshotCacheKey, compilerSnapshotMacros);
  }

  std::vector<CompilerPrivateMacroConstant> compilerPrivateConstants =
      {};
  const std::string cacheKey =
      makeCompilerPrivateConstantCacheKey(rootText, defines,
                                          prepared.includeContext);
  if (!lookupCompilerPrivateConstantCache(cacheKey, compilerPrivateConstants)) {
    compilerPrivateConstants = collectCompilerPrivateConstantsFromActiveClosure(
        ast, prepared.includeContext, prepared.strictView,
        prepared.includeAstCache);
    storeCompilerPrivateConstantCache(cacheKey, compilerPrivateConstants);
  }

  IncludedDocumentCapture capture;
  capture.targetUri = targetUri;
  interpretPreprocessorViewWithIncludeContext(
      ast, defines, prepared.includeContext, compilerPrivateConstants,
      compilerSnapshotMacros, prepared.includeAstCache, &capture);
  if (!capture.found)
    return false;
  refreshMacroHealthMetrics(capture.view);
  resultOut = std::move(capture.view);
  return true;
}
