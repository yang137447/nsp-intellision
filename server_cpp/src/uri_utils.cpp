#include "uri_utils.hpp"
#include <algorithm>
#include <cctype>

std::string uriDecode(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];
    if (c == '%' && i + 2 < text.size()) {
      char h1 = text[i + 1];
      char h2 = text[i + 2];
      auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9')
          return h - '0';
        if (h >= 'a' && h <= 'f')
          return h - 'a' + 10;
        if (h >= 'A' && h <= 'F')
          return h - 'A' + 10;
        return 0;
      };
      int value = (hex(h1) << 4) | hex(h2);
      out.push_back(static_cast<char>(value));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string uriToPath(const std::string &uri) {
  if (uri.rfind("file://", 0) != 0)
    return "";
  std::string path = uri.substr(7);
  if (path.rfind("/", 0) == 0)
    path = path.substr(1);
  path = uriDecode(path);
  std::replace(path.begin(), path.end(), '/', '\\');
  return path;
}

std::string pathToUri(const std::string &path) {
  std::string generic = path;
  std::replace(generic.begin(), generic.end(), '\\', '/');
  if (generic.size() >= 2 && generic[1] == ':') {
    generic[0] = static_cast<char>(std::tolower(generic[0]));
    return "file:///" + generic;
  }
  if (!generic.empty() && generic[0] != '/') {
    generic = "/" + generic;
  }
  return "file://" + generic;
}
