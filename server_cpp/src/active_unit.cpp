#include "active_unit.hpp"

#include <mutex>

static std::mutex gActiveUnitMutex;
static std::string gActiveUnitUri;
static std::string gActiveUnitPath;
static uint64_t gActiveUnitRevision = 0;

void setActiveUnit(const std::string &uri, const std::string &path) {
  std::lock_guard<std::mutex> lock(gActiveUnitMutex);
  if (gActiveUnitUri == uri && gActiveUnitPath == path)
    return;
  gActiveUnitUri = uri;
  gActiveUnitPath = path;
  gActiveUnitRevision++;
}

std::string getActiveUnitUri() {
  std::lock_guard<std::mutex> lock(gActiveUnitMutex);
  return gActiveUnitUri;
}

std::string getActiveUnitPath() {
  std::lock_guard<std::mutex> lock(gActiveUnitMutex);
  return gActiveUnitPath;
}

uint64_t getActiveUnitRevision() {
  std::lock_guard<std::mutex> lock(gActiveUnitMutex);
  return gActiveUnitRevision;
}
