#pragma once

#include "diagnostics.hpp"
#include "json.hpp"
#include "server_documents.hpp"

#include <string>
#include <unordered_map>
#include <vector>

// Internal diagnostics-audit debug request helpers.
//
// Responsibilities:
// - expose read-only server-side probes used by real-workspace audit tests
// - keep unit include-closure collection and one-shot diagnostics builds aligned
//   with the same shared workspace summary and diagnostics entry points used by
//   production code
//
// Non-goals:
// - does not publish diagnostics or change visible LSP behavior
// - does not own active-unit selection, document runtime state, or scheduling

struct DiagnosticsAuditDebugContext {
  std::unordered_map<std::string, Document> documents;
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::string shaderCompilerPath;
  std::vector<std::string> shaderExtensions;
  std::unordered_map<std::string, int> defines;
  DiagnosticsBuildOptions diagnosticsOptions;
};

Json buildDiagnosticsAuditIncludeClosureDebugResponse(const Json *params);

Json buildDiagnosticsAuditDiagnosticsDebugResponse(
    const Json *params, const DiagnosticsAuditDebugContext &context,
    const std::string &fallbackActiveUnitUri,
    const std::string &fallbackActiveUnitPath);
