#include "lsp_io.hpp"
#include <iostream>
#include <mutex>
#include <sstream>

static std::mutex gWriteMutex;

bool readMessage(std::string &payload) {
  size_t contentLength = 0;
  bool hasLength = false;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty()) {
      if (hasLength)
        break;
      continue;
    }
    if (line.rfind("Content-Length:", 0) == 0) {
      std::string value = line.substr(15);
      contentLength = static_cast<size_t>(std::stoul(value));
      hasLength = true;
    }
  }
  if (!hasLength) {
    return false;
  }
  payload.assign(contentLength, '\0');
  std::cin.read(&payload[0], static_cast<std::streamsize>(contentLength));
  return std::cin.good();
}

void writeMessage(const std::string &json) {
  std::lock_guard<std::mutex> lock(gWriteMutex);
  std::ostringstream out;
  out << "Content-Length: " << json.size() << "\r\n\r\n" << json;
  std::cout << out.str();
  std::cout.flush();
}

void writeResponse(const Json &id, const Json &result) {
  Json response;
  response.type = Json::Type::Object;
  response.o["jsonrpc"] = Json{Json::Type::String, false, 0.0, "2.0", {}, {}};
  response.o["id"] = id;
  response.o["result"] = result;
  writeMessage(serializeJson(response));
}

void writeError(const Json &id, int code, const std::string &message) {
  Json error;
  error.type = Json::Type::Object;
  error.o["code"] = Json{Json::Type::Number, false, static_cast<double>(code), "", {}, {}};
  error.o["message"] = Json{Json::Type::String, false, 0.0, message, {}, {}};
  Json response;
  response.type = Json::Type::Object;
  response.o["jsonrpc"] = Json{Json::Type::String, false, 0.0, "2.0", {}, {}};
  response.o["id"] = id;
  response.o["error"] = error;
  writeMessage(serializeJson(response));
}

void writeNotification(const std::string &method, const Json &params) {
  Json message;
  message.type = Json::Type::Object;
  message.o["jsonrpc"] = Json{Json::Type::String, false, 0.0, "2.0", {}, {}};
  message.o["method"] = Json{Json::Type::String, false, 0.0, method, {}, {}};
  message.o["params"] = params;
  writeMessage(serializeJson(message));
}
