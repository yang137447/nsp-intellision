#include "workspace_index_internal.hpp"

#include "lsp_helpers.hpp"

#include <fstream>
#include <sstream>
#include <string>

bool readFileToString(const std::string &path, std::string &out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return false;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  out = buffer.str();
  return true;
}

Json serializeFileEntry(const std::string &path, const FileMeta &meta) {
  Json file = makeObject();
  file.o["path"] = makeString(path);
  file.o["mtime"] = makeNumber(static_cast<double>(meta.mtime));
  file.o["size"] = makeNumber(static_cast<double>(meta.size));

  Json defs = makeArray();
  for (const auto &def : meta.defs) {
    Json d = makeObject();
    d.o["name"] = makeString(def.name);
    d.o["type"] = makeString(def.type);
    d.o["uri"] = makeString(def.uri);
    d.o["line"] = makeNumber(def.line);
    d.o["start"] = makeNumber(def.start);
    d.o["end"] = makeNumber(def.end);
    d.o["kind"] = makeNumber(def.kind);
    defs.a.push_back(std::move(d));
  }
  file.o["defs"] = std::move(defs);

  Json structs = makeArray();
  for (const auto &st : meta.structs) {
    Json s = makeObject();
    s.o["name"] = makeString(st.name);
    Json members = makeArray();
    for (const auto &m : st.members) {
      Json mem = makeObject();
      mem.o["name"] = makeString(m.name);
      mem.o["type"] = makeString(m.type);
      members.a.push_back(std::move(mem));
    }
    s.o["members"] = std::move(members);
    structs.a.push_back(std::move(s));
  }
  file.o["structs"] = std::move(structs);

  Json includes = makeArray();
  for (const auto &inc : meta.includes) {
    includes.a.push_back(makeString(inc));
  }
  file.o["includes"] = std::move(includes);

  Json artDefaults = makeArray();
  for (const auto &macro : meta.artDefaultZeroMacros) {
    Json item = makeObject();
    item.o["name"] = makeString(macro.name);
    item.o["artType"] = makeString(macro.artType);
    item.o["uri"] = makeString(macro.uri);
    item.o["line"] = makeNumber(macro.line);
    item.o["start"] = makeNumber(macro.start);
    item.o["end"] = makeNumber(macro.end);
    Json companionConstants = makeArray();
    for (const auto &constant : macro.companionConstants) {
      Json constantItem = makeObject();
      constantItem.o["name"] = makeString(constant.name);
      constantItem.o["value"] = makeNumber(constant.value);
      constantItem.o["uri"] = makeString(constant.uri);
      constantItem.o["line"] = makeNumber(constant.line);
      constantItem.o["start"] = makeNumber(constant.start);
      constantItem.o["end"] = makeNumber(constant.end);
      companionConstants.a.push_back(std::move(constantItem));
    }
    item.o["companionConstants"] = std::move(companionConstants);
    artDefaults.a.push_back(std::move(item));
  }
  file.o["artDefaultZeroMacros"] = std::move(artDefaults);
  return file;
}

