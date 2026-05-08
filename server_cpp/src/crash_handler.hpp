#pragma once

#include <string>

// Process-wide crash handling for the C++ LSP server.
//
// installCrashHandler registers SEH/signal/terminate hooks and records the
// target crash log path. It must not create the log file during normal startup;
// users should only see crash artifacts after a real crash path writes through
// logCrashMessage.
void installCrashHandler(const std::string &logPath);

// Appends a single line to the configured crash log. Callers should reserve this
// for abnormal paths or explicit debug waits so normal server startup stays
// silent in user workspaces.
void logCrashMessage(const std::string &msg);
void waitForDebugger();
