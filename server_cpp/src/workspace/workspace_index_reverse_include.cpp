#include "workspace_index_internal.hpp"


#include "active_unit.hpp"
#include "executable_path.hpp"
#include "include_resolver.hpp"
#include "json.hpp"
#include "lsp_helpers.hpp"
#include "lsp_io.hpp"
#include "nsf_lexer.hpp"
#include "server_parse.hpp"
#include "text_utils.hpp"
#include "uri_utils.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace fs = std::filesystem;


void rebuildGlobals(IndexStore &store) {
  store.defsBySymbol.clear();
  store.structMembersOrdered.clear();
  store.bestDefBySymbol.clear();
  store.bestStructDefByName.clear();
  store.bestTypeBySymbol.clear();
  store.structMemberType.clear();

  const std::string unitPathRaw = getActiveUnitPath();
  const std::string unitPath = normalizePathForCompare(unitPathRaw);
  const std::string unitDir = normalizePathForCompare(
      unitPathRaw.empty() ? std::string()
                          : fs::path(unitPathRaw).parent_path().string());

  std::vector<std::string> orderedFiles;
  orderedFiles.reserve(store.filesByPath.size());
  for (const auto &filePair : store.filesByPath)
    orderedFiles.push_back(filePair.first);

  std::sort(orderedFiles.begin(), orderedFiles.end(),
            [&](const std::string &a, const std::string &b) {
              const std::string an = normalizePathForCompare(a);
              const std::string bn = normalizePathForCompare(b);
              auto score = [&](const std::string &p) -> int {
                if (!unitPath.empty() && p == unitPath)
                  return 0;
                if (!unitDir.empty() && isPathUnderOrEqual(unitDir, p))
                  return 1;
                return 2;
              };
              const int sa = score(an);
              const int sb = score(bn);
              if (sa != sb)
                return sa < sb;
              return an < bn;
            });

  for (const auto &pathKey : orderedFiles) {
    const auto itFile = store.filesByPath.find(pathKey);
    if (itFile == store.filesByPath.end())
      continue;
    const auto &meta = itFile->second;
    for (const auto &def : meta.defs) {
      store.defsBySymbol[def.name].push_back(def);
      DefinitionLocation loc{def.uri, def.line, def.start, def.end};
      if (def.kind == 23) {
        auto it = store.bestStructDefByName.find(def.name);
        if (it == store.bestStructDefByName.end())
          store.bestStructDefByName.emplace(def.name, loc);
      }
      auto it = store.bestDefBySymbol.find(def.name);
      if (it == store.bestDefBySymbol.end())
        store.bestDefBySymbol.emplace(def.name, loc);
      if (!def.type.empty()) {
        auto tit = store.bestTypeBySymbol.find(def.name);
        if (tit == store.bestTypeBySymbol.end())
          store.bestTypeBySymbol.emplace(def.name, def.type);
      }
    }
    for (const auto &st : meta.structs) {
      auto itOrdered = store.structMembersOrdered.find(st.name);
      if (itOrdered == store.structMembersOrdered.end()) {
        std::vector<IndexedStructMember> ordered;
        ordered.reserve(st.members.size());
        for (const auto &m : st.members) {
          if (!m.name.empty())
            ordered.push_back(m);
        }
        if (!ordered.empty())
          store.structMembersOrdered.emplace(st.name, std::move(ordered));
      }
      auto &memberMap = store.structMemberType[st.name];
      for (const auto &m : st.members) {
        if (!m.name.empty() && !m.type.empty())
          memberMap.emplace(m.name, m.type);
      }
    }
  }
}

void buildReverseIncludes(
    const IndexStore &store,
    std::unordered_map<std::string, std::vector<std::string>> &outReverse) {
  outReverse.clear();
  for (const auto &pair : store.filesByPath) {
    const std::string includerKey = normalizePathForCompare(pair.first);
    for (const auto &dep : pair.second.includes) {
      if (dep.empty())
        continue;
      outReverse[dep].push_back(includerKey);
    }
  }
}


