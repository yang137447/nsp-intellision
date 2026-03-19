#pragma once

#include "definition_location.hpp"
#include "server_documents.hpp"

#include <cstddef>
#include <string>
#include <vector>

struct ServerRequestContext;

struct MemberAccessBaseTypeOptions {
  bool includeWorkspaceIndexFallback = false;
  bool includeIncludeGraphFallback = false;
};

struct MemberAccessBaseTypeResult {
  std::string typeName;
  bool resolved = false;
};

MemberAccessBaseTypeResult resolveMemberAccessBaseType(
    const std::string &uri, const Document &doc, const std::string &base,
    size_t cursorOffset, const ServerRequestContext &ctx,
    const MemberAccessBaseTypeOptions &options);

struct MemberHoverInfo {
  std::string memberType;
  DefinitionLocation ownerStructLocation;
  bool hasStructLocation = false;
  std::string memberLeadingDoc;
  std::string memberInlineDoc;
  bool found = false;
};

bool resolveMemberHoverInfo(const std::string &uri,
                            const std::string &ownerType,
                            const std::string &memberName,
                            ServerRequestContext &ctx,
                            MemberHoverInfo &out);

struct MemberCompletionQuery {
  std::string ownerType;
  std::vector<std::string> fields;
  std::vector<std::string> methods;
};

bool collectMemberCompletionQuery(const std::string &uri,
                                  const std::string &ownerType,
                                  ServerRequestContext &ctx,
                                  MemberCompletionQuery &out);
