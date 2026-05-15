#pragma once

#include <string>
#include <unordered_map>

// User/configuration supplied preprocessor macro replacements.
//
// The map is the complete effective `nsf.preprocessorMacros` setting received
// from the client. Bundled macro resources are used only to prefill that user
// setting; they are not implicitly stacked underneath this table at analysis
// time. Numeric `nsf.defines` and source-level #define/#undef still have the
// final word.
using ConfiguredPreprocessorMacros =
    std::unordered_map<std::string, std::string>;
