#pragma once
#include <string>
#include "json.hpp"

bool readMessage(std::string &payload);
void writeMessage(const std::string &json);
void writeResponse(const Json &id, const Json &result);
void writeError(const Json &id, int code, const std::string &message);
void writeNotification(const std::string &method, const Json &params);
