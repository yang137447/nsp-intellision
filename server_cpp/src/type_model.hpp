#pragma once

#include <string>
#include <vector>

// Shared object-type query entry backed by resources/types/*.
// This module exposes consumer-ready object semantics so request handlers,
// hover/signature rendering, and diagnostics do not have to re-interpret raw
// resource fields like coordDim/isArray on their own.
bool isTypeModelAvailable();
const std::string &getTypeModelError();

const std::vector<std::string> &getTypeModelObjectTypeNames();
bool getTypeModelObjectFamily(const std::string &typeName, std::string &outFamily);
bool isTypeModelTextureLike(const std::string &typeName);
bool isTypeModelSamplerLike(const std::string &typeName);
// Returns the spatial coordDim from resources/types/object_types.
// This does not include array-slice dimensions.
int getTypeModelCoordDim(const std::string &typeName);
// Returns the coordinate dimension expected by Sample/Gather-like methods.
// Array textures contribute one extra slice dimension.
int getTypeModelSampleCoordDim(const std::string &typeName);
// Returns the coordinate dimension expected by Load-like methods.
// This includes sample coordinates plus the extra integer location component.
int getTypeModelLoadCoordDim(const std::string &typeName);
bool typeModelSameObjectFamily(const std::string &leftType,
                               const std::string &rightType);
