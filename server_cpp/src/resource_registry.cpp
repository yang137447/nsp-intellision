#include "resource_registry.hpp"

#include "executable_path.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

static std::filesystem::path bundleDirFromKey(const std::string &bundleKey) {
  std::filesystem::path bundleDir = getExecutableDir() / "resources";
  std::string segment;
  for (char ch : bundleKey) {
    if (ch == '/' || ch == '\\') {
      if (!segment.empty()) {
        bundleDir /= segment;
        segment.clear();
      }
      continue;
    }
    segment.push_back(ch);
  }
  if (!segment.empty())
    bundleDir /= segment;
  return bundleDir;
}

static bool readFileToString(const std::filesystem::path &path,
                             std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

static bool loadJsonFromPath(const std::filesystem::path &path, Json &root,
                             std::string &errorOut) {
  std::string text;
  if (!readFileToString(path, text)) {
    errorOut = "read failed: " + path.string();
    return false;
  }
  if (!parseJson(text, root)) {
    errorOut = "parse failed: " + path.string();
    return false;
  }
  return true;
}

static std::string describeJsonType(const Json &value) {
  switch (value.type) {
  case Json::Type::Null:
    return "null";
  case Json::Type::Bool:
    return "boolean";
  case Json::Type::Number:
    return "number";
  case Json::Type::String:
    return "string";
  case Json::Type::Array:
    return "array";
  case Json::Type::Object:
    return "object";
  }
  return "unknown";
}

static bool jsonValuesEqual(const Json &left, const Json &right) {
  if (left.type != right.type)
    return false;
  switch (left.type) {
  case Json::Type::Null:
    return true;
  case Json::Type::Bool:
    return left.b == right.b;
  case Json::Type::Number:
    return left.n == right.n;
  case Json::Type::String:
    return left.s == right.s;
  case Json::Type::Array:
    if (left.a.size() != right.a.size())
      return false;
    for (size_t i = 0; i < left.a.size(); ++i) {
      if (!jsonValuesEqual(left.a[i], right.a[i]))
        return false;
    }
    return true;
  case Json::Type::Object:
    if (left.o.size() != right.o.size())
      return false;
    for (const auto &entry : left.o) {
      auto it = right.o.find(entry.first);
      if (it == right.o.end() || !jsonValuesEqual(entry.second, it->second))
        return false;
    }
    return true;
  }
  return false;
}

static bool jsonTypeMatches(const Json &value, const std::string &expectedType) {
  if (expectedType == "object")
    return value.type == Json::Type::Object;
  if (expectedType == "array")
    return value.type == Json::Type::Array;
  if (expectedType == "string")
    return value.type == Json::Type::String;
  if (expectedType == "number")
    return value.type == Json::Type::Number;
  if (expectedType == "boolean")
    return value.type == Json::Type::Bool;
  if (expectedType == "integer") {
    return value.type == Json::Type::Number &&
           std::floor(value.n) == value.n;
  }
  if (expectedType == "null")
    return value.type == Json::Type::Null;
  return false;
}

static bool validateJsonAgainstSchema(const Json &value, const Json &schema,
                                      const std::string &currentPath,
                                      std::string &errorOut) {
  if (schema.type != Json::Type::Object) {
    errorOut = currentPath + ": schema must be an object";
    return false;
  }

  const Json *typeValue = getObjectValue(schema, "type");
  if (typeValue) {
    bool typeOk = false;
    if (typeValue->type == Json::Type::String) {
      typeOk = jsonTypeMatches(value, typeValue->s);
      if (!typeOk) {
        errorOut = currentPath + ": expected " + typeValue->s + ", got " +
                   describeJsonType(value);
        return false;
      }
    } else if (typeValue->type == Json::Type::Array) {
      std::string expected;
      for (size_t i = 0; i < typeValue->a.size(); ++i) {
        const Json &candidate = typeValue->a[i];
        if (candidate.type != Json::Type::String) {
          errorOut = currentPath + ": schema type array must contain strings";
          return false;
        }
        if (i > 0)
          expected += ", ";
        expected += candidate.s;
        if (jsonTypeMatches(value, candidate.s))
          typeOk = true;
      }
      if (!typeOk) {
        errorOut = currentPath + ": expected one of [" + expected +
                   "], got " + describeJsonType(value);
        return false;
      }
    } else {
      errorOut = currentPath + ": schema type must be string or array";
      return false;
    }
  }

  const Json *enumValue = getObjectValue(schema, "enum");
  if (enumValue) {
    if (enumValue->type != Json::Type::Array) {
      errorOut = currentPath + ": schema enum must be an array";
      return false;
    }
    bool found = false;
    for (const auto &candidate : enumValue->a) {
      if (jsonValuesEqual(value, candidate)) {
        found = true;
        break;
      }
    }
    if (!found) {
      errorOut = currentPath + ": value not in enum";
      return false;
    }
  }

  const Json *constValue = getObjectValue(schema, "const");
  if (constValue && !jsonValuesEqual(value, *constValue)) {
    errorOut = currentPath + ": value must equal " + serializeJson(*constValue);
    return false;
  }

  if (value.type == Json::Type::String) {
    const Json *minLengthValue = getObjectValue(schema, "minLength");
    if (minLengthValue) {
      if (minLengthValue->type != Json::Type::Number ||
          minLengthValue->n < 0.0 ||
          std::floor(minLengthValue->n) != minLengthValue->n) {
        errorOut = currentPath + ": schema minLength must be a non-negative integer";
        return false;
      }
      const size_t minLength = static_cast<size_t>(minLengthValue->n);
      if (value.s.size() < minLength) {
        errorOut = currentPath + ": string length < " +
                   std::to_string(minLength);
        return false;
      }
    }
  }

  if (value.type == Json::Type::Array) {
    const Json *minItemsValue = getObjectValue(schema, "minItems");
    if (minItemsValue) {
      if (minItemsValue->type != Json::Type::Number || minItemsValue->n < 0.0 ||
          std::floor(minItemsValue->n) != minItemsValue->n) {
        errorOut = currentPath + ": schema minItems must be a non-negative integer";
        return false;
      }
      const size_t minItems = static_cast<size_t>(minItemsValue->n);
      if (value.a.size() < minItems) {
        errorOut = currentPath + ": array length < " +
                   std::to_string(minItems);
        return false;
      }
    }

    const Json *itemsValue = getObjectValue(schema, "items");
    if (itemsValue) {
      for (size_t i = 0; i < value.a.size(); ++i) {
        if (!validateJsonAgainstSchema(value.a[i], *itemsValue,
                                       currentPath + "[" + std::to_string(i) +
                                           "]",
                                       errorOut)) {
          return false;
        }
      }
    }
  }

  if (value.type == Json::Type::Object) {
    const Json *requiredValue = getObjectValue(schema, "required");
    if (requiredValue) {
      if (requiredValue->type != Json::Type::Array) {
        errorOut = currentPath + ": schema required must be an array";
        return false;
      }
      for (const auto &requiredEntry : requiredValue->a) {
        if (requiredEntry.type != Json::Type::String) {
          errorOut = currentPath + ": schema required entries must be strings";
          return false;
        }
        if (value.o.find(requiredEntry.s) == value.o.end()) {
          errorOut = currentPath + ": missing required property \"" +
                     requiredEntry.s + "\"";
          return false;
        }
      }
    }

    const Json *propertiesValue = getObjectValue(schema, "properties");
    if (propertiesValue && propertiesValue->type != Json::Type::Object) {
      errorOut = currentPath + ": schema properties must be an object";
      return false;
    }

    if (propertiesValue) {
      for (const auto &propertySchema : propertiesValue->o) {
        auto it = value.o.find(propertySchema.first);
        if (it == value.o.end())
          continue;
        if (!validateJsonAgainstSchema(
                it->second, propertySchema.second,
                currentPath + "." + propertySchema.first, errorOut)) {
          return false;
        }
      }
    }

    const Json *additionalPropertiesValue =
        getObjectValue(schema, "additionalProperties");
    if (additionalPropertiesValue) {
      if (additionalPropertiesValue->type != Json::Type::Bool) {
        errorOut = currentPath +
                   ": schema additionalProperties must be a boolean";
        return false;
      }
      if (!additionalPropertiesValue->b) {
        for (const auto &entry : value.o) {
          if (!propertiesValue ||
              propertiesValue->o.find(entry.first) == propertiesValue->o.end()) {
            errorOut = currentPath + ": unexpected property \"" + entry.first +
                       "\"";
            return false;
          }
        }
      }
    }
  }

  return true;
}

static bool validateResourceJson(const std::filesystem::path &dataPath,
                                 const Json &root, const Json &schemaRoot,
                                 std::string &errorOut) {
  std::string validationError;
  if (!validateJsonAgainstSchema(root, schemaRoot, "$", validationError)) {
    errorOut = "schema validation failed: " + dataPath.string() + ": " +
               validationError;
    return false;
  }
  return true;
}

} // namespace

