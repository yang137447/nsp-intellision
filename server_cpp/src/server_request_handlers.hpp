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

struct ServerRequestContext {
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
  std::string scanCacheKey;
  std::unordered_map<std::string, DefinitionLocation> scanDefinitionCache;
  std::unordered_set<std::string> scanDefinitionMisses;
  std::unordered_map<std::string, std::vector<std::string>>
      scanStructFieldsCache;
  std::unordered_set<std::string> scanStructFieldsMisses;
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

bool handleCoreRequestMethods(const std::string &method, const Json &id,
                              const Json *params, ServerRequestContext &ctx,
                              const std::vector<std::string> &keywords,
                              const std::vector<std::string> &directives);
