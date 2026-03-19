#pragma once

#include <filesystem>
#include <string>

#include "json.hpp"

struct ResourceBundlePaths {
  std::filesystem::path basePath;
  std::filesystem::path overridePath;
  std::filesystem::path schemaPath;
};

ResourceBundlePaths resolveResourceBundlePaths(const std::string &bundleKey);

bool loadResourceBundleJson(const std::string &bundleKey, Json &baseRoot,
                            Json &overrideRoot, std::string &errorOut);
