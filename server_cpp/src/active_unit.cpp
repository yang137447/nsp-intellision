#include "active_unit.hpp"

#include <mutex>

static std::mutex gActiveUnitMutex;
static std::string gActiveUnitUri;
static std::string gActiveUnitPath;

void setActiveUnit(const std::string &uri, const std::string &path) {
  std::lock_guard<std::mutex> lock(gActiveUnitMutex);
  gActiveUnitUri = uri;
  gActiveUnitPath = path;
}

std::string getActiveUnitUri() {
  std::lock_guard<std::mutex> lock(gActiveUnitMutex);
  return gActiveUnitUri;
}

std::string getActiveUnitPath() {
  std::lock_guard<std::mutex> lock(gActiveUnitMutex);
  return gActiveUnitPath;
}
