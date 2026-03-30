#pragma once

#include "json.hpp"

#include <string>

void collectBracketDiagnostics(const std::string &text, Json &diags);

void collectPreprocessorDiagnostics(const std::string &text, Json &diags);
