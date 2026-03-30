#pragma once

#include "json.hpp"
#include "member_query.hpp"

#include <string>
#include <unordered_set>
#include <vector>

Json makeCompletionItem(const std::string &label, int kind,
                        const std::string &detail = std::string(),
                        const std::string &filterText = std::string());

void appendCompletionItem(Json &items, const std::string &label, int kind,
                          const std::string &detail = std::string(),
                          const std::string &filterText = std::string());

void appendUniqueCompletionItem(Json &items,
                                std::unordered_set<std::string> &seen,
                                const std::string &label, int kind,
                                const std::string &detail = std::string(),
                                const std::string &filterText = std::string());

void appendCompletionItems(Json &items, const std::vector<std::string> &labels,
                           int kind);

Json buildMemberCompletionItems(const MemberCompletionQuery &query);
