#include "include_resolver.hpp"

#include "uri_utils.hpp"

#include <cctype>

std::vector<std::string>
resolveIncludeCandidates(const std::string &currentUri,
                         const std::string &includePath,
                         const std::vector<std::string> &workspaceFolders,
                         const std::vector<std::string> &includePaths,
                         const std::vector<std::string> &shaderExtensions) {
  auto isAbsolutePath = [](const std::string &path) {
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':')
      return true;
    return path.rfind("\\\\", 0) == 0 || path.rfind("/", 0) == 0;
  };
  auto hasExtension = [](const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    return dot != std::string::npos &&
           (slash == std::string::npos || dot > slash);
  };
  auto joinPath = [](const std::string &base, const std::string &child) {
    if (base.empty())
      return child;
    char sep = '\\';
    if (base.back() == '/' || base.back() == '\\')
      return base + child;
    return base + sep + child;
  };

  std::vector<std::string> candidates;
  if (isAbsolutePath(includePath)) {
    candidates.push_back(includePath);
    return candidates;
  }
  const std::string currentPath = uriToPath(currentUri);
  if (!currentPath.empty()) {
    size_t lastSlash = currentPath.find_last_of("\\/");
    std::string base = lastSlash == std::string::npos
                           ? currentPath
                           : currentPath.substr(0, lastSlash);
    candidates.push_back(joinPath(base, includePath));
  }
  for (const auto &inc : includePaths) {
    if (isAbsolutePath(inc)) {
      candidates.push_back(joinPath(inc, includePath));
    }
  }
  for (const auto &folder : workspaceFolders) {
    for (const auto &inc : includePaths) {
      std::string incRoot =
          isAbsolutePath(inc) ? inc : joinPath(folder, inc);
      candidates.push_back(joinPath(incRoot, includePath));
    }
  }
  std::vector<std::string> expanded;
  for (const auto &candidate : candidates) {
    if (hasExtension(candidate)) {
      expanded.push_back(candidate);
    } else {
      for (const auto &ext : shaderExtensions) {
        expanded.push_back(candidate + ext);
      }
    }
  }
  return expanded;
}
