#include "server_request_handler_hover.hpp"

#include "active_unit.hpp"
#include "call_query.hpp"
#include "callsite_parser.hpp"
#include "declaration_query.hpp"
#include "expanded_source.hpp"
#include "hlsl_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_docs.hpp"
#include "hover_markdown.hpp"
#include "hover_rendering.hpp"
#include "include_resolver.hpp"
#include "interactive_semantic_runtime.hpp"
#include "language_registry.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "macro_generated_functions.hpp"
#include "member_query.hpp"
#include "nsf_lexer.hpp"
#include "semantic_snapshot.hpp"
#include "server_parse.hpp"
#include "server_request_handler_common.hpp"
#include "symbol_query.hpp"
#include "text_utils.hpp"
#include "type_desc.hpp"
#include "type_eval.hpp"
#include "type_model.hpp"
#include "uri_utils.hpp"
#include "workspace_index.hpp"
#include "workspace_summary_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <sys/stat.h>
#endif

bool findDeclaredIdentifierInDeclarationLine(const std::string &line,
                                             const std::string &word,
                                             size_t &posOut);

static bool queryFunctionSignatureWithSemanticFallback(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::string &name, int lineIndex, int nameCharacter,
    ServerRequestContext &ctx, std::string &labelOut,
    std::vector<std::string> &parametersOut);
static bool findLocalFunctionDeclarationUpTo(const std::string &text,
                                             const std::string &word,
                                             size_t maxOffset, int &lineOut,
                                             int &nameCharOut);
static void appendStructFieldMarkdown(
    std::string &markdown,
    const std::vector<SemanticSnapshotStructFieldInfo> &fields);
static std::string extractReturnTypeFromFunctionLabel(const std::string &label,
                                                      const std::string &name);
static bool buildMacroHoverInputFromDocumentText(
    const std::string &currentUri, const std::string &definitionUri,
    const std::string &definitionText, int definitionLine,
    const std::string &expectedName, HoverMacroMarkdownInput &macroInput);

struct IncludeContextDefinitionGroup {
  DefinitionLocation location;
  std::vector<std::string> unitLabels;
};

struct IncludeContextResolutionSummary {
  std::vector<std::string> candidateUnitPaths;
  std::vector<IncludeContextDefinitionGroup> definitionGroups;
  size_t resolvedUnitCount = 0;
  size_t unresolvedUnitCount = 0;

  bool hasCandidateContext() const { return candidateUnitPaths.size() > 1; }
  bool hasAmbiguousDefinitions() const { return definitionGroups.size() > 1; }
  bool hasPartialResolution() const {
    return definitionGroups.size() == 1 && unresolvedUnitCount > 0;
  }
  bool hasNoResolvedDefinitions() const {
    return definitionGroups.empty() && unresolvedUnitCount > 0;
  }
  bool hasFullyResolvedSingleDefinition() const {
    return hasCandidateContext() && definitionGroups.size() == 1 &&
           unresolvedUnitCount == 0 &&
           resolvedUnitCount == candidateUnitPaths.size();
  }
};

static void buildIncludeContextResolutionSummary(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    IncludeContextResolutionSummary &outSummary);
static void appendIncludeContextDefinitionListItems(
    const std::string &uri, const std::string &word,
    const IncludeContextResolutionSummary &summary, ServerRequestContext &ctx,
    std::vector<HoverLocationListItem> &outItems);

static std::string formatFileLineDisplay(const std::string &uri, int line,
                                         const std::string &currentUri) {
  int oneBased = line >= 0 ? (line + 1) : 0;
  std::string path = uriToPath(uri);
  if (path.empty())
    path = uri;
  std::string base = std::filesystem::path(path).filename().string();
  if (base.empty())
    base = uri == currentUri ? "current-file" : path;
  std::string label =
      base + ":" + std::to_string(oneBased > 0 ? oneBased : 0);
  std::string target = uri;
  if (target.empty())
    return label;
  if (oneBased > 0) {
    target += "#L";
    target += std::to_string(oneBased);
  }
  return "[" + label + "](" + target + ")";
}

static bool buildMacroHoverInputFromDocumentText(
    const std::string &currentUri, const std::string &definitionUri,
    const std::string &definitionText, int definitionLine,
    const std::string &expectedName, HoverMacroMarkdownInput &macroInput) {
  macroInput = HoverMacroMarkdownInput{};
  if (definitionLine < 0)
    return false;

  const std::string macroLine = getLineAt(definitionText, definitionLine);
  ParsedMacroDefinitionInfo macroInfo;
  if (!extractMacroDefinitionInLineShared(macroLine, macroInfo) ||
      macroInfo.name.empty() || macroInfo.name != expectedName) {
    return false;
  }

  std::string macroBlock = macroLine;
  int nextLine = definitionLine + 1;
  std::string currentLine = macroLine;
  while (true) {
    const std::string trimmed = trimRightCopy(currentLine);
    if (trimmed.empty() || trimmed.back() != '\\')
      break;
    currentLine = getLineAt(definitionText, nextLine);
    if (currentLine.empty())
      break;
    macroBlock += "\n";
    macroBlock += currentLine;
    nextLine++;
    if (nextLine - definitionLine > 32)
      break;
  }

  macroInput.code = macroBlock;
  macroInput.kindLabel =
      macroInfo.isFunctionLike ? "(Function-like macro)" : "(Macro)";
  macroInput.definedAt =
      formatFileLineDisplay(definitionUri, definitionLine, currentUri);
  macroInput.leadingDoc =
      extractLeadingDocumentationAtLine(definitionText, definitionLine);
  const int nameEndChar =
      byteOffsetInLineToUtf16(macroLine, static_cast<int>(macroInfo.nameEnd));
  macroInput.inlineDoc = extractTrailingInlineCommentAtLine(
      definitionText, definitionLine, nameEndChar);
  return true;
}

static bool writeMacroHoverResponse(
    const Json &id, const std::string &currentUri,
    const std::string &definitionUri, int definitionLine,
    const std::string &expectedName, ServerRequestContext &ctx,
    const std::vector<IndexedDefinition> *macroDefs = nullptr) {
  std::string defText;
  HoverMacroMarkdownInput macroInput;
  if (!ctx.readDocumentText(definitionUri, defText)) {
    macroInput.code = "#define " + expectedName;
    macroInput.kindLabel = "(Macro)";
    macroInput.definedAt =
        formatFileLineDisplay(definitionUri, definitionLine, currentUri);
  } else if (!buildMacroHoverInputFromDocumentText(
                 currentUri, definitionUri, defText, definitionLine,
                 expectedName, macroInput)) {
    return false;
  }

  if (macroDefs && macroDefs->size() > 1) {
    macroInput.listTitle = "Definitions";
    size_t shown = 0;
    for (size_t i = 0; i < macroDefs->size() && shown < 50; i++) {
      HoverLocationListItem item;
      item.label = "#define " + (*macroDefs)[i].name;
      item.locationDisplay =
          formatFileLineDisplay((*macroDefs)[i].uri, (*macroDefs)[i].line,
                                currentUri);
      macroInput.listItems.push_back(std::move(item));
      shown++;
    }
    macroInput.appendEllipsisAfterList = macroDefs->size() > shown;
  }

  Json hover = makeObject();
  hover.o["contents"] = makeMarkup(renderHoverMacroMarkdown(macroInput));
  writeResponse(id, hover);
  return true;
}

bool pathHasNsfExtension(const std::string &path) {
  std::filesystem::path fsPath(path);
  std::string ext = fsPath.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return ext == ".nsf";
}

static std::string normalizeComparablePath(std::string value) {
  value = std::filesystem::path(value).lexically_normal().string();
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   char c = static_cast<char>(std::tolower(ch));
                   return c == '\\' ? '/' : c;
                 });
  while (value.size() > 1 && value.back() == '/')
    value.pop_back();
  return value;
}

static bool sameDocumentIdentity(const std::string &lhsUriOrPath,
                                 const std::string &rhsUriOrPath) {
  if (lhsUriOrPath == rhsUriOrPath)
    return true;
  std::string lhsPath = uriToPath(lhsUriOrPath);
  if (lhsPath.empty())
    lhsPath = lhsUriOrPath;
  std::string rhsPath = uriToPath(rhsUriOrPath);
  if (rhsPath.empty())
    rhsPath = rhsUriOrPath;
  if (lhsPath.empty() || rhsPath.empty())
    return false;
  return normalizeComparablePath(lhsPath) == normalizeComparablePath(rhsPath);
}

static std::string documentIdentityKey(const std::string &uriOrPath) {
  std::string path = uriToPath(uriOrPath);
  if (path.empty())
    path = uriOrPath;
  if (path.empty())
    return std::string();
  return normalizeComparablePath(path);
}

