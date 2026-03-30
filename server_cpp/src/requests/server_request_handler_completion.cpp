#include "server_request_handler_completion.hpp"

#include "callsite_parser.hpp"
#include "completion_rendering.hpp"
#include "hlsl_builtin_docs.hpp"
#include "interactive_semantic_runtime.hpp"
#include "language_registry.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "member_query.hpp"
#include "nsf_lexer.hpp"
#include "text_utils.hpp"
#include "type_model.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

static const std::vector<std::string> &getHlslScalarVectorMatrixTypeNames() {
  static const std::vector<std::string> names = []() {
    const std::vector<std::string> scalarTypes = {
        "void",       "bool",       "int",       "uint",      "dword",
        "half",       "float",      "double",    "min16float","min10float",
        "min16int",   "min12int",   "min16uint",
    };
    const std::vector<std::string> numericBases = {
        "bool",       "int",        "uint",      "half",      "float",
        "double",     "min16float", "min10float","min16int",  "min12int",
        "min16uint",
    };

    std::vector<std::string> generated = scalarTypes;
    generated.reserve(scalarTypes.size() + numericBases.size() * 12);
    for (const auto &base : numericBases) {
      for (int dim = 2; dim <= 4; ++dim)
        generated.push_back(base + std::to_string(dim));
      for (int rows = 2; rows <= 4; ++rows) {
        for (int cols = 2; cols <= 4; ++cols)
          generated.push_back(base + std::to_string(rows) + "x" +
                              std::to_string(cols));
      }
    }
    return generated;
  }();
  return names;
}

static std::string extractCompletionPrefix(const std::string &lineText,
                                           int cursorCharacter) {
  if (cursorCharacter < 0)
    return std::string();
  size_t cursor =
      static_cast<size_t>(utf16ToByteOffsetInLine(lineText, cursorCharacter));
  if (cursor > lineText.size())
    cursor = lineText.size();
  size_t start = cursor;
  while (start > 0 && isIdentifierChar(lineText[start - 1]))
    start--;
  return lineText.substr(start, cursor - start);
}

bool request_completion_handlers::handleCompletionRequest(
    const std::string &method, const Json &id, const Json *params,
    ServerRequestContext &ctx, const std::vector<std::string> &keywords,
    const std::vector<std::string> &directives) {
  if (method != "textDocument/completion" || !params)
    return false;

  const Json *textDocument = getObjectValue(*params, "textDocument");
  const Json *position = getObjectValue(*params, "position");
  std::string uri;
  int line = -1;
  int character = -1;
  const Document *doc = nullptr;
  if (textDocument && position) {
    const Json *uriValue = getObjectValue(*textDocument, "uri");
    const Json *lineValue = getObjectValue(*position, "line");
    const Json *charValue = getObjectValue(*position, "character");
    if (uriValue && lineValue && charValue) {
      uri = getStringValue(*uriValue);
      line = static_cast<int>(getNumberValue(*lineValue));
      character = static_cast<int>(getNumberValue(*charValue));
      doc = ctx.findDocument(uri);
    }
  }

  static const std::vector<std::string> attributeKeywords = {
      "unroll", "loop",    "fastopt", "allow_uav_condition",
      "branch", "flatten", "call"};
  std::vector<InteractiveCompletionItem> interactiveItems;

  auto isAttributeCompletionContext = [&](const std::string &lineText,
                                          int cursorChar) {
    if (cursorChar < 0)
      return false;
    size_t cursor =
        static_cast<size_t>(utf16ToByteOffsetInLine(lineText, cursorChar));
    if (cursor > lineText.size())
      cursor = lineText.size();
    size_t open = lineText.rfind('[', cursor == 0 ? 0 : cursor - 1);
    if (open == std::string::npos)
      return false;
    size_t close = lineText.find(']', open);
    if (close != std::string::npos && close < cursor)
      return false;
    size_t firstNonSpace = 0;
    while (firstNonSpace < lineText.size() &&
           std::isspace(static_cast<unsigned char>(lineText[firstNonSpace]))) {
      firstNonSpace++;
    }
    if (firstNonSpace >= lineText.size())
      return false;
    if (firstNonSpace == open)
      return true;
    size_t prev = open;
    while (prev > 0) {
      prev--;
      if (!std::isspace(static_cast<unsigned char>(lineText[prev])))
        break;
    }
    char prevCh = prev < lineText.size() ? lineText[prev] : '\0';
    return prevCh == '{' || prevCh == ';';
  };

  if (doc && line >= 0 && character >= 0) {
    std::string lineText = getLineAt(doc->text, line);
    if (isAttributeCompletionContext(lineText, character)) {
      Json items = makeArray();
      appendCompletionItems(items, attributeKeywords, 14);
      writeResponse(id, items);
      return true;
    }
    size_t cursorOffset = positionToOffsetUtf16(doc->text, line, character);
    std::string base;
    std::string member;
    if (extractMemberAccessAtOffset(doc->text, cursorOffset, base, member)) {
      MemberAccessBaseTypeOptions baseOptions;
      MemberAccessBaseTypeResult baseResolution =
          resolveMemberAccessBaseType(uri, *doc, base, cursorOffset, ctx,
                                      baseOptions);
      if (baseResolution.resolved) {
        MemberCompletionQuery query;
        if (collectMemberCompletionQuery(uri, baseResolution.typeName, ctx,
                                         query)) {
          Json items = buildMemberCompletionItems(query);
          if (!items.a.empty()) {
            writeResponse(id, items);
            return true;
          }
        }
      }
    }

    interactiveCollectCompletionItems(uri, *doc, cursorOffset,
                                      extractCompletionPrefix(lineText, character),
                                      ctx, interactiveItems);
  }

  Json items = makeArray();
  std::unordered_set<std::string> seen;
  auto appendUniqueItems = [&](const std::vector<std::string> &labels,
                               int kind) {
    for (const auto &label : labels) {
      if (!label.empty() && label[0] == '#') {
        appendUniqueCompletionItem(items, seen, label, kind, std::string(),
                                   label.substr(1));
        continue;
      }
      appendUniqueCompletionItem(items, seen, label, kind);
    }
  };
  for (const auto &item : interactiveItems) {
    appendUniqueCompletionItem(items, seen, item.label, item.kind, item.detail);
  }
  appendUniqueItems(getHlslDirectiveCompletionItems(), 14);
  appendUniqueItems(getHlslKeywordNames(), 14);
  appendUniqueItems(getHlslScalarVectorMatrixTypeNames(), 7);
  appendUniqueItems(getTypeModelObjectTypeNames(), 7);
  appendUniqueItems(getHlslSystemSemanticNames(), 13);
  appendUniqueItems(getHlslBuiltinNames(), 3);
  if (doc && line >= 0 && character >= 0) {
    std::string lineText = getLineAt(doc->text, line);
    if (isAttributeCompletionContext(lineText, character)) {
      appendUniqueItems(attributeKeywords, 14);
    }
  }
  writeResponse(id, items);
  return true;
}

