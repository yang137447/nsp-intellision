#pragma once

#include <string>
#include <vector>

struct ServerRequestContext;

struct WorkspaceScanPlanOptions {
  bool includeDocumentDirectory = false;
  bool appendWorkspaceFolders = true;
  bool fallbackToWorkspaceFoldersWhenEmpty = false;
  std::vector<std::string> requiredExtensions;
  std::vector<std::string> excludedExtensions;
};

struct WorkspaceScanPlan {
  std::vector<std::string> roots;
  std::vector<std::string> extensions;
  std::string cacheKey;
};

WorkspaceScanPlan buildWorkspaceScanPlan(const ServerRequestContext &ctx,
                                         const std::string &documentUri,
                                         const WorkspaceScanPlanOptions &options);

void resetWorkspaceScanCachesIfPlanChanged(ServerRequestContext &ctx,
                                           const WorkspaceScanPlan &plan);
