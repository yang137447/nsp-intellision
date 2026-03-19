#pragma once

#include <string>

void installCrashHandler(const std::string& logPath);
void logCrashMessage(const std::string& msg);
void waitForDebugger();