static bool extractIncludePathOutsideCommentsLocal(
    const std::string &line, bool &inBlockCommentInOut,
    std::string &outIncludePath) {
  outIncludePath.clear();
  bool inString = false;
  bool escape = false;
  for (size_t i = 0; i < line.size();) {
    const char ch = line[i];
    const char next = (i + 1 < line.size()) ? line[i + 1] : '\0';

    if (inBlockCommentInOut) {
      if (ch == '*' && next == '/') {
        inBlockCommentInOut = false;
        i += 2;
        continue;
      }
      i++;
      continue;
    }

    if (!inString && ch == '/' && next == '*') {
      inBlockCommentInOut = true;
      i += 2;
      continue;
    }
    if (!inString && ch == '/' && next == '/')
      break;

    if (ch == '"' && !escape) {
      inString = !inString;
      i++;
      continue;
    }
    if (inString) {
      escape = (!escape && ch == '\\');
      i++;
      continue;
    }
    escape = false;

    if (ch != '#') {
      i++;
      continue;
    }

    size_t j = i + 1;
    while (j < line.size() &&
           std::isspace(static_cast<unsigned char>(line[j]))) {
      j++;
    }
    if (j + 7 > line.size() || line.compare(j, 7, "include") != 0) {
      i++;
      continue;
    }
    j += 7;
    while (j < line.size() &&
           std::isspace(static_cast<unsigned char>(line[j]))) {
      j++;
    }
    if (j >= line.size())
      return false;

    const char opener = line[j];
    const char closer = opener == '<' ? '>' : (opener == '"' ? '"' : '\0');
    if (closer == '\0')
      return false;
    j++;
    const size_t start = j;
    while (j < line.size() && line[j] != closer)
      j++;
    if (j >= line.size())
      return false;
    outIncludePath = line.substr(start, j - start);
    return !outIncludePath.empty();
  }
  return false;
}

std::vector<std::string>
collectIncludeContextUnitPaths(const std::string &uri,
                               const ServerRequestContext &ctx) {
  std::vector<std::string> unitPaths;
  if (!getActiveUnitPath().empty())
    return unitPaths;

  const std::string path = uriToPath(uri);
  if (path.empty() || pathHasNsfExtension(path))
    return unitPaths;

  workspaceSummaryRuntimeCollectIncludingUnits({uri}, unitPaths, 256);
  std::sort(unitPaths.begin(), unitPaths.end());
  unitPaths.erase(std::unique(unitPaths.begin(), unitPaths.end()),
                  unitPaths.end());
  return unitPaths;
}

static std::string formatIncludeContextUnitLabel(
    const std::string &candidateUnitPath) {
  std::filesystem::path fsPath(candidateUnitPath);
  std::string label = fsPath.filename().string();
  if (label.empty())
    label = candidateUnitPath;
  return label;
}

static std::string buildIncludeContextHoverNote(
    const IncludeContextResolutionSummary &summary) {
  if (!summary.hasCandidateContext())
    return std::string();
  if (summary.hasAmbiguousDefinitions()) {
    std::string note = "(Include context ambiguous) ";
    note += std::to_string(summary.definitionGroups.size());
    note += " candidate definitions across ";
    note += std::to_string(summary.candidateUnitPaths.size());
    note += " candidate units.";
    if (summary.unresolvedUnitCount > 0) {
      note += " ";
      note += std::to_string(summary.unresolvedUnitCount);
      note += " candidate units had no indexed definition.";
    }
    return note;
  }
  if (summary.hasPartialResolution()) {
    std::string note = "(Include context partially resolved) One shared "
                       "definition matched across ";
    note += std::to_string(summary.resolvedUnitCount);
    note += " candidate units; ";
    note += std::to_string(summary.unresolvedUnitCount);
    note += " candidate units had no indexed definition.";
    return note;
  }
  if (summary.hasNoResolvedDefinitions()) {
    std::string note = "(Include context unresolved) No candidate unit "
                       "produced an indexed definition";
    if (!summary.candidateUnitPaths.empty()) {
      note += " across ";
      note += std::to_string(summary.candidateUnitPaths.size());
      note += " candidate units";
    }
    note += ".";
    return note;
  }
  return std::string();
}

static std::string formatIncludeContextUnitSummary(
    const std::vector<std::string> &unitLabels) {
  if (unitLabels.empty())
    return std::string();
  std::string summary;
  const size_t shown = std::min<size_t>(unitLabels.size(), 3);
  for (size_t i = 0; i < shown; i++) {
    if (i > 0)
      summary += ", ";
    summary += unitLabels[i];
  }
  if (unitLabels.size() > shown) {
    summary += ", +";
    summary += std::to_string(unitLabels.size() - shown);
    summary += " more";
  }
  return summary;
}

static bool shouldAttachIncludeContextHoverSummary(
    const IncludeContextResolutionSummary &summary,
    const std::string &currentUri, const std::string &definitionUri) {
  if (!summary.hasCandidateContext() || definitionUri.empty())
    return false;
  if (sameDocumentIdentity(currentUri, definitionUri))
    return false;
  return summary.hasAmbiguousDefinitions() || summary.unresolvedUnitCount > 0;
}

static void appendIncludeContextFunctionSummaryIfNeeded(
    const std::string &currentUri, const std::string &definitionUri,
    const std::string &word, const IncludeContextResolutionSummary &summary,
    ServerRequestContext &ctx, HoverFunctionMarkdownInput &functionInput) {
  if (!shouldAttachIncludeContextHoverSummary(summary, currentUri,
                                              definitionUri)) {
    return;
  }
  const std::string includeContextNote = buildIncludeContextHoverNote(summary);
  if (includeContextNote.empty())
    return;
  if (!functionInput.selectionNote.empty())
    functionInput.selectionNote += "\n\n";
  functionInput.selectionNote += includeContextNote;
  if (!summary.hasAmbiguousDefinitions())
    return;
  appendIncludeContextDefinitionListItems(currentUri, word, summary, ctx,
                                         functionInput.listItems);
  if (!functionInput.listItems.empty())
    functionInput.listTitle = "Candidate definitions";
}

static void appendIncludeContextSymbolNoteIfNeeded(
    const std::string &currentUri, const std::string &definitionUri,
    const IncludeContextResolutionSummary &summary,
    HoverSymbolMarkdownInput &symbolInput) {
  if (!shouldAttachIncludeContextHoverSummary(summary, currentUri,
                                              definitionUri)) {
    return;
  }
  const std::string includeContextNote = buildIncludeContextHoverNote(summary);
  if (includeContextNote.empty())
    return;
  symbolInput.notes.push_back(includeContextNote);
}

static bool pickIncludeContextDefinitionForUnit(
    const std::string &candidateUnitPath, const std::string &word,
    DefinitionLocation &outLocation) {
  outLocation = DefinitionLocation{};
  std::vector<std::string> includeClosurePaths;
  workspaceSummaryRuntimeCollectIncludeClosureForUnit(candidateUnitPath,
                                                      includeClosurePaths, 512);
  const std::string unitPathKey = normalizeComparablePath(candidateUnitPath);
  if (!unitPathKey.empty()) {
    includeClosurePaths.push_back(unitPathKey);
  }
  if (includeClosurePaths.empty())
    return false;

  std::unordered_set<std::string> closureSet;
  for (const auto &path : includeClosurePaths) {
    if (!path.empty())
      closureSet.insert(normalizeComparablePath(path));
  }
  if (closureSet.empty())
    return false;

  std::vector<IndexedDefinition> defs;
  if (!workspaceSummaryRuntimeFindDefinitions(word, defs, 256) || defs.empty())
    return false;

  bool found = false;
  int bestScore = std::numeric_limits<int>::max();
  IndexedDefinition bestDef;
  for (const auto &def : defs) {
    std::string defPath = uriToPath(def.uri);
    if (defPath.empty())
      continue;
    const std::string defKey = normalizeComparablePath(defPath);
    if (closureSet.find(defKey) == closureSet.end())
      continue;
    int score = defKey == unitPathKey ? 0 : 1;
    if (!found || score < bestScore ||
        (score == bestScore &&
         (def.uri < bestDef.uri ||
          (def.uri == bestDef.uri &&
           (def.line < bestDef.line ||
            (def.line == bestDef.line && def.start < bestDef.start)))))) {
      bestDef = def;
      bestScore = score;
      found = true;
    }
  }
  if (!found)
    return false;

  outLocation.uri = bestDef.uri;
  outLocation.line = bestDef.line;
  outLocation.start = bestDef.start;
  outLocation.end = bestDef.end;
  return true;
}

static void buildIncludeContextResolutionSummary(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    IncludeContextResolutionSummary &outSummary) {
  outSummary = IncludeContextResolutionSummary{};
  if (!getActiveUnitPath().empty())
    return;

  const std::string path = uriToPath(uri);
  if (path.empty() || pathHasNsfExtension(path))
    return;

  outSummary.candidateUnitPaths = collectIncludeContextUnitPaths(uri, ctx);
  if (!outSummary.hasCandidateContext())
    return;

  std::unordered_map<std::string, size_t> groupIndices;
  for (const auto &candidateUnitPath : outSummary.candidateUnitPaths) {
    DefinitionLocation location;
    if (!pickIncludeContextDefinitionForUnit(candidateUnitPath, word,
                                             location)) {
      outSummary.unresolvedUnitCount++;
      continue;
    }

    outSummary.resolvedUnitCount++;
    const std::string key =
        location.uri + "|" + std::to_string(location.line) + "|" +
        std::to_string(location.start) + "|" + std::to_string(location.end);
    auto [it, inserted] =
        groupIndices.emplace(key, outSummary.definitionGroups.size());
    if (inserted) {
      IncludeContextDefinitionGroup group;
      group.location = location;
      outSummary.definitionGroups.push_back(std::move(group));
    }
    auto &unitLabels = outSummary.definitionGroups[it->second].unitLabels;
    const std::string label = formatIncludeContextUnitLabel(candidateUnitPath);
    if (std::find(unitLabels.begin(), unitLabels.end(), label) ==
        unitLabels.end()) {
      unitLabels.push_back(label);
    }
  }
}

