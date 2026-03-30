#include "hover_rendering.hpp"

#include "hover_markdown.hpp"

#include <string>

namespace {

void appendDocSections(std::string &markdown, const std::string &leadingDoc,
                       const std::string &inlineDoc) {
  if (!leadingDoc.empty()) {
    markdown += "\n\n";
    markdown += leadingDoc;
  }
  if (!inlineDoc.empty()) {
    markdown += "\n\n";
    markdown += inlineDoc;
  }
}

void appendLocationList(std::string &markdown, const std::string &title,
                        const std::vector<HoverLocationListItem> &items,
                        bool appendEllipsisAfterList) {
  if (items.empty()) {
    return;
  }
  markdown += "\n\n---\n\n";
  markdown += title;
  markdown += " (";
  markdown += std::to_string(items.size());
  markdown += "):";
  for (size_t i = 0; i < items.size(); i++) {
    markdown += "\n";
    markdown += std::to_string(i + 1);
    markdown += ". `";
    markdown += items[i].label;
    markdown += "`";
    if (!items[i].locationDisplay.empty()) {
      markdown += " — ";
      markdown += items[i].locationDisplay;
    }
  }
  if (appendEllipsisAfterList) {
    markdown += "\n...";
  }
}

} // namespace

std::string renderHoverFunctionMarkdown(const HoverFunctionMarkdownInput &input) {
  std::string markdown;
  markdown += formatCppCodeBlock(input.code);
  if (!input.selectionNote.empty()) {
    markdown += "\n\n";
    markdown += input.selectionNote;
  }
  if (!input.kindLabel.empty()) {
    markdown += "\n\n";
    markdown += input.kindLabel;
  }
  if (!input.returnType.empty()) {
    markdown += "\n\nReturns: ";
    markdown += input.returnType;
  }
  if (!input.parameters.empty()) {
    markdown += "\n\nParameters:";
    for (const auto &parameter : input.parameters) {
      markdown += "\n- `";
      markdown += parameter;
      markdown += "`";
    }
  } else if (input.showEmptyParameters) {
    markdown += "\n\nParameters: (none)";
  }
  if (!input.definedAt.empty()) {
    markdown += "\n\nDefined at: ";
    markdown += input.definedAt;
  }
  appendDocSections(markdown, input.leadingDoc, input.inlineDoc);
  appendLocationList(markdown, input.listTitle, input.listItems,
                     input.appendEllipsisAfterList);
  return markdown;
}

std::string renderHoverSymbolMarkdown(const HoverSymbolMarkdownInput &input) {
  std::string markdown;
  markdown += formatCppCodeBlock(input.code);
  for (const auto &note : input.notes) {
    if (note.empty()) {
      continue;
    }
    markdown += "\n\n";
    markdown += note;
  }
  if (!input.typeName.empty()) {
    markdown += "\n\nType: ";
    markdown += input.typeName;
  } else if (!input.indeterminateReason.empty()) {
    markdown += "\n\nIndeterminate reason=";
    markdown += input.indeterminateReason;
  }
  if (!input.definedAt.empty()) {
    markdown += "\n\nDefined at: ";
    markdown += input.definedAt;
  }
  appendDocSections(markdown, input.leadingDoc, input.inlineDoc);
  return markdown;
}

std::string renderHoverMacroMarkdown(const HoverMacroMarkdownInput &input) {
  std::string markdown;
  markdown += formatCppCodeBlock(input.code);
  if (!input.kindLabel.empty()) {
    markdown += "\n\n";
    markdown += input.kindLabel;
  }
  if (!input.definedAt.empty()) {
    markdown += "\n\nDefined at: ";
    markdown += input.definedAt;
  }
  appendDocSections(markdown, input.leadingDoc, input.inlineDoc);
  appendLocationList(markdown, input.listTitle, input.listItems,
                     input.appendEllipsisAfterList);
  return markdown;
}
