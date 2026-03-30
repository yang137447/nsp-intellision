#include "diagnostics.hpp"
#include "diagnostics_semantic.hpp"

#include "callsite_parser.hpp"
#include "diagnostics_emit.hpp"
#include "diagnostics_indeterminate.hpp"
#include "diagnostics_io.hpp"
#include "diagnostics_preprocessor.hpp"
#include "diagnostics_syntax.hpp"
#include "fast_ast.hpp"
#include "full_ast.hpp"
#include "hlsl_builtin_docs.hpp"
#include "hover_markdown.hpp"
#include "include_resolver.hpp"
#include "indeterminate_reasons.hpp"
#include "language_registry.hpp"
#include "lsp_helpers.hpp"
#include "macro_generated_functions.hpp"
#include "nsf_lexer.hpp"
#include "overload_resolver.hpp"
#include "preprocessor_view.hpp"
#include "semantic_cache.hpp"
#include "semantic_snapshot.hpp"
#include "server_parse.hpp"
#include "text_utils.hpp"
#include "type_desc.hpp"
#include "type_eval.hpp"
#include "type_model.hpp"
#include "uri_utils.hpp"
#include "workspace_summary_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

static std::string
makeDefinesFingerprint(const std::unordered_map<std::string, int> &defines) {
  std::vector<std::pair<std::string, int>> ordered(defines.begin(),
                                                   defines.end());
  std::sort(
      ordered.begin(), ordered.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
  std::ostringstream oss;
  for (const auto &entry : ordered) {
    oss << entry.first << "=" << entry.second << ";";
  }
  return oss.str();
}


DiagnosticsBuildResult
buildDiagnosticsWithOptions(const std::string &uri, const std::string &text,
                            const std::vector<std::string> &workspaceFolders,
                            const std::vector<std::string> &includePaths,
                            const std::vector<std::string> &shaderExtensions,
                            const std::unordered_map<std::string, int> &defines,
                            const DiagnosticsBuildOptions &options) {
  DiagnosticsBuildResult result;
  result.diagnostics = makeArray();
  const auto startTime = std::chrono::steady_clock::now();
  const size_t maxDiagnostics =
      static_cast<size_t>(std::max(20, options.maxItems));
  const auto timeBudget =
      std::chrono::milliseconds(std::max(30, options.timeBudgetMs));
  auto budgetExpired = [&]() {
    return std::chrono::steady_clock::now() - startTime > timeBudget;
  };
  size_t indeterminateCount = 0;
  const size_t indeterminateMaxItems =
      static_cast<size_t>(std::max(0, options.indeterminateMaxItems));
  if (options.semanticCacheEnabled) {
    SemanticCacheKey key;
    key.includePaths = includePaths;
    key.shaderExtensions = shaderExtensions;
    key.workspaceFolders = workspaceFolders;
    key.definesFingerprint = makeDefinesFingerprint(defines);
    key.unitPath = uriToPath(uri);
    auto snapshot = semanticCacheGetSnapshot(key, uri, options.documentEpoch);
    if (!snapshot) {
      SemanticSnapshot created;
      created.uri = uri;
      created.documentEpoch = options.documentEpoch;
      created.includeGraphUrisOrdered.push_back(uri);
      semanticCacheUpsertSnapshot(key, created);
    }
  }

  collectBracketDiagnostics(text, result.diagnostics);
  if (result.diagnostics.a.size() >= maxDiagnostics) {
    result.truncated = true;
  }
  const auto preprocessorView = buildDiagnosticsPreprocessorView(
      uri, text, workspaceFolders, includePaths, shaderExtensions, defines,
      options);
  if (!result.truncated) {
    collectPreprocessorDiagnostics(text, result.diagnostics);
    if (result.diagnostics.a.size() >= maxDiagnostics) {
      result.truncated = true;
    }
  }
  if (!result.truncated) {
    for (const auto &diag : preprocessorView.conditionDiagnostics) {
      if (result.diagnostics.a.size() >= maxDiagnostics) {
        result.truncated = true;
        break;
      }
      result.diagnostics.a.push_back(
          makeDiagnostic(text, diag.line, diag.start, diag.end, diag.severity,
                         "nsf", diag.message));
    }
  }
  if (!result.truncated && options.enableExpensiveRules && !budgetExpired()) {
    collectReturnAndTypeDiagnostics(
        uri, text, workspaceFolders, includePaths, shaderExtensions, defines,
        preprocessorView, result.diagnostics, options.timeBudgetMs,
        maxDiagnostics,
        result.timedOut, options.indeterminateEnabled,
        options.indeterminateSeverity, indeterminateMaxItems,
        indeterminateCount);
    if (result.diagnostics.a.size() >= maxDiagnostics) {
      result.truncated = true;
    }
  } else if (!options.enableExpensiveRules) {
    result.heavyRulesSkipped = true;
  } else if (budgetExpired()) {
    result.timedOut = true;
    result.heavyRulesSkipped = true;
  }

  std::istringstream stream(text);
  std::string lineText;
  int lineIndex = 0;
  bool inBlockComment = false;
  std::unordered_map<std::string, bool> includeCandidateExistsCache;
  while (std::getline(stream, lineText)) {
    if (result.diagnostics.a.size() >= maxDiagnostics) {
      result.truncated = true;
      break;
    }
    if (budgetExpired()) {
      result.timedOut = true;
      result.truncated = true;
      break;
    }
    size_t includePos =
        findIncludeDirectiveOutsideComments(lineText, inBlockComment);
    if (includePos == std::string::npos) {
      lineIndex++;
      continue;
    }
    if (lineIndex < static_cast<int>(preprocessorView.lineActive.size()) &&
        !preprocessorView.lineActive[lineIndex]) {
      lineIndex++;
      continue;
    }

    std::string includePath;
    int spanStart = -1;
    int spanEnd = -1;
    if (!findIncludePathSpan(lineText, includePos, spanStart, spanEnd)) {
      result.diagnostics.a.push_back(makeDiagnostic(
          text, lineIndex, static_cast<int>(includePos),
          static_cast<int>(includePos + std::string("#include").size()), 2,
          "nsf", "Invalid #include syntax."));
      lineIndex++;
      continue;
    }
    includePath = lineText.substr(static_cast<size_t>(spanStart),
                                  static_cast<size_t>(spanEnd - spanStart));

    auto candidates = resolveIncludeCandidates(
        uri, includePath, workspaceFolders, includePaths, shaderExtensions);
    bool found = false;
    for (const auto &candidate : candidates) {
      auto cacheIt = includeCandidateExistsCache.find(candidate);
      bool exists = false;
      if (cacheIt == includeCandidateExistsCache.end()) {
        struct _stat statBuffer;
        exists = (_stat(candidate.c_str(), &statBuffer) == 0);
        includeCandidateExistsCache.emplace(candidate, exists);
      } else {
        exists = cacheIt->second;
      }
      if (exists) {
        found = true;
        break;
      }
    }
    if (!found) {
      result.diagnostics.a.push_back(
          makeDiagnostic(text, lineIndex, spanStart, spanEnd, 2, "nsf",
                         "Cannot resolve include: " + includePath));
    }
    lineIndex++;
  }

  int blockLine = -1;
  int blockChar = -1;
  if (result.diagnostics.a.size() < maxDiagnostics &&
      hasUnterminatedBlockComment(text, blockLine, blockChar)) {
    result.diagnostics.a.push_back(
        makeDiagnostic(text, blockLine, blockChar, blockChar + 2, 1, "nsf",
                       "Unterminated block comment."));
  } else if (result.diagnostics.a.size() >= maxDiagnostics) {
    result.truncated = true;
  }
  if (result.diagnostics.a.size() > maxDiagnostics) {
    result.diagnostics.a.resize(maxDiagnostics);
    result.truncated = true;
  }
  if (options.indeterminateEnabled &&
      result.diagnostics.a.size() < maxDiagnostics) {
    if (result.timedOut && indeterminateCount < indeterminateMaxItems) {
      result.diagnostics.a.push_back(makeDiagnosticWithCodeAndReason(
          text, 0, 0, 0, options.indeterminateSeverity, "nsf",
          "Indeterminate diagnostics: time budget exhausted.",
          "NSF_INDET_BUDGET_TIMEOUT",
          IndeterminateReason::DiagnosticsBudgetTimeout));
      indeterminateCount++;
    }
    if (result.heavyRulesSkipped &&
        indeterminateCount < indeterminateMaxItems) {
      result.diagnostics.a.push_back(makeDiagnosticWithCodeAndReason(
          text, 0, 0, 0, options.indeterminateSeverity, "nsf",
          "Indeterminate diagnostics: heavy rules skipped.",
          "NSF_INDET_HEAVY_RULES_SKIPPED",
          IndeterminateReason::DiagnosticsHeavyRulesSkipped));
      indeterminateCount++;
    }
  }
  if (options.indeterminateEnabled && options.indeterminateSuppressWhenErrors &&
      hasDiagnosticErrorOrWarning(result.diagnostics)) {
    std::vector<Json> filtered;
    filtered.reserve(result.diagnostics.a.size());
    for (const auto &diag : result.diagnostics.a) {
      if (isIndeterminateDiagnostic(diag))
        continue;
      filtered.push_back(diag);
    }
    result.diagnostics.a = std::move(filtered);
  }
  fillIndeterminateMetricsFromDiagnostics(result.diagnostics, result);
  result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - startTime)
                         .count();
  return result;
}

Json buildDiagnostics(const std::string &uri, const std::string &text,
                      const std::vector<std::string> &workspaceFolders,
                      const std::vector<std::string> &includePaths,
                      const std::vector<std::string> &shaderExtensions,
                      const std::unordered_map<std::string, int> &defines) {
  DiagnosticsBuildOptions options;
  return buildDiagnosticsWithOptions(uri, text, workspaceFolders, includePaths,
                                     shaderExtensions, defines, options)
      .diagnostics;
}

SemanticCacheMetricsSnapshot takeSemanticCacheMetricsSnapshot() {
  const SemanticCacheManagerStats stats = semanticCacheConsumeStats();
  SemanticCacheMetricsSnapshot snapshot;
  snapshot.snapshotHit = stats.snapshotHit;
  snapshot.snapshotMiss = stats.snapshotMiss;
  return snapshot;
}