bool pickBestWorkspaceDefinitionForCurrentContext(
    const std::string &uri, const std::string &word,
    DefinitionLocation &outLocation) {
  outLocation = DefinitionLocation{};
  const std::string activeUnitPath = getActiveUnitPath();
  const std::string currentPath = uriToPath(uri);
  const std::string currentUnitPath =
      (!currentPath.empty() && pathHasNsfExtension(currentPath))
          ? currentPath
          : (!activeUnitPath.empty() ? activeUnitPath : std::string());
  if (!currentUnitPath.empty() &&
      pickIncludeContextDefinitionForUnit(currentUnitPath, word, outLocation)) {
    return true;
  }

  std::vector<IndexedDefinition> defs;
  if (!workspaceSummaryRuntimeFindDefinitions(word, defs, 256) || defs.empty())
    return false;

  const std::string currentPathKey =
      currentPath.empty() ? std::string() : normalizeComparablePath(currentPath);
  const std::string currentDirKey =
      currentPath.empty()
          ? std::string()
          : normalizeComparablePath(
                std::filesystem::path(currentPath).parent_path().string());

  bool found = false;
  int bestScore = std::numeric_limits<int>::max();
  IndexedDefinition bestDef;
  for (const auto &def : defs) {
    std::string defPath = uriToPath(def.uri);
    if (defPath.empty())
      continue;
    const std::string defKey = normalizeComparablePath(defPath);
    int score = 2;
    if (!currentPathKey.empty() && defKey == currentPathKey) {
      score = 0;
    } else if (!currentDirKey.empty() &&
               defKey.rfind(currentDirKey, 0) == 0 &&
               (defKey.size() == currentDirKey.size() ||
                defKey[currentDirKey.size()] == '/')) {
      score = 1;
    }
    if (!found || score < bestScore ||
        (score == bestScore &&
         (def.uri < bestDef.uri ||
          (def.uri == bestDef.uri &&
           (def.line < bestDef.line ||
            (def.line == bestDef.line && def.start < bestDef.start)))))) {
      bestDef = def;
      bestScore = score;
      found = true;
    }
  }
  if (!found)
    return false;

  outLocation.uri = bestDef.uri;
  outLocation.line = bestDef.line;
  outLocation.start = bestDef.start;
  outLocation.end = bestDef.end;
  return true;
}

bool collectIncludeContextDefinitionLocations(
    const std::string &uri, const std::string &word, ServerRequestContext &ctx,
    std::vector<DefinitionLocation> &outLocations) {
  outLocations.clear();
  IncludeContextResolutionSummary summary;
  buildIncludeContextResolutionSummary(uri, word, ctx, summary);
  if (!summary.hasAmbiguousDefinitions())
    return false;
  outLocations.reserve(summary.definitionGroups.size());
  for (const auto &group : summary.definitionGroups)
    outLocations.push_back(group.location);
  return outLocations.size() > 1;
}

static void appendIncludeContextDefinitionListItems(
    const std::string &uri, const std::string &word,
    const IncludeContextResolutionSummary &summary, ServerRequestContext &ctx,
    std::vector<HoverLocationListItem> &outItems) {
  outItems.clear();
  if (!summary.hasAmbiguousDefinitions())
    return;

  outItems.reserve(summary.definitionGroups.size());
  for (const auto &group : summary.definitionGroups) {
    HoverLocationListItem item;
    item.locationDisplay = formatFileLineDisplay(group.location.uri,
                                                 group.location.line, uri);
    std::string defText;
    if (ctx.readDocumentText(group.location.uri, defText)) {
      std::string label;
      std::vector<std::string> paramsOut;
      uint64_t dEpoch = 0;
      const Document *dDoc = ctx.findDocument(group.location.uri);
      if (dDoc)
        dEpoch = dDoc->epoch;
      const bool fastSig = queryFunctionSignatureWithSemanticFallback(
          group.location.uri, defText, dEpoch, word, group.location.line,
          group.location.start, ctx, label, paramsOut);
      if (fastSig && !label.empty()) {
        item.label = label;
      }
    }
    if (item.label.empty())
      item.label = word;
    const std::string unitSummary =
        formatIncludeContextUnitSummary(group.unitLabels);
    if (!unitSummary.empty()) {
      item.locationDisplay += " (units: ";
      item.locationDisplay += unitSummary;
      item.locationDisplay += ")";
    }
    outItems.push_back(std::move(item));
  }
}

static bool queryFunctionSignatureWithSemanticFallback(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::string &name, int lineIndex, int nameCharacter,
    ServerRequestContext &ctx, std::string &labelOut,
    std::vector<std::string> &parametersOut) {
  const Document *doc = ctx.findDocument(uri);
  if (doc &&
      interactiveResolveFunctionSignature(uri, *doc, name, lineIndex,
                                          nameCharacter, ctx, labelOut,
                                          parametersOut)) {
    return true;
  }
  return querySemanticSnapshotFunctionSignature(
      uri, text, epoch, ctx.workspaceFolders, ctx.includePaths,
      ctx.shaderExtensions, ctx.preprocessorDefines, name, lineIndex,
      nameCharacter, labelOut, parametersOut);
}

static void appendStructFieldMarkdown(
    std::string &markdown,
    const std::vector<SemanticSnapshotStructFieldInfo> &fields) {
  if (fields.empty())
    return;
  markdown += "\n\nMembers:";
  size_t shown = 0;
  for (size_t i = 0; i < fields.size() && shown < 256; i++) {
    markdown += "\n- `";
    if (!fields[i].type.empty()) {
      markdown += fields[i].type;
      markdown += " ";
    }
    markdown += fields[i].name;
    markdown += "`";
    shown++;
  }
  if (fields.size() > shown)
    markdown += "\n- `...`";
}

static bool writeIncludeContextHoverResponse(
    const Json &id, const std::string &uri, const std::string &word,
    const IncludeContextResolutionSummary &summary, ServerRequestContext &ctx) {
  if (!summary.hasAmbiguousDefinitions())
    return false;

  const std::string includeContextNote = buildIncludeContextHoverNote(summary);
  if (includeContextNote.empty() || summary.definitionGroups.empty()) {
    return false;
  }

  std::vector<HoverLocationListItem> listItems;
  appendIncludeContextDefinitionListItems(uri, word, summary, ctx, listItems);
  if (listItems.empty())
    return false;

  const DefinitionLocation &primary = summary.definitionGroups.front().location;
  std::string defText;
  if (!ctx.readDocumentText(primary.uri, defText))
    return false;

  std::string label;
  std::vector<std::string> paramsOut;
  uint64_t locEpoch = 0;
  const Document *locDoc = ctx.findDocument(primary.uri);
  if (locDoc)
    locEpoch = locDoc->epoch;
  const bool fastSig = queryFunctionSignatureWithSemanticFallback(
      primary.uri, defText, locEpoch, word, primary.line, primary.start, ctx,
      label, paramsOut);
  if (!fastSig || label.empty()) {
    return false;
  }

  HoverFunctionMarkdownInput functionInput;
  functionInput.code = label;
  functionInput.kindLabel = "(HLSL function)";
  functionInput.returnType = extractReturnTypeFromFunctionLabel(label, word);
  functionInput.parameters = paramsOut;
  functionInput.definedAt = formatFileLineDisplay(primary.uri, primary.line, uri);
  functionInput.leadingDoc = extractLeadingDocumentationAtLine(defText, primary.line);
  functionInput.inlineDoc =
      extractTrailingInlineCommentAtLine(defText, primary.line, primary.end);
  functionInput.selectionNote = includeContextNote;
  functionInput.listTitle = "Candidate definitions";
  functionInput.listItems = std::move(listItems);

  Json hover = makeObject();
  hover.o["contents"] = makeMarkup(renderHoverFunctionMarkdown(functionInput));
  writeResponse(id, hover);
  return true;
}

static bool writeCurrentDocParameterHoverResponse(
    const Json &id, const std::string &uri, const Document &doc,
    const std::string &word, size_t cursorOffset, ServerRequestContext &ctx) {
  bool isParam = false;
  TypeEvalResult typeEval = resolveHoverTypeAtDeclaration(
      uri, doc, word, cursorOffset, ctx, isParam);
  if (!isParam)
    return false;

  HoverSymbolMarkdownInput symbolInput;
  symbolInput.code =
      (typeEval.type.empty() ? std::string() : (typeEval.type + " ")) + word;
  symbolInput.notes.push_back("(Parameter)");
  symbolInput.typeName = typeEval.type;
  symbolInput.indeterminateReason = typeEval.reasonCode;

  DefinitionLocation definition;
  if (interactiveResolveDefinitionLocation(uri, doc, word, cursorOffset, ctx,
                                           definition) &&
      definition.uri == uri) {
    symbolInput.definedAt =
        formatFileLineDisplay(definition.uri, definition.line, uri);
    symbolInput.leadingDoc =
        extractLeadingDocumentationAtLine(doc.text, definition.line);
    symbolInput.inlineDoc = extractTrailingInlineCommentAtLine(
        doc.text, definition.line, definition.end);
  }

  Json hover = makeObject();
  hover.o["contents"] = makeMarkup(renderHoverSymbolMarkdown(symbolInput));
  writeResponse(id, hover);
  return true;
}

