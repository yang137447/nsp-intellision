#pragma once

#include "definition_location.hpp"
#include "server_documents.hpp"

#include <cstddef>
#include <string>
#include <vector>

struct ServerRequestContext;

// Resolves the type of a member-access base expression such as `value` in
// `value.member`.
struct MemberAccessBaseTypeOptions {
  bool includeWorkspaceIndexFallback = false;
};

struct MemberAccessBaseTypeResult {
  std::string typeName;
  bool resolved = false;
};

MemberAccessBaseTypeResult resolveMemberAccessBaseType(
    const std::string &uri, const Document &doc, const std::string &base,
    size_t cursorOffset, const ServerRequestContext &ctx,
    const MemberAccessBaseTypeOptions &options);

// Hover payload for one resolved struct field/member.
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

// Consumer-ready struct field entry used by member completion rendering.
//
// `type` is empty only when the source layer could not determine one; normal
// struct-field completions should prefer filling it.
struct MemberCompletionField {
  std::string name;
  std::string type;
};

// Completion payload for `base.` style member access.
//
// `fields` contains struct-field candidates in display order. `methods`
// contains callable object/builtin methods for the resolved owner type.
struct MemberCompletionQuery {
  std::string ownerType;
  std::vector<MemberCompletionField> fields;
  std::vector<std::string> methods;
};

// Collects consumer-ready member completion data for one resolved owner type.
bool collectMemberCompletionQuery(const std::string &uri,
                                  const std::string &ownerType,
                                  ServerRequestContext &ctx,
                                  MemberCompletionQuery &out);
