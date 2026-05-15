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

// Removes repeated diagnostics from one document payload using the LSP-visible
// identity: document uri, range, message, code, and source.
void dedupeDiagnosticsForUri(const std::string &uri, Json &diagnostics);