static bool tryExtractIncludeSpan(const std::string &lineText, int cursorChar,
                                  std::string &includePathOut) {
  includePathOut.clear();
  if (cursorChar < 0)
    return false;
  size_t includePos = lineText.find("#include");
  if (includePos == std::string::npos)
    return false;

  size_t start = lineText.find('"', includePos);
  size_t end = start != std::string::npos ? lineText.find('"', start + 1)
                                          : std::string::npos;
  if (start != std::string::npos && end != std::string::npos && end > start) {
    int startChar = byteOffsetInLineToUtf16(lineText, static_cast<int>(start));
    int endChar = byteOffsetInLineToUtf16(lineText, static_cast<int>(end));
    if (cursorChar >= startChar && cursorChar <= endChar) {
      includePathOut = lineText.substr(start + 1, end - start - 1);
      return !includePathOut.empty();
    }
    return false;
  }

  start = lineText.find('<', includePos);
  end = start != std::string::npos ? lineText.find('>', start + 1)
                                   : std::string::npos;
  if (start != std::string::npos && end != std::string::npos && end > start) {
    int startChar = byteOffsetInLineToUtf16(lineText, static_cast<int>(start));
    int endChar = byteOffsetInLineToUtf16(lineText, static_cast<int>(end));
    if (cursorChar >= startChar && cursorChar <= endChar) {
      includePathOut = lineText.substr(start + 1, end - start - 1);
      return !includePathOut.empty();
    }
    return false;
  }
  return false;
}

static bool isSwizzleToken(const std::string &text) {
  if (text.empty() || text.size() > 4)
    return false;
  for (char ch : text) {
    if (ch != 'x' && ch != 'y' && ch != 'z' && ch != 'w' && ch != 'r' &&
        ch != 'g' && ch != 'b' && ch != 'a')
      return false;
  }
  return true;
}

static std::string trimCopy(const std::string &value) {
  return trimRightCopy(trimLeftCopy(value));
}

static std::string extractReturnTypeFromFunctionLabel(const std::string &label,
                                                      const std::string &name) {
  size_t namePos = label.rfind(name);
  if (namePos == std::string::npos)
    return "";
  size_t after = namePos + name.size();
  while (after < label.size() &&
         std::isspace(static_cast<unsigned char>(label[after]))) {
    after++;
  }
  if (after >= label.size() || label[after] != '(')
    return "";
  return trimCopy(label.substr(0, namePos));
}

static std::string swizzleResultType(const std::string &baseType,
                                     size_t swizzleLen) {
  std::string scalar = "float";
  if (baseType.rfind("half", 0) == 0)
    scalar = "half";
  else if (baseType.rfind("double", 0) == 0)
    scalar = "double";
  if (swizzleLen <= 1)
    return scalar;
  return scalar + std::to_string(swizzleLen);
}

static bool findLocalFunctionDeclarationUpTo(const std::string &text,
                                             const std::string &word,
                                             size_t maxOffset, int &lineOut,
                                             int &nameCharOut) {
  lineOut = -1;
  nameCharOut = 0;
  if (word.empty())
    return false;
  size_t lineStartOffset = 0;
  std::istringstream stream(text);
  std::string line;
  int lineIndex = 0;
  bool found = false;
  while (std::getline(stream, line)) {
    if (lineStartOffset >= maxOffset)
      break;
    auto tokens = lexLineTokens(line);
    if (!tokens.empty() && tokens[0].kind == LexToken::Kind::Identifier) {
      const std::string &first = tokens[0].text;
      if (first == "return" || first == "if" || first == "for" ||
          first == "while" || first == "switch") {
        lineStartOffset += line.size() + 1;
        lineIndex++;
        continue;
      }
    }
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
      const auto &tok = tokens[i];
      if (tok.kind != LexToken::Kind::Identifier || tok.text != word)
        continue;
      if (tokens[i + 1].kind != LexToken::Kind::Punct ||
          tokens[i + 1].text != "(")
        continue;
      if (i > 0 && tokens[i - 1].kind == LexToken::Kind::Punct &&
          (tokens[i - 1].text == "." || tokens[i - 1].text == "->" ||
           tokens[i - 1].text == "::"))
        continue;
      bool hasTypePrefix = false;
      bool hasAssignBefore = false;
      for (size_t j = 0; j < i; j++) {
        if (tokens[j].kind == LexToken::Kind::Punct &&
            (tokens[j].text == "=" || tokens[j].text == ":")) {
          hasAssignBefore = true;
          break;
        }
        if (tokens[j].kind == LexToken::Kind::Identifier &&
            !isQualifierToken(tokens[j].text)) {
          hasTypePrefix = true;
        }
      }
      if (!hasTypePrefix || hasAssignBefore)
        continue;
      lineOut = lineIndex;
      nameCharOut = byteOffsetInLineToUtf16(line, static_cast<int>(tok.start));
      found = true;
    }
    lineStartOffset += line.size() + 1;
    lineIndex++;
  }
  return found;
}

static void collectFieldInfosFromTextRecursive(
    const std::string &baseUri, const std::string &text,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines, int depth,
    std::unordered_set<std::string> &visitedUris,
    std::unordered_set<std::string> &seenNames,
    std::vector<SemanticSnapshotStructFieldInfo> &outFields) {
  if (depth <= 0 || outFields.size() >= 512)
    return;
  if (!visitedUris.insert(baseUri).second)
    return;

  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  const HlslAstDocument ast = buildHlslAstDocument(expandedSource);
  std::vector<char> globalConsumed(ast.globalVariables.size(), 0);

  for (const auto &decl : ast.topLevelDecls) {
    if (decl.kind == HlslTopLevelDeclKind::Include) {
      const std::string includePath = decl.name;
      if (!includePath.empty()) {
        auto candidates =
            resolveIncludeCandidates(baseUri, includePath, workspaceFolders,
                                     includePaths, shaderExtensions);
        std::vector<std::string> candidateUris;
        candidateUris.reserve(candidates.size());
        for (const auto &candidate : candidates)
          candidateUris.push_back(pathToUri(candidate));
        prefetchDocumentTexts(candidateUris, documents);
        for (const auto &candidate : candidates) {
          std::string candidateUri = pathToUri(candidate);
          std::string candidateText;
          if (!loadDocumentText(candidateUri, documents, candidateText))
            continue;
          collectFieldInfosFromTextRecursive(
              candidateUri, candidateText, documents, workspaceFolders,
              includePaths, shaderExtensions, defines, depth - 1, visitedUris,
              seenNames, outFields);
          break;
        }
      }
    } else if (decl.kind == HlslTopLevelDeclKind::GlobalVariable) {
      const HlslAstGlobalVariableDecl *matchedGlobal = nullptr;
      size_t matchedIndex = 0;
      for (size_t index = 0; index < ast.globalVariables.size(); index++) {
        if (globalConsumed[index])
          continue;
        const auto &candidate = ast.globalVariables[index];
        if (candidate.line != decl.line || candidate.name != decl.name)
          continue;
        matchedGlobal = &candidate;
        matchedIndex = index;
        break;
      }
      if (!matchedGlobal)
        continue;
      globalConsumed[matchedIndex] = 1;
      if (!seenNames.insert(matchedGlobal->name).second)
        continue;
      SemanticSnapshotStructFieldInfo item;
      item.name = matchedGlobal->name;
      item.type = matchedGlobal->type;
      item.line = matchedGlobal->line;
      outFields.push_back(std::move(item));
      if (outFields.size() >= 512)
        return;
    }
    if (outFields.size() >= 512)
      return;
  }
}

static bool collectStructFieldInfosFromTextWithInlineIncludes(
    const std::string &baseUri, const std::string &text,
    const std::string &structName,
    const std::unordered_map<std::string, Document> &documents,
    const std::vector<std::string> &workspaceFolders,
    const std::vector<std::string> &includePaths,
    const std::vector<std::string> &shaderExtensions,
    const std::unordered_map<std::string, int> &defines,
    std::vector<SemanticSnapshotStructFieldInfo> &fieldsOut) {
  fieldsOut.clear();
  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  const HlslAstDocument ast = buildHlslAstDocument(expandedSource);

  const HlslAstStructDecl *targetStruct = nullptr;
  for (const auto &decl : ast.structs) {
    if (decl.name == structName) {
      targetStruct = &decl;
      break;
    }
  }

  std::unordered_set<std::string> seen;
  if (targetStruct) {
    for (const auto &field : targetStruct->fields) {
      if (!seen.insert(field.name).second)
        continue;
      SemanticSnapshotStructFieldInfo item;
      item.name = field.name;
      item.type = field.type;
      item.line = field.line;
      fieldsOut.push_back(std::move(item));
      if (fieldsOut.size() >= 512)
        return true;
    }
  }

  std::vector<std::string> inlineIncludePaths;
  if (targetStruct) {
    inlineIncludePaths.reserve(targetStruct->inlineIncludes.size());
    for (const auto &inlineInclude : targetStruct->inlineIncludes) {
      if (!inlineInclude.path.empty())
        inlineIncludePaths.push_back(inlineInclude.path);
    }
  }

  std::unordered_set<std::string> seenIncludePaths;
  for (const auto &inlineIncludePath : inlineIncludePaths) {
    if (inlineIncludePath.empty() ||
        !seenIncludePaths.insert(inlineIncludePath).second) {
      continue;
    }
    auto candidates =
        resolveIncludeCandidates(baseUri, inlineIncludePath, workspaceFolders,
                                 includePaths, shaderExtensions);
    std::vector<std::string> candidateUris;
    candidateUris.reserve(candidates.size());
    for (const auto &candidate : candidates)
      candidateUris.push_back(pathToUri(candidate));
    prefetchDocumentTexts(candidateUris, documents);
    for (const auto &candidate : candidates) {
      std::string candidateUri = pathToUri(candidate);
      std::string candidateText;
      if (!loadDocumentText(candidateUri, documents, candidateText))
        continue;
      std::unordered_set<std::string> visited;
      collectFieldInfosFromTextRecursive(
          candidateUri, candidateText, documents, workspaceFolders,
          includePaths, shaderExtensions, defines, 12, visited, seen,
          fieldsOut);
      break;
    }
    if (fieldsOut.size() >= 512)
      return true;
  }
  return !fieldsOut.empty();
}

