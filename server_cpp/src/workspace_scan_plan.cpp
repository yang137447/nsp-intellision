#include "workspace_scan_plan.hpp"

#include "server_request_handlers.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <cctype>

namespace {

bool isAbsolutePath(const std::string &path) {
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
      path[1] == ':') {
    return true;
  }
  return path.rfind("\\\\", 0) == 0 || path.rfind("/", 0) == 0;
}

std::string joinPath(const std::string &base, const std::string &child) {
  if (base.empty()) {
    return child;
  }
  if (base.back() == '/' || base.back() == '\\') {
    return base + child;
  }
  return base + "\\" + child;
}

void addExtensionIfMissing(std::vector<std::string> &extensions,
                           const std::string &ext) {
  for (const auto &item : extensions) {
    if (item == ext) {
      return;
    }
  }
  extensions.push_back(ext);
}

std::string buildWorkspaceScanCacheKey(const WorkspaceScanPlan &plan) {
  std::string cacheKey;
  for (const auto &root : plan.roots) {
    cacheKey.append(root);
    cacheKey.push_back('|');
  }
  cacheKey.push_back('#');
  for (const auto &ext : plan.extensions) {
    cacheKey.append(ext);
    cacheKey.push_back('|');
  }
  return cacheKey;
}

} // namespace

WorkspaceScanPlan buildWorkspaceScanPlan(const ServerRequestContext &ctx,
                                         const std::string &documentUri,
                                         const WorkspaceScanPlanOptions &options) {
  WorkspaceScanPlan plan;

  if (options.includeDocumentDirectory) {
    std::string documentPath = uriToPath(documentUri);
    if (!documentPath.empty()) {
      size_t lastSlash = documentPath.find_last_of("\\/");
      if (lastSlash != std::string::npos) {
        plan.roots.push_back(documentPath.substr(0, lastSlash));
      }
    }
  }

  for (const auto &inc : ctx.includePaths) {
    if (inc.empty()) {
      continue;
    }
    if (isAbsolutePath(inc)) {
      plan.roots.push_back(inc);
      continue;
    }
    for (const auto &folder : ctx.workspaceFolders) {
      if (!folder.empty()) {
        plan.roots.push_back(joinPath(folder, inc));
      }
    }
  }

  if (options.fallbackToWorkspaceFoldersWhenEmpty && plan.roots.empty()) {
    for (const auto &folder : ctx.workspaceFolders) {
      if (!folder.empty()) {
        plan.roots.push_back(folder);
      }
    }
  }

  if (options.appendWorkspaceFolders) {
    for (const auto &folder : ctx.workspaceFolders) {
      if (!folder.empty()) {
        plan.roots.push_back(folder);
      }
    }
  }

  plan.extensions = ctx.shaderExtensions;
  for (const auto &ext : options.requiredExtensions) {
    addExtensionIfMissing(plan.extensions, ext);
  }
  for (const auto &ext : options.excludedExtensions) {
    plan.extensions.erase(
        std::remove(plan.extensions.begin(), plan.extensions.end(), ext),
        plan.extensions.end());
  }

  plan.cacheKey = buildWorkspaceScanCacheKey(plan);
  return plan;
}

void resetWorkspaceScanCachesIfPlanChanged(ServerRequestContext &ctx,
                                           const WorkspaceScanPlan &plan) {
  if (plan.cacheKey == ctx.scanCacheKey) {
    return;
  }
  ctx.scanCacheKey = plan.cacheKey;
  ctx.scanDefinitionCache.clear();
  ctx.scanDefinitionMisses.clear();
  ctx.scanStructFieldsCache.clear();
  ctx.scanStructFieldsMisses.clear();
}
