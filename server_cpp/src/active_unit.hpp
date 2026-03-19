#pragma once

#include <string>

void setActiveUnit(const std::string &uri, const std::string &path);
std::string getActiveUnitUri();
std::string getActiveUnitPath();