static bool structHasActiveInlineInclude(
    const std::string &text, const std::string &structName,
    const std::unordered_map<std::string, int> &defines) {
  if (structName.empty())
    return false;

  const ExpandedSource expandedSource =
      buildLinePreservingExpandedSource(text, defines);
  const HlslAstDocument ast = buildHlslAstDocument(expandedSource);
  for (const auto &decl : ast.structs) {
    if (decl.name != structName)
      continue;
    if (!decl.inlineIncludes.empty())
      return true;
  }
  return false;
}

static void appendStructFieldInfosUniqueByName(
    const std::vector<SemanticSnapshotStructFieldInfo> &source,
    std::unordered_set<std::string> &seenNames,
    std::vector<SemanticSnapshotStructFieldInfo> &dest) {
  for (const auto &field : source) {
    if (field.name.empty() || !seenNames.insert(field.name).second)
      continue;
    dest.push_back(field);
  }
}

static bool queryStructFieldInfosWithInlineIncludeFallback(
    const std::string &uri, const std::string &text, uint64_t epoch,
    const std::string &structName, bool allowInlineIncludeFallback,
    ServerRequestContext &ctx,
    std::vector<SemanticSnapshotStructFieldInfo> &fieldsOut) {
  fieldsOut.clear();
  std::unordered_set<std::string> seenNames;

  std::vector<SemanticSnapshotStructFieldInfo> snapshotFields;
  if (querySemanticSnapshotStructFieldInfos(
          uri, text, epoch, ctx.workspaceFolders, ctx.includePaths,
          ctx.shaderExtensions, ctx.preprocessorDefines, structName,
          snapshotFields) &&
      !snapshotFields.empty()) {
    appendStructFieldInfosUniqueByName(snapshotFields, seenNames, fieldsOut);
  }

  if (!allowInlineIncludeFallback) {
    return !fieldsOut.empty();
  }

  std::vector<SemanticSnapshotStructFieldInfo> inlineIncludeFields;
  if (!collectStructFieldInfosFromTextWithInlineIncludes(
          uri, text, structName, ctx.documentSnapshot(), ctx.workspaceFolders,
          ctx.includePaths, ctx.shaderExtensions, ctx.preprocessorDefines,
          inlineIncludeFields) ||
      inlineIncludeFields.empty()) {
    return !fieldsOut.empty();
  }

  appendStructFieldInfosUniqueByName(inlineIncludeFields, seenNames, fieldsOut);
  return !fieldsOut.empty();
}

static bool
findStructMemberDeclarationAtOrAfterLine(const std::string &text, int startLine,
                                         const std::string &memberName,
                                         int &lineOut, int &minCharacterOut) {
  lineOut = -1;
  minCharacterOut = 0;
  if (memberName.empty())
    return false;

  std::istringstream stream(text);
  std::string line;
  int lineIndex = 0;
  bool inBlockComment = false;
  bool inString = false;
  bool bodyStarted = false;
  int braceDepth = 0;

  while (std::getline(stream, line)) {
    if (lineIndex < startLine) {
      lineIndex++;
      continue;
    }

    if (lineIndex == startLine) {
      size_t pos = 0;
      if (findDeclaredIdentifierInDeclarationLine(line, memberName, pos)) {
        lineOut = lineIndex;
        int endByte = static_cast<int>(pos + memberName.size());
        minCharacterOut = byteOffsetInLineToUtf16(line, endByte);
        return true;
      }
    }

    if (bodyStarted && braceDepth == 1) {
      size_t pos = 0;
      if (findDeclaredIdentifierInDeclarationLine(line, memberName, pos)) {
        lineOut = lineIndex;
        int endByte = static_cast<int>(pos + memberName.size());
        minCharacterOut = byteOffsetInLineToUtf16(line, endByte);
        return true;
      }
    }

    bool inLineComment = false;
    for (size_t i = 0; i < line.size(); i++) {
      char ch = line[i];
      char next = i + 1 < line.size() ? line[i + 1] : '\0';
      if (inLineComment)
        break;
      if (inBlockComment) {
        if (ch == '*' && next == '/') {
          inBlockComment = false;
          i++;
        }
        continue;
      }
      if (inString) {
        if (ch == '"' && (i == 0 || line[i - 1] != '\\'))
          inString = false;
        continue;
      }
      if (ch == '/' && next == '/') {
        inLineComment = true;
        break;
      }
      if (ch == '/' && next == '*') {
        inBlockComment = true;
        i++;
        continue;
      }
      if (ch == '"') {
        inString = true;
        continue;
      }
      if (ch == '{') {
        if (!bodyStarted) {
          bodyStarted = true;
          braceDepth = 1;
        } else {
          braceDepth++;
        }
      } else if (ch == '}' && bodyStarted) {
        braceDepth--;
        if (braceDepth <= 0)
          return false;
      }
    }

    lineIndex++;
  }
  return false;
}

