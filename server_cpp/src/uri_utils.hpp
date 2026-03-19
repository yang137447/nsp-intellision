#pragma once
#include <string>

std::string uriDecode(const std::string &text);
std::string uriToPath(const std::string &uri);
std::string pathToUri(const std::string &path);
