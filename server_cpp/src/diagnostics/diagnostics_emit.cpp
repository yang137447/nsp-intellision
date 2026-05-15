#include "diagnostics_emit.hpp"

#include "lsp_helpers.hpp"
#include "text_utils.hpp"

#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::string diagnosticStringField(const Json &diagnostic,
                                  const std::string &field) {
  const Json *value = getObjectValue(diagnostic, field);
  if (!value)
    return "";
  if (value->type == Json::Type::String)
    return value->s;
  if (field == "code" && value->type != Json::Type::Null)
    return serializeJson(*value);
  return "";
}

int diagnosticPositionNumber(const Json &diagnostic,
                             const std::string &positionName,
                             const std::string &field) {
  const Json *range = getObjectValue(diagnostic, "range");
  if (!range || range->type != Json::Type::Object)
    return -1;
  const Json *position = getObjectValue(*range, positionName);
  if (!position || position->type != Json::Type::Object)
    return -1;
  const Json *value = getObjectValue(*position, field);
  if (!value || value->type != Json::Type::Number)
    return -1;
  return static_cast<int>(value->n);
}

std::string diagnosticDedupeKey(const std::string &uri,
                                const Json &diagnostic) {
  std::ostringstream key;
  key << uri << '|'
      << diagnosticPositionNumber(diagnostic, "start", "line") << ':'
      << diagnosticPositionNumber(diagnostic, "start", "character") << '-'
      << diagnosticPositionNumber(diagnostic, "end", "line") << ':'
      << diagnosticPositionNumber(diagnostic, "end", "character") << '|'
      << diagnosticStringField(diagnostic, "message") << '|'
      << diagnosticStringField(diagnostic, "code") << '|'
      << diagnosticStringField(diagnostic, "source");
  return key.str();
}

} // namespace

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

void dedupeDiagnosticsForUri(const std::string &uri, Json &diagnostics) {
  if (diagnostics.type != Json::Type::Array)
    return;
  std::unordered_set<std::string> seen;
  seen.reserve(diagnostics.a.size());
  std::vector<Json> deduped;
  deduped.reserve(diagnostics.a.size());
  for (const auto &diagnostic : diagnostics.a) {
    const std::string key = diagnosticDedupeKey(uri, diagnostic);
    if (seen.insert(key).second)
      deduped.push_back(diagnostic);
  }
  diagnostics.a = std::move(deduped);
}