bool request_hover_handlers::handleHoverRequest(
    const std::string &method, const Json &id, const Json *params,
    ServerRequestContext &ctx, const std::vector<std::string> &keywords,
    const std::vector<std::string> &) {
  if (method != "textDocument/hover" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  if (!textDocument || !position) {
    writeResponse(id, makeNull());
    return true;
  }
  const Json *uriValue = getObjectValue(*textDocument, "uri");
  const Json *lineValue = getObjectValue(*position, "line");
  const Json *charValue = getObjectValue(*position, "character");
  if (!uriValue || !lineValue || !charValue) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string uri = getStringValue(*uriValue);
  int line = static_cast<int>(getNumberValue(*lineValue));
  int character = static_cast<int>(getNumberValue(*charValue));
  const Document *doc = ctx.findDocument(uri);
  if (!doc) {
    writeResponse(id, makeNull());
    return true;
  }
  std::string lineText = getLineAt(doc->text, line);
  size_t cursorOffset = positionToOffsetUtf16(doc->text, line, character);

  {
    std::string includePath;
    if (tryExtractIncludeSpan(lineText, character, includePath)) {
      auto candidates =
          resolveIncludeCandidates(uri, includePath, ctx.workspaceFolders,
                                   ctx.includePaths, ctx.shaderExtensions);
      std::string resolved;
      for (const auto &candidate : candidates) {
#ifdef _WIN32
        struct _stat statBuffer;
        if (_stat(candidate.c_str(), &statBuffer) == 0) {
          resolved =
              std::filesystem::path(candidate).lexically_normal().string();
          break;
        }
#endif
      }
      std::string md;
      md += formatCppCodeBlock("#include \"" + includePath + "\"");
      if (!resolved.empty()) {
        md += "\n\nResolved path: ";
        md += resolved;
      }
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  std::string word = extractWordAt(lineText, character);
  if (word.empty()) {
    writeResponse(id, makeNull());
    return true;
  }
  IncludeContextResolutionSummary includeContextSummary;
  buildIncludeContextResolutionSummary(uri, word, ctx, includeContextSummary);
  ParsedMacroDefinitionInfo currentLineMacro;
  if (extractMacroDefinitionInLineShared(lineText, currentLineMacro) &&
      currentLineMacro.name == word &&
      writeMacroHoverResponse(id, uri, uri, line, word, ctx)) {
    return true;
  }

  std::string base;
  std::string member;
  if (extractMemberAccessAtOffset(doc->text, cursorOffset, base, member)) {
    MemberAccessBaseTypeOptions baseOptions;
    baseOptions.includeWorkspaceIndexFallback = true;
    MemberAccessBaseTypeResult baseResolution =
        resolveMemberAccessBaseType(uri, *doc, base, cursorOffset, ctx,
                                    baseOptions);
    std::string baseType = baseResolution.typeName;
    if (baseType.empty())
      baseType = base;

    if (word == base) {
      HoverSymbolMarkdownInput symbolInput;
      symbolInput.code =
          (baseType.empty() ? std::string() : (baseType + " ")) + base;
      symbolInput.notes.push_back("(Member access base)");
      symbolInput.typeName = baseType;
      DeclCandidate decl;
      if (findBestDeclarationUpTo(doc->text, base, cursorOffset, decl) &&
          decl.found) {
        if (decl.braceDepth > 0) {
          symbolInput.notes.push_back("(Local variable)");
        }
        symbolInput.definedAt = formatFileLineDisplay(uri, decl.line, uri);
      }
      std::string md = renderHoverSymbolMarkdown(symbolInput);
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }

    if (isSwizzleToken(word)) {
      std::string resultType = swizzleResultType(baseType, word.size());
      std::string md;
      md += formatCppCodeBlock(resultType + " " + base + "." + word);
      md += "\n\n(Swizzle) Base: ";
      md += base;
      md += " (";
      md += baseType;
      md += ")\n\nType: ";
      md += resultType;
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }

    {
      std::string md;
      if (formatHlslBuiltinMethodMarkdown(word, baseType, md)) {
        Json hover = makeObject();
        hover.o["contents"] = makeMarkup(md);
        writeResponse(id, hover);
        return true;
      }
    }

    MemberHoverInfo memberHoverInfo;
    resolveMemberHoverInfo(uri, baseType, word, ctx, memberHoverInfo);
    std::string md;
    if (memberHoverInfo.found) {
      md += formatCppCodeBlock(memberHoverInfo.memberType + " " + word);
      md += "\n\n(Field) Owner: ";
      md += baseType;
      md += "\n\nType: ";
      md += memberHoverInfo.memberType;
      if (memberHoverInfo.hasStructLocation) {
        md += "\n\nDefined at: ";
        md += formatFileLineDisplay(memberHoverInfo.ownerStructLocation.uri,
                                    memberHoverInfo.ownerStructLocation.line,
                                    uri);
        if (!memberHoverInfo.memberLeadingDoc.empty()) {
          md += "\n\n";
          md += memberHoverInfo.memberLeadingDoc;
        }
        if (!memberHoverInfo.memberInlineDoc.empty()) {
          md += "\n\n";
          md += memberHoverInfo.memberInlineDoc;
        }
      }
    } else {
      md += formatCppCodeBlock(word);
    }
    Json hover = makeObject();
    hover.o["contents"] = makeMarkup(md);
    writeResponse(id, hover);
    return true;
  }

  {
    std::string md;
    if (formatHlslSystemSemanticMarkdown(word, md)) {
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    std::string md;
    if (formatHlslDirectiveMarkdown(word, md)) {
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    std::string md;
    if (formatHlslKeywordMarkdown(word, md)) {
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  const HlslBuiltinSignature *builtinSig = lookupHlslBuiltinSignature(word);
  if (builtinSig) {
    std::string md;
    md += formatCppCodeBlock(builtinSig->label.empty() ? word
                                                       : builtinSig->label);
    md += "\n\n(HLSL built-in function)";
    if (!builtinSig->documentation.empty()) {
      md += "\n\n";
      md += builtinSig->documentation;
    }
    Json hover = makeObject();
    hover.o["contents"] = makeMarkup(md);
    writeResponse(id, hover);
    return true;
  }

  const std::string *builtinDoc = lookupHlslBuiltinDoc(word);
  if (builtinDoc) {
    std::string md;
    md += formatCppCodeBlock("ret " + word + "(...)");
    md += "\n\n(HLSL built-in function)\n\n";
    md += *builtinDoc;
    Json hover = makeObject();
    hover.o["contents"] = makeMarkup(md);
    writeResponse(id, hover);
    return true;
  }

  if (writeIncludeContextHoverResponse(id, uri, word, includeContextSummary,
                                       ctx))
    return true;

  bool looksLikeCall = false;
  {
    std::string callName;
    CallSiteKind callKind = CallSiteKind::FunctionCall;
    if (detectCallLikeCalleeAtOffset(doc->text, cursorOffset, callName,
                                     callKind) &&
        callName == word && callKind == CallSiteKind::FunctionCall) {
      looksLikeCall = true;
    }
  }
  if (looksLikeCall) {
    int localDeclLine = -1;
    int localDeclChar = 0;
    if (findLocalFunctionDeclarationUpTo(doc->text, word, cursorOffset,
                                         localDeclLine, localDeclChar)) {
      if (writeMacroHoverResponse(id, uri, uri, localDeclLine, word, ctx)) {
        return true;
      }
      std::string label;
      std::vector<std::string> paramsOut;
      const bool fastSig = queryFunctionSignatureWithSemanticFallback(
          uri, doc->text, doc->epoch, word, localDeclLine, localDeclChar, ctx,
          label, paramsOut);
      if (fastSig && !label.empty()) {
        HoverFunctionMarkdownInput functionInput;
        functionInput.code = label;
        functionInput.kindLabel = "(HLSL function)";
        functionInput.returnType =
            extractReturnTypeFromFunctionLabel(label, word);
        functionInput.parameters = paramsOut;
        functionInput.definedAt = formatFileLineDisplay(uri, localDeclLine, uri);
        functionInput.leadingDoc =
            extractLeadingDocumentationAtLine(doc->text, localDeclLine);
        functionInput.inlineDoc =
            extractTrailingInlineCommentAtLine(doc->text, localDeclLine, 0);
        appendIncludeContextFunctionSummaryIfNeeded(
            uri, uri, word, includeContextSummary, ctx, functionInput);
        std::string md = renderHoverFunctionMarkdown(functionInput);
        Json hover = makeObject();
        hover.o["contents"] = makeMarkup(md);
        writeResponse(id, hover);
        return true;
      }
    }
  }

  if (writeCurrentDocParameterHoverResponse(id, uri, *doc, word, cursorOffset,
                                            ctx)) {
    return true;
  }

  std::vector<IndexedDefinition> defs;
      if (workspaceSummaryRuntimeFindDefinitions(word, defs, 64)) {
    std::vector<IndexedDefinition> funcDefs;
    std::vector<IndexedDefinition> macroDefs;
    std::vector<IndexedDefinition> structDefs;
    for (const auto &d : defs) {
      if (d.kind == 12)
        funcDefs.push_back(d);
      else if (d.kind == 14)
        macroDefs.push_back(d);
      else if (d.kind == 23)
        structDefs.push_back(d);
    }

    if (!funcDefs.empty()) {
      IndexedDefinition primary = funcDefs.front();
      std::string preferredDefinitionKey;
      if (!includeContextSummary.hasAmbiguousDefinitions()) {
        DefinitionLocation preferredLocation;
        if (pickBestWorkspaceDefinitionForCurrentContext(uri, word,
                                                         preferredLocation)) {
          preferredDefinitionKey = documentIdentityKey(preferredLocation.uri);
          for (const auto &candidate : funcDefs) {
            if (sameDocumentIdentity(candidate.uri, preferredLocation.uri)) {
              primary = candidate;
              break;
            }
          }
        }
      }
      struct HoverFunctionCandidate {
        std::string label;
        std::vector<std::string> params;
        IndexedDefinition def;
      };
      std::vector<HoverFunctionCandidate> labels;
      labels.reserve(funcDefs.size());
      std::unordered_set<std::string> seen;
      seen.reserve(funcDefs.size());
      std::unordered_set<std::string> seenUris;
      for (const auto &d : funcDefs) {
        const std::string definitionKey = documentIdentityKey(d.uri);
        if (!preferredDefinitionKey.empty() &&
            definitionKey != preferredDefinitionKey) {
          continue;
        }
        if (definitionKey.empty())
          continue;
        if (seenUris.insert(definitionKey).second) {
          std::string defText;
          if (ctx.readDocumentText(d.uri, defText)) {
            uint64_t dEpoch = 0;
            if (const Document *dDoc = ctx.findDocument(d.uri))
              dEpoch = dDoc->epoch;
            std::vector<SemanticSnapshotFunctionOverloadInfo> overloads;
            if (querySemanticSnapshotFunctionOverloads(
                    d.uri, defText, dEpoch, ctx.workspaceFolders,
                    ctx.includePaths, ctx.shaderExtensions,
                    ctx.preprocessorDefines, word, overloads)) {
              for (const auto &overload : overloads) {
                IndexedDefinition overloadDef;
                overloadDef.name = word;
                overloadDef.type = overload.returnType;
                overloadDef.uri = d.uri;
                overloadDef.line = overload.line;
                overloadDef.start = overload.character;
                overloadDef.end =
                    overload.character + static_cast<int>(word.size());
                std::string key;
                key.reserve(overload.label.size() + d.uri.size() + 64);
                key.append(overload.label);
                key.push_back('|');
                key.append(d.uri);
                key.push_back('|');
                key.append(std::to_string(overload.line));
                key.push_back('|');
                key.append(std::to_string(overload.character));
                if (!seen.insert(key).second)
                  continue;
                labels.push_back(HoverFunctionCandidate{
                    overload.label, overload.parameters, overloadDef});
              }
            }
          }
        }
      }
      size_t selectedLabelIndex = 0;
      if (looksLikeCall && kEnableOverloadResolver && labels.size() > 1) {
        std::vector<CandidateSignature> resolverCandidates;
        resolverCandidates.reserve(labels.size());
        for (const auto &item : labels) {
          CandidateSignature candidate;
          candidate.name = word;
          candidate.displayLabel = item.label;
          candidate.displayParams = item.params;
          candidate.sourceUri = item.def.uri;
          candidate.sourceLine = item.def.line;
          candidate.visibilityCondition = "";
          candidate.params.reserve(item.params.size());
          for (const auto &param : item.params) {
            ParamDesc desc;
            desc.name = extractParameterName(param);
            desc.type = parseParamTypeDescFromDecl(param);
            candidate.params.push_back(std::move(desc));
          }
          resolverCandidates.push_back(std::move(candidate));
        }
        std::vector<TypeDesc> argumentTypes;
        inferCallArgumentTypesAtCursor(uri, doc->text, cursorOffset,
                                       doc->epoch, ctx.workspaceFolders,
                                       ctx.includePaths, ctx.shaderExtensions,
                                       ctx.preprocessorDefines,
                                       argumentTypes);
        ResolveCallContext resolveContext;
        resolveContext.defines = ctx.preprocessorDefines;
        resolveContext.allowNarrowing = false;
        resolveContext.enableVisibilityFiltering = true;
        resolveContext.allowPartialArity = true;
        ResolveCallResult resolveResult = resolveCallCandidates(
            resolverCandidates, argumentTypes, resolveContext);
        recordOverloadResolverResult(resolveResult);
        if (!resolveResult.rankedCandidates.empty() &&
            (resolveResult.status == ResolveCallStatus::Resolved ||
             resolveResult.status == ResolveCallStatus::Ambiguous)) {
          const int idx = resolveResult.rankedCandidates.front().candidateIndex;
          if (idx >= 0 && static_cast<size_t>(idx) < labels.size())
            selectedLabelIndex = static_cast<size_t>(idx);
        }
      }
      std::string primaryLabel =
          labels.empty() ? (word + "(...)") : labels[selectedLabelIndex].label;
      std::vector<std::string> primaryParams;
      if (!labels.empty()) {
        primary = labels[selectedLabelIndex].def;
        primaryParams = labels[selectedLabelIndex].params;
      }

      std::string primaryDoc;
      std::string primaryInlineDoc;
      std::string defText;
      if (ctx.readDocumentText(primary.uri, defText)) {
        primaryDoc = extractLeadingDocumentationAtLine(defText, primary.line);
        primaryInlineDoc = extractTrailingInlineCommentAtLine(
            defText, primary.line, primary.end);
      }

      HoverFunctionMarkdownInput functionInput;
      functionInput.code = primaryLabel;
      functionInput.kindLabel = "(HLSL function)";
      functionInput.returnType =
          primary.type.empty()
              ? extractReturnTypeFromFunctionLabel(primaryLabel, word)
              : primary.type;
      functionInput.parameters = primaryParams;
      functionInput.definedAt =
          formatFileLineDisplay(primary.uri, primary.line, uri);
      functionInput.leadingDoc = primaryDoc;
      functionInput.inlineDoc = primaryInlineDoc;
      if (!looksLikeCall && labels.size() > 1) {
        functionInput.selectionNote =
            "(Multiple candidates) Unable to select best overload reliably.";
      }
      appendIncludeContextFunctionSummaryIfNeeded(
          uri, primary.uri, word, includeContextSummary, ctx, functionInput);
      const bool hasIncludeContextSummary =
          functionInput.listTitle == "Candidate definitions" &&
          !functionInput.listItems.empty();
      if (labels.size() > 1 && !hasIncludeContextSummary) {
        std::vector<HoverLocationListItem> uniqueItems;
        std::unordered_set<std::string> seenItems;
        bool hasMoreUniqueItems = false;
        for (size_t i = 0; i < labels.size(); i++) {
          HoverLocationListItem item;
          item.label = labels[i].label;
          item.locationDisplay =
              formatFileLineDisplay(labels[i].def.uri, labels[i].def.line, uri);
          const std::string itemKey = item.label + "|" + item.locationDisplay;
          if (!seenItems.insert(itemKey).second)
            continue;
          if (uniqueItems.size() >= 50) {
            hasMoreUniqueItems = true;
            break;
          }
          uniqueItems.push_back(std::move(item));
        }
        if (uniqueItems.size() > 1) {
          functionInput.listTitle = "Overloads";
          functionInput.listItems = std::move(uniqueItems);
          functionInput.appendEllipsisAfterList = hasMoreUniqueItems;
        }
      }
      std::string md = renderHoverFunctionMarkdown(functionInput);
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }

    if (!macroDefs.empty() && funcDefs.empty()) {
      const IndexedDefinition &d = macroDefs.front();
      if (writeMacroHoverResponse(id, uri, d.uri, d.line, word, ctx,
                                  &macroDefs)) {
        return true;
      }
    }

    if (!structDefs.empty()) {
      IndexedDefinition d = structDefs.front();
      std::string md;
      md += formatCppCodeBlock("struct " + word);
      md += "\n\nDefined at: ";
      md += formatFileLineDisplay(d.uri, d.line, uri);
      std::string structText;
      std::vector<SemanticSnapshotStructFieldInfo> fields;
      uint64_t dEpoch = 0;
      if (const Document *dDoc = ctx.findDocument(d.uri))
        dEpoch = dDoc->epoch;
      if (ctx.readDocumentText(d.uri, structText) &&
          queryStructFieldInfosWithInlineIncludeFallback(
              d.uri, structText, dEpoch, word,
              /*allowInlineIncludeFallback=*/sameDocumentIdentity(d.uri, uri),
              ctx, fields) &&
          !fields.empty()) {
        appendStructFieldMarkdown(md, fields);
      } else {
        std::vector<std::string> fieldNames;
      if (workspaceSummaryRuntimeGetStructFields(word, fieldNames) &&
          !fieldNames.empty()) {
          std::vector<SemanticSnapshotStructFieldInfo> fallback;
          fallback.reserve(fieldNames.size());
          for (const auto &fieldName : fieldNames) {
            SemanticSnapshotStructFieldInfo item;
            item.name = fieldName;
          workspaceSummaryRuntimeGetStructMemberType(word, fieldName,
                                                     item.type);
            fallback.push_back(std::move(item));
          }
          appendStructFieldMarkdown(md, fallback);
        }
      }
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    DefinitionLocation structLoc;
      if (workspaceSummaryRuntimeFindStructDefinition(word, structLoc)) {
      std::string md;
      md += formatCppCodeBlock("struct " + word);
      md += "\n\nDefined at: ";
      md += formatFileLineDisplay(structLoc.uri, structLoc.line, uri);
      std::string structText;
      std::vector<SemanticSnapshotStructFieldInfo> fields;
      uint64_t structEpoch = 0;
      if (const Document *structDoc = ctx.findDocument(structLoc.uri))
        structEpoch = structDoc->epoch;
      if (ctx.readDocumentText(structLoc.uri, structText) &&
          queryStructFieldInfosWithInlineIncludeFallback(
              structLoc.uri, structText, structEpoch, word,
              /*allowInlineIncludeFallback=*/true, ctx, fields) &&
          !fields.empty()) {
        appendStructFieldMarkdown(md, fields);
      } else {
        std::vector<std::string> fieldNames;
      if (workspaceSummaryRuntimeGetStructFields(word, fieldNames) &&
          !fieldNames.empty()) {
          std::vector<SemanticSnapshotStructFieldInfo> fallback;
          fallback.reserve(fieldNames.size());
          for (const auto &fieldName : fieldNames) {
            SemanticSnapshotStructFieldInfo item;
            item.name = fieldName;
          workspaceSummaryRuntimeGetStructMemberType(word, fieldName,
                                                     item.type);
            fallback.push_back(std::move(item));
          }
          appendStructFieldMarkdown(md, fallback);
        }
      }
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    std::vector<SemanticSnapshotStructFieldInfo> fieldInfos;
    DefinitionLocation indexedStructLoc;
    const bool hasIndexedStructLoc =
      workspaceSummaryRuntimeFindStructDefinition(word, indexedStructLoc);
    const bool allowInlineIncludeFallback =
        !hasIndexedStructLoc || sameDocumentIdentity(indexedStructLoc.uri, uri);
    queryStructFieldInfosWithInlineIncludeFallback(
        uri, doc->text, doc->epoch, word, allowInlineIncludeFallback, ctx,
        fieldInfos);

    std::vector<std::string> fields;
    bool haveFields = false;
    if (fieldInfos.empty())
      haveFields = workspaceSummaryRuntimeGetStructFields(word, fields);

    if ((haveFields && !fields.empty()) || !fieldInfos.empty()) {
      DefinitionLocation loc;
    bool hasLoc = workspaceSummaryRuntimeFindDefinition(word, loc);
      std::string md;
      md += formatCppCodeBlock("struct " + word);
      if (hasLoc) {
        md += "\n\nDefined at: ";
        md += formatFileLineDisplay(loc.uri, loc.line, uri);
      }
      if (!fieldInfos.empty()) {
        std::unordered_set<std::string> seen;
        std::vector<SemanticSnapshotStructFieldInfo> uniqueFields;
        uniqueFields.reserve(fieldInfos.size());
        for (const auto &field : fieldInfos) {
          if (!seen.insert(field.name).second)
            continue;
          uniqueFields.push_back(field);
        }
        appendStructFieldMarkdown(md, uniqueFields);
      } else {
        std::unordered_set<std::string> seen;
        std::vector<SemanticSnapshotStructFieldInfo> fallback;
        fallback.reserve(fields.size());
        for (const auto &fieldName : fields) {
          if (!seen.insert(fieldName).second)
            continue;
          SemanticSnapshotStructFieldInfo item;
          item.name = fieldName;
          workspaceSummaryRuntimeGetStructMemberType(word, fieldName,
                                                     item.type);
          fallback.push_back(std::move(item));
        }
        appendStructFieldMarkdown(md, fallback);
      }
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    DeclCandidate decl;
    if (findBestDeclarationUpTo(doc->text, word, cursorOffset, decl) &&
        decl.found) {
      if (decl.braceDepth == 0 &&
          writeMacroHoverResponse(id, uri, uri, decl.line, word, ctx)) {
        return true;
      }
      bool isParam = false;
      TypeEvalResult typeEval = resolveHoverTypeAtDeclaration(
          uri, *doc, word, cursorOffset, ctx, isParam);
      const std::string &typeName = typeEval.type;
      if (decl.braceDepth == 0 && !isParam) {
        int nameChar = byteOffsetInLineToUtf16(decl.lineText, decl.nameBytePos);
        std::string functionLabel;
        std::vector<std::string> functionParams;
        const bool fastSig = queryFunctionSignatureWithSemanticFallback(
            uri, doc->text, doc->epoch, word, decl.line, nameChar, ctx,
            functionLabel, functionParams);
        if (fastSig && !functionLabel.empty()) {
          HoverFunctionMarkdownInput functionInput;
          functionInput.code = functionLabel;
          functionInput.kindLabel = "(HLSL function)";
          functionInput.returnType =
              extractReturnTypeFromFunctionLabel(functionLabel, word);
          functionInput.parameters = functionParams;
          functionInput.definedAt =
              formatFileLineDisplay(uri, decl.line, uri);
          functionInput.leadingDoc =
              extractLeadingDocumentationAtLine(doc->text, decl.line);
          int endChar = byteOffsetInLineToUtf16(
              decl.lineText, static_cast<int>(decl.nameBytePos + word.size()));
          functionInput.inlineDoc =
              extractTrailingInlineCommentAtLine(doc->text, decl.line, endChar);
          appendIncludeContextFunctionSummaryIfNeeded(
              uri, uri, word, includeContextSummary, ctx, functionInput);
          std::string md = renderHoverFunctionMarkdown(functionInput);
          Json hover = makeObject();
          hover.o["contents"] = makeMarkup(md);
          writeResponse(id, hover);
          return true;
        }
      }
      std::string code = (typeName.empty() ? std::string() : (typeName + " "));
      code += word;
      HoverSymbolMarkdownInput symbolInput;
      symbolInput.code = code;
      if (decl.braceDepth > 0) {
        symbolInput.notes.push_back("(Local variable)");
      } else if (isParam) {
        symbolInput.notes.push_back("(Parameter)");
      }
      symbolInput.typeName = typeName;
      symbolInput.indeterminateReason = typeEval.reasonCode;
      symbolInput.definedAt = formatFileLineDisplay(uri, decl.line, uri);
      symbolInput.leadingDoc =
          extractLeadingDocumentationAtLine(doc->text, decl.line);
      if (decl.braceDepth == 0 && !isParam) {
        int endByte = static_cast<int>(decl.nameBytePos + word.size());
        int endChar = byteOffsetInLineToUtf16(decl.lineText, endByte);
        symbolInput.inlineDoc =
            extractTrailingInlineCommentAtLine(doc->text, decl.line, endChar);
      }
      appendIncludeContextSymbolNoteIfNeeded(uri, uri, includeContextSummary,
                                             symbolInput);
      std::string md = renderHoverSymbolMarkdown(symbolInput);
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    DefinitionLocation interactiveDefinition;
    if (interactiveResolveDefinitionLocation(uri, *doc, word, cursorOffset, ctx,
                                             interactiveDefinition) &&
        interactiveDefinition.uri == uri) {
      if (writeMacroHoverResponse(id, uri, interactiveDefinition.uri,
                                  interactiveDefinition.line, word, ctx)) {
        return true;
      }
      std::string label;
      std::vector<std::string> paramsOut;
      if (queryFunctionSignatureWithSemanticFallback(
              uri, doc->text, doc->epoch, word, interactiveDefinition.line,
              interactiveDefinition.start, ctx, label, paramsOut) &&
          !label.empty()) {
        HoverFunctionMarkdownInput functionInput;
        functionInput.code = label;
        functionInput.kindLabel = "(HLSL function)";
        functionInput.returnType =
            extractReturnTypeFromFunctionLabel(label, word);
        functionInput.parameters = paramsOut;
        functionInput.definedAt =
            formatFileLineDisplay(uri, interactiveDefinition.line, uri);
        functionInput.leadingDoc =
            extractLeadingDocumentationAtLine(doc->text, interactiveDefinition.line);
        functionInput.inlineDoc = extractTrailingInlineCommentAtLine(
            doc->text, interactiveDefinition.line, interactiveDefinition.end);
        std::string md = renderHoverFunctionMarkdown(functionInput);
        Json hover = makeObject();
        hover.o["contents"] = makeMarkup(md);
        writeResponse(id, hover);
        return true;
      }

      std::string typeName;
      findTypeOfIdentifierInDeclarationLineShared(
          getLineAt(doc->text, interactiveDefinition.line), word, typeName);
      HoverSymbolMarkdownInput symbolInput;
      symbolInput.code =
          (typeName.empty() ? std::string() : (typeName + " ")) + word;
      symbolInput.typeName = typeName;
      symbolInput.definedAt =
          formatFileLineDisplay(uri, interactiveDefinition.line, uri);
      symbolInput.leadingDoc =
          extractLeadingDocumentationAtLine(doc->text, interactiveDefinition.line);
      symbolInput.inlineDoc = extractTrailingInlineCommentAtLine(
          doc->text, interactiveDefinition.line, interactiveDefinition.end);
      appendIncludeContextSymbolNoteIfNeeded(
          uri, interactiveDefinition.uri, includeContextSummary, symbolInput);
      std::string md = renderHoverSymbolMarkdown(symbolInput);
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    SymbolDefinitionResolveOptions resolveOptions;
    resolveOptions.order =
        SymbolDefinitionSearchOrder::WorkspaceThenMacro;
    ResolvedSymbolDefinitionTarget resolvedTarget;
    if (resolveSymbolDefinitionTarget(uri, word, ctx, resolveOptions,
                                      resolvedTarget)) {
      if (resolvedTarget.hasMacroGeneratedFunction) {
        const auto &macroFn = resolvedTarget.macroGeneratedFunction;
        HoverFunctionMarkdownInput functionInput;
        functionInput.code = macroFn.label;
        functionInput.kindLabel = "(HLSL function)";
        functionInput.returnType = macroFn.returnType;
        functionInput.parameters = macroFn.parameterDecls;
        functionInput.definedAt = formatFileLineDisplay(
            macroFn.definition.uri, macroFn.definition.line, uri);
        std::string defText;
        if (ctx.readDocumentText(macroFn.definition.uri, defText)) {
          functionInput.leadingDoc = extractLeadingDocumentationAtLine(
              defText, macroFn.definition.line);
          functionInput.inlineDoc = extractTrailingInlineCommentAtLine(
              defText, macroFn.definition.line, macroFn.definition.end);
        }
        appendIncludeContextFunctionSummaryIfNeeded(
            uri, macroFn.definition.uri, word, includeContextSummary, ctx,
            functionInput);
        std::string md = renderHoverFunctionMarkdown(functionInput);
        Json hover = makeObject();
        hover.o["contents"] = makeMarkup(md);
        writeResponse(id, hover);
        return true;
      }

      std::string typeName;
      workspaceSummaryRuntimeGetSymbolType(word, typeName);
      const DefinitionLocation &loc = resolvedTarget.location;
      if (writeMacroHoverResponse(id, uri, loc.uri, loc.line, word, ctx)) {
        return true;
      }
      std::string defText;
      if (ctx.readDocumentText(loc.uri, defText)) {
        std::string label;
        std::vector<std::string> paramsOut;
        uint64_t locEpoch = 0;
        const Document *locDoc = ctx.findDocument(loc.uri);
        if (locDoc) {
          locEpoch = locDoc->epoch;
        }
        const bool fastSig = queryFunctionSignatureWithSemanticFallback(
            loc.uri, defText, locEpoch, word, loc.line, loc.start, ctx, label,
            paramsOut);
        if (fastSig && !label.empty()) {
          HoverFunctionMarkdownInput functionInput;
          functionInput.code = label;
          functionInput.kindLabel = "(HLSL function)";
          functionInput.returnType =
              extractReturnTypeFromFunctionLabel(label, word);
          functionInput.parameters = paramsOut;
          functionInput.definedAt =
              formatFileLineDisplay(loc.uri, loc.line, uri);
          functionInput.leadingDoc =
              extractLeadingDocumentationAtLine(defText, loc.line);
          functionInput.inlineDoc =
              extractTrailingInlineCommentAtLine(defText, loc.line, loc.end);
          appendIncludeContextFunctionSummaryIfNeeded(
              uri, loc.uri, word, includeContextSummary, ctx, functionInput);
          std::string md = renderHoverFunctionMarkdown(functionInput);
          Json hover = makeObject();
          hover.o["contents"] = makeMarkup(md);
          writeResponse(id, hover);
          return true;
        }
      }

      HoverSymbolMarkdownInput symbolInput;
      symbolInput.code =
          (typeName.empty() ? std::string() : (typeName + " ")) + word;
      symbolInput.typeName = typeName;
      symbolInput.definedAt = formatFileLineDisplay(loc.uri, loc.line, uri);
      if (!defText.empty() || ctx.readDocumentText(loc.uri, defText)) {
        symbolInput.leadingDoc =
            extractLeadingDocumentationAtLine(defText, loc.line);
        symbolInput.inlineDoc =
            extractTrailingInlineCommentAtLine(defText, loc.line, loc.end);
      }
      appendIncludeContextSymbolNoteIfNeeded(uri, loc.uri, includeContextSummary,
                                             symbolInput);
      std::string md = renderHoverSymbolMarkdown(symbolInput);
      Json hover = makeObject();
      hover.o["contents"] = makeMarkup(md);
      writeResponse(id, hover);
      return true;
    }
  }

  {
    std::string md = formatCppCodeBlock(word);
    Json hover = makeObject();
    hover.o["contents"] = makeMarkup(md);
    writeResponse(id, hover);
    return true;
  }
}