ResourceBundlePaths resolveResourceBundlePaths(const std::string &bundleKey) {
  const std::filesystem::path bundleDir = bundleDirFromKey(bundleKey);
  ResourceBundlePaths paths;
  paths.basePath = bundleDir / "base.json";
  paths.overridePath = bundleDir / "override.json";
  paths.schemaPath = bundleDir / "schema.json";
  return paths;
}

bool loadResourceBundleJson(const std::string &bundleKey, Json &baseRoot,
                            Json &overrideRoot, std::string &errorOut) {
  const ResourceBundlePaths paths = resolveResourceBundlePaths(bundleKey);
  Json schemaRoot;
  if (!std::filesystem::exists(paths.schemaPath)) {
    errorOut = "missing schema: " + paths.schemaPath.string();
    return false;
  }
  if (!loadJsonFromPath(paths.schemaPath, schemaRoot, errorOut))
    return false;
  if (!loadJsonFromPath(paths.basePath, baseRoot, errorOut))
    return false;
  if (!validateResourceJson(paths.basePath, baseRoot, schemaRoot, errorOut))
    return false;
  if (!loadJsonFromPath(paths.overridePath, overrideRoot, errorOut))
    return false;
  if (!validateResourceJson(paths.overridePath, overrideRoot, schemaRoot,
                            errorOut)) {
    return false;
  }
  errorOut.clear();
  return true;
}
