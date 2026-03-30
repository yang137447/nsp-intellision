#pragma once

#include "json.hpp"

#include <string>

Json makeDiagnostic(const std::string &text, int line, int startByte,
                    int endByte, int severity, const std::string &source,
                    const std::string &message);

Json makeDiagnosticWithCodeAndReason(
    const std::string &text, int line, int startByte, int endByte, int severity,
    const std::string &source, const std::string &message,
    const std::string &code, const std::string &reasonCode);
