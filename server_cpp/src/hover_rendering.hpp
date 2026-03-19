#pragma once

#include <string>
#include <vector>

struct HoverLocationListItem {
  std::string label;
  std::string locationDisplay;
};

struct HoverFunctionMarkdownInput {
  std::string code;
  std::string kindLabel;
  std::string returnType;
  std::vector<std::string> parameters;
  std::string definedAt;
  std::string leadingDoc;
  std::string inlineDoc;
  std::string selectionNote;
  std::string listTitle;
  std::vector<HoverLocationListItem> listItems;
  bool showEmptyParameters = true;
  bool appendEllipsisAfterList = false;
};

struct HoverSymbolMarkdownInput {
  std::string code;
  std::vector<std::string> notes;
  std::string typeName;
  std::string indeterminateReason;
  std::string definedAt;
  std::string leadingDoc;
  std::string inlineDoc;
};

struct HoverMacroMarkdownInput {
  std::string code;
  std::string definedAt;
  std::string leadingDoc;
  std::string inlineDoc;
  std::string listTitle;
  std::vector<HoverLocationListItem> listItems;
  bool appendEllipsisAfterList = false;
};

std::string renderHoverFunctionMarkdown(const HoverFunctionMarkdownInput &input);

std::string renderHoverSymbolMarkdown(const HoverSymbolMarkdownInput &input);

std::string renderHoverMacroMarkdown(const HoverMacroMarkdownInput &input);
