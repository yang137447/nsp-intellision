#pragma once

#include "definition_location.hpp"
#include "json.hpp"
#include "semantic_tokens.hpp"
#include "server_documents.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Request-layer boundary for the core LSP handlers.
//
// Responsibilities:
// - expose the immutable request-scoped snapshot consumed by handlers
// - keep the current scheduling contract readable at the API boundary
// - dispatch core request methods onto current-doc / deferred / workspace
//   runtimes without making handlers own long-lived document state
//
// Non-goals:
// - this header is not the owner of per-document caches or snapshot lifetime
// - it does not define resource facts; handlers must still defer to shared
//   registries and query modules for language knowledge

struct ServerRequestContext {
  // Request-scoped document/config snapshot. Handlers should treat this as
  // read-only input and must not mutate document runtime state through it.
  std::unordered_map<std::string, Document> documents;
  std::vector<std::string> workspaceFolders;
  std::vector<std::string> includePaths;
  std::vector<std::string> shaderExtensions;
  SemanticTokenLegend semanticLegend;
  bool inlayHintsEnabled = true;
  bool inlayHintsParameterNamesEnabled = true;
  bool semanticTokensEnabled = true;
  bool diagnosticsExpensiveRulesEnabled = true;
  int diagnosticsTimeBudgetMs = 1200;
  int diagnosticsMaxItems = 1200;
  bool diagnosticsFastEnabled = true;
  int diagnosticsFastDelayMs = 90;
  int diagnosticsFastTimeBudgetMs = 180;
  int diagnosticsFastMaxItems = 240;
  bool diagnosticsFullEnabled = true;
  int diagnosticsFullDelayMs = 700;
  bool diagnosticsFullExpensiveRulesEnabled = true;
  int diagnosticsFullTimeBudgetMs = 1200;
  int diagnosticsFullMaxItems = 1200;
  int diagnosticsWorkerCount = 2;
  bool diagnosticsAutoWorkerCount = true;
  bool semanticCacheEnabled = true;
  bool diagnosticsIndeterminateEnabled = true;
  int diagnosticsIndeterminateSeverity = 4;
  int diagnosticsIndeterminateMaxItems = 200;
  bool diagnosticsIndeterminateSuppressWhenErrors = true;
  int indexingWorkerCount = 16;
  int indexingQueueCapacity = 4096;
  std::unordered_map<std::string, int> preprocessorDefines;
  std::function<bool()> isCancellationRequested;

  const std::unordered_map<std::string, Document> &documentSnapshot() const {
    return documents;
  }

  const Document *findDocument(const std::string &uri) const {
    auto it = documents.find(uri);
    if (it == documents.end())
      return nullptr;
    return &it->second;
  }

  bool readDocumentText(const std::string &uri, std::string &text) const {
    return loadDocumentText(uri, documents, text);
  }
};

struct SignatureHelpMetricsSnapshot {
  uint64_t indeterminateTotal = 0;
  uint64_t indeterminateReasonCallTargetUnknown = 0;
  uint64_t indeterminateReasonDefinitionTextUnavailable = 0;
  uint64_t indeterminateReasonSignatureExtractFailed = 0;
  uint64_t indeterminateReasonOther = 0;
  uint64_t overloadResolverAttempts = 0;
  uint64_t overloadResolverResolved = 0;
  uint64_t overloadResolverAmbiguous = 0;
  uint64_t overloadResolverNoViable = 0;
  uint64_t overloadResolverShadowMismatch = 0;
};

SignatureHelpMetricsSnapshot takeSignatureHelpMetricsSnapshot();

// Dispatches the core LSP request methods owned by server_request_handlers.cpp.
//
// Current M1 scheduling contract:
// - interactive high-priority path: completion, hover, signature help, and
//   current-document short-path definition
// - background latest-only + cancellable path: semantic tokens, inlay hints,
//   document symbols, references, prepareRename, rename, workspace symbol
//
// Handler implementations may consult workspace summary on current-doc miss, but
// interactive requests must not reintroduce include-graph scans or full
// workspace rescans as hidden hot-path fallback.
bool handleCoreRequestMethods(const std::string &method, const Json &id,
                              const Json *params, ServerRequestContext &ctx,
                              const std::vector<std::string> &keywords,
                              const std::vector<std::string> &directives);
