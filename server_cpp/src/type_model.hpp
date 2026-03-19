#pragma once

#include <string>
#include <vector>

bool isTypeModelAvailable();
const std::string &getTypeModelError();

const std::vector<std::string> &getTypeModelObjectTypeNames();
bool getTypeModelObjectFamily(const std::string &typeName, std::string &outFamily);
bool isTypeModelTextureLike(const std::string &typeName);
bool isTypeModelSamplerLike(const std::string &typeName);
int getTypeModelCoordDim(const std::string &typeName);
bool typeModelSameObjectFamily(const std::string &leftType,
                               const std::string &rightType);
