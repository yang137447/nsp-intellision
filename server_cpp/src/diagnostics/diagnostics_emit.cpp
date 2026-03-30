#include "diagnostics_emit.hpp"

#include "lsp_helpers.hpp"
#include "text_utils.hpp"

#include <string>

Json makeDiagnostic(const std::string &text, int line, int startByte,
                    int endByte, int severity, const std::string &source,
                    const std::string &message) {
  std::string lineText = getLineAt(text, line);
  int start = byteOffsetInLineToUtf16(lineText, startByte);
  int end = byteOffsetInLineToUtf16(lineText, endByte);
  if (end < start)
    end = start;
  Json diag = makeObject();
  diag.o["range"] = makeRangeExact(line, start, end);
  diag.o["severity"] = makeNumber(severity);
  diag.o["source"] = makeString(source);
  diag.o["message"] = makeString(message);
  return diag;
}

Json makeDiagnosticWithCodeAndReason(
    const std::string &text, int line, int startByte, int endByte, int severity,
    const std::string &source, const std::string &message,
    const std::string &code, const std::string &reasonCode) {
  Json diag =
      makeDiagnostic(text, line, startByte, endByte, severity, source, message);
  diag.o["code"] = makeString(code);
  Json data = makeObject();
  data.o["reasonCode"] = makeString(reasonCode);
  diag.o["data"] = data;
  return diag;
}
