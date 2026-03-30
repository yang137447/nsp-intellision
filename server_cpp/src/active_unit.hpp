#pragma once

#include <cstdint>
#include <string>

// Tracks the currently selected active unit for workspace-sensitive analysis.
// `getActiveUnitRevision()` increments whenever the selected unit changes so
// background work can drop stale unit-specific tasks promptly.
void setActiveUnit(const std::string &uri, const std::string &path);
std::string getActiveUnitUri();
std::string getActiveUnitPath();
uint64_t getActiveUnitRevision();