bool deserializeFileEntry(const Json &file, std::string &outPath,
                          FileMeta &outMeta) {
  if (file.type != Json::Type::Object)
    return false;
  const Json *pathV = getObjectValue(file, "path");
  if (!pathV || pathV->type != Json::Type::String)
    return false;
  outPath = pathV->s;
  FileMeta meta;
  const Json *mtimeV = getObjectValue(file, "mtime");
  const Json *sizeV = getObjectValue(file, "size");
  if (mtimeV && mtimeV->type == Json::Type::Number)
    meta.mtime = static_cast<uint64_t>(mtimeV->n);
  if (sizeV && sizeV->type == Json::Type::Number)
    meta.size = static_cast<uint64_t>(sizeV->n);

  const Json *defsV = getObjectValue(file, "defs");
  if (defsV && defsV->type == Json::Type::Array) {
    for (const auto &d : defsV->a) {
      if (d.type != Json::Type::Object)
        continue;
      IndexedDefinition def;
      const Json *nameV2 = getObjectValue(d, "name");
      def.name = nameV2 ? getStringValue(*nameV2) : "";
      const Json *typeV2 = getObjectValue(d, "type");
      def.type = typeV2 ? getStringValue(*typeV2) : "";
      const Json *uriV2 = getObjectValue(d, "uri");
      def.uri = uriV2 ? getStringValue(*uriV2) : "";
      const Json *lineV = getObjectValue(d, "line");
      const Json *startV = getObjectValue(d, "start");
      const Json *endV = getObjectValue(d, "end");
      const Json *kindV = getObjectValue(d, "kind");
      def.line = lineV ? static_cast<int>(getNumberValue(*lineV)) : 0;
      def.start = startV ? static_cast<int>(getNumberValue(*startV)) : 0;
      def.end = endV ? static_cast<int>(getNumberValue(*endV)) : def.start;
      def.kind = kindV ? static_cast<int>(getNumberValue(*kindV)) : 0;
      if (!def.name.empty() && !def.uri.empty())
        meta.defs.push_back(std::move(def));
    }
  }

  const Json *structsV = getObjectValue(file, "structs");
  if (structsV && structsV->type == Json::Type::Array) {
    for (const auto &s : structsV->a) {
      if (s.type != Json::Type::Object)
        continue;
      IndexedStruct st;
      const Json *nameV = getObjectValue(s, "name");
      st.name = nameV ? getStringValue(*nameV) : "";
      const Json *membersV = getObjectValue(s, "members");
      if (membersV && membersV->type == Json::Type::Array) {
        for (const auto &m : membersV->a) {
          if (m.type != Json::Type::Object)
            continue;
          IndexedStructMember mem;
          const Json *mn = getObjectValue(m, "name");
          const Json *mt = getObjectValue(m, "type");
          mem.name = mn ? getStringValue(*mn) : "";
          mem.type = mt ? getStringValue(*mt) : "";
          if (!mem.name.empty())
            st.members.push_back(std::move(mem));
        }
      }
      if (!st.name.empty() && !st.members.empty())
        meta.structs.push_back(std::move(st));
    }
  }

  const Json *includesV = getObjectValue(file, "includes");
  if (includesV && includesV->type == Json::Type::Array) {
    for (const auto &inc : includesV->a) {
      if (inc.type != Json::Type::String)
        continue;
      if (!inc.s.empty())
        meta.includes.push_back(inc.s);
    }
  }

  const Json *artDefaultsV = getObjectValue(file, "artDefaultZeroMacros");
  if (artDefaultsV && artDefaultsV->type == Json::Type::Array) {
    for (const auto &item : artDefaultsV->a) {
      if (item.type != Json::Type::Object)
        continue;
      ArtDefaultZeroMacro macro;
      const Json *nameV = getObjectValue(item, "name");
      const Json *typeV = getObjectValue(item, "artType");
      const Json *uriV = getObjectValue(item, "uri");
      const Json *lineV = getObjectValue(item, "line");
      const Json *startV = getObjectValue(item, "start");
      const Json *endV = getObjectValue(item, "end");
      macro.name = nameV ? getStringValue(*nameV) : "";
      macro.artType = typeV ? getStringValue(*typeV) : "";
      macro.uri = uriV ? getStringValue(*uriV) : "";
      macro.line = lineV ? static_cast<int>(getNumberValue(*lineV)) : 0;
      macro.start = startV ? static_cast<int>(getNumberValue(*startV)) : 0;
      macro.end = endV ? static_cast<int>(getNumberValue(*endV)) : macro.start;
      const Json *companionV = getObjectValue(item, "companionConstants");
      if (companionV && companionV->type == Json::Type::Array) {
        for (const auto &constantItem : companionV->a) {
          if (constantItem.type != Json::Type::Object)
            continue;
          ArtCompanionConstant constant;
          const Json *constantNameV = getObjectValue(constantItem, "name");
          const Json *constantValueV = getObjectValue(constantItem, "value");
          const Json *constantUriV = getObjectValue(constantItem, "uri");
          const Json *constantLineV = getObjectValue(constantItem, "line");
          const Json *constantStartV = getObjectValue(constantItem, "start");
          const Json *constantEndV = getObjectValue(constantItem, "end");
          constant.name =
              constantNameV ? getStringValue(*constantNameV) : "";
          constant.value = constantValueV
                               ? static_cast<int>(getNumberValue(
                                     *constantValueV))
                               : 0;
          constant.uri = constantUriV ? getStringValue(*constantUriV) : "";
          constant.line = constantLineV
                              ? static_cast<int>(getNumberValue(
                                    *constantLineV))
                              : 0;
          constant.start = constantStartV
                               ? static_cast<int>(getNumberValue(
                                     *constantStartV))
                               : 0;
          constant.end = constantEndV
                             ? static_cast<int>(getNumberValue(*constantEndV))
                             : constant.start;
          if (!constant.name.empty() && !constant.uri.empty())
            macro.companionConstants.push_back(std::move(constant));
        }
      }
      if (!macro.name.empty() && !macro.uri.empty())
        meta.artDefaultZeroMacros.push_back(std::move(macro));
    }
  }

  outMeta = std::move(meta);
  return true;
}

Json serializeIndex(const IndexStore &store) {
  Json root = makeObject();
  root.o["key"] = makeString(store.key);
  Json files = makeArray();
  for (const auto &pair : store.filesByPath) {
    files.a.push_back(serializeFileEntry(pair.first, pair.second));
  }
  root.o["files"] = std::move(files);
  return root;
}

bool deserializeIndex(const Json &root, IndexStore &store) {
  if (root.type != Json::Type::Object)
    return false;
  const Json *key = getObjectValue(root, "key");
  const Json *files = getObjectValue(root, "files");
  if (!key || key->type != Json::Type::String)
    return false;
  if (!files || files->type != Json::Type::Array)
    return false;
  store.key = key->s;
  store.filesByPath.clear();

  for (const auto &file : files->a) {
    std::string path;
    FileMeta meta;
    if (!deserializeFileEntry(file, path, meta))
      continue;
    const std::string key = normalizePathForCompare(path);
    if (key.empty())
      continue;
    store.filesByPath.emplace(key, std::move(meta));
  }
  rebuildGlobals(store);
  return true;
}
