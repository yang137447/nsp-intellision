#include "completion_rendering.hpp"

#include "lsp_helpers.hpp"

Json makeCompletionItem(const std::string &label, int kind,
                        const std::string &detail) {
  Json item = makeObject();
  item.o["label"] = makeString(label);
  item.o["kind"] = makeNumber(kind);
  if (!detail.empty()) {
    item.o["detail"] = makeString(detail);
  }
  return item;
}

void appendCompletionItem(Json &items, const std::string &label, int kind,
                          const std::string &detail) {
  items.a.push_back(makeCompletionItem(label, kind, detail));
}

void appendUniqueCompletionItem(Json &items,
                                std::unordered_set<std::string> &seen,
                                const std::string &label, int kind,
                                const std::string &detail) {
  if (!seen.insert(label).second) {
    return;
  }
  appendCompletionItem(items, label, kind, detail);
}

void appendCompletionItems(Json &items, const std::vector<std::string> &labels,
                           int kind) {
  for (const auto &label : labels) {
    appendCompletionItem(items, label, kind);
  }
}

Json buildMemberCompletionItems(const MemberCompletionQuery &query) {
  Json items = makeArray();
  std::unordered_set<std::string> seen;
  for (const auto &field : query.fields) {
    appendUniqueCompletionItem(items, seen, field, 5, query.ownerType);
  }
  for (const auto &method : query.methods) {
    appendUniqueCompletionItem(items, seen, method, 2, query.ownerType);
  }
  return items;
}
