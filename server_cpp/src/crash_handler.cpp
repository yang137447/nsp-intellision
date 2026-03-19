#include "crash_handler.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

static std::string gCrashLogPath;
static std::recursive_mutex gLogMutex;

#ifdef _WIN32
static void logStackTrace(const char *tag) {
  HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
  if (!dbghelp) {
    logCrashMessage(
        "CRASH: stacktrace unavailable (failed to load dbghelp.dll)");
    return;
  }

  typedef struct SYMBOL_INFO {
    ULONG SizeOfStruct;
    ULONG TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index;
    ULONG Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG Register;
    ULONG Scope;
    ULONG Tag;
    ULONG NameLen;
    ULONG MaxNameLen;
    char Name[1];
  } SYMBOL_INFO, *PSYMBOL_INFO;

  typedef struct IMAGEHLP_LINE64 {
    DWORD SizeOfStruct;
    void *Key;
    DWORD LineNumber;
    char *FileName;
    DWORD64 Address;
  } IMAGEHLP_LINE64, *PIMAGEHLP_LINE64;

  using SymInitializeFn = BOOL(WINAPI *)(HANDLE, PCSTR, BOOL);
  using SymCleanupFn = BOOL(WINAPI *)(HANDLE);
  using SymSetOptionsFn = DWORD(WINAPI *)(DWORD);
  using SymFromAddrFn =
      BOOL(WINAPI *)(HANDLE, DWORD64, DWORD64 *, PSYMBOL_INFO);
  using SymGetLineFromAddr64Fn =
      BOOL(WINAPI *)(HANDLE, DWORD64, DWORD *, PIMAGEHLP_LINE64);

  auto pSymInitialize = reinterpret_cast<SymInitializeFn>(
      GetProcAddress(dbghelp, "SymInitialize"));
  auto pSymCleanup =
      reinterpret_cast<SymCleanupFn>(GetProcAddress(dbghelp, "SymCleanup"));
  auto pSymSetOptions = reinterpret_cast<SymSetOptionsFn>(
      GetProcAddress(dbghelp, "SymSetOptions"));
  auto pSymFromAddr =
      reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr"));
  auto pSymGetLineFromAddr64 = reinterpret_cast<SymGetLineFromAddr64Fn>(
      GetProcAddress(dbghelp, "SymGetLineFromAddr64"));

  if (!pSymInitialize || !pSymCleanup || !pSymSetOptions || !pSymFromAddr) {
    logCrashMessage("CRASH: stacktrace unavailable (dbghelp exports missing)");
    FreeLibrary(dbghelp);
    return;
  }

  constexpr DWORD SYMOPT_UNDNAME = 0x00000002;
  constexpr DWORD SYMOPT_DEFERRED_LOADS = 0x00000004;
  constexpr DWORD SYMOPT_LOAD_LINES = 0x00000010;
  constexpr DWORD SYMOPT_FAIL_CRITICAL_ERRORS = 0x00000200;

  const HANDLE process = GetCurrentProcess();
  pSymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                 SYMOPT_FAIL_CRITICAL_ERRORS);

  if (!pSymInitialize(process, nullptr, TRUE)) {
    logCrashMessage("CRASH: SymInitialize failed");
    FreeLibrary(dbghelp);
    return;
  }

  logCrashMessage(std::string("CRASH: stacktrace begin tag=") +
                  (tag ? tag : "unknown"));

  void *frames[64];
  const USHORT captured = CaptureStackBackTrace(0, 64, frames, nullptr);

  alignas(void *) unsigned char symbolBuffer[sizeof(SYMBOL_INFO) + 1024];
  auto *symbol = reinterpret_cast<PSYMBOL_INFO>(symbolBuffer);
  std::memset(symbolBuffer, 0, sizeof(symbolBuffer));
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 1024;

  for (USHORT i = 0; i < captured; i++) {
    const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
    DWORD64 displacement = 0;
    const bool hasSymbol =
        pSymFromAddr(process, address, &displacement, symbol) == TRUE;

    char lineBuf[2048];
    lineBuf[0] = '\0';

    bool hasLine = false;
    DWORD lineDisp = 0;
    IMAGEHLP_LINE64 line;
    std::memset(&line, 0, sizeof(line));
    line.SizeOfStruct = sizeof(line);
    if (pSymGetLineFromAddr64 &&
        pSymGetLineFromAddr64(process, address, &lineDisp, &line) == TRUE) {
      hasLine = true;
    }

    if (hasSymbol && hasLine && line.FileName) {
      std::snprintf(lineBuf, sizeof(lineBuf), "#%u 0x%llx %s +0x%llx (%s:%lu)",
                    static_cast<unsigned>(i),
                    static_cast<unsigned long long>(address), symbol->Name,
                    static_cast<unsigned long long>(displacement),
                    line.FileName, static_cast<unsigned long>(line.LineNumber));
    } else if (hasSymbol) {
      std::snprintf(lineBuf, sizeof(lineBuf), "#%u 0x%llx %s +0x%llx",
                    static_cast<unsigned>(i),
                    static_cast<unsigned long long>(address), symbol->Name,
                    static_cast<unsigned long long>(displacement));
    } else {
      std::snprintf(lineBuf, sizeof(lineBuf), "#%u 0x%llx",
                    static_cast<unsigned>(i),
                    static_cast<unsigned long long>(address));
    }

    logCrashMessage(lineBuf);
  }

  logCrashMessage("CRASH: stacktrace end");
  pSymCleanup(process);
  FreeLibrary(dbghelp);
}
#endif

static std::string makeDumpPath(const char *tag) {
#ifdef _WIN32
  SYSTEMTIME st;
  GetLocalTime(&st);
  const DWORD pid = GetCurrentProcessId();
  char buf[512];
  std::snprintf(
      buf, sizeof(buf), "nsf_lsp_%s_%lu_%04u%02u%02u_%02u%02u%02u_%03u.dmp",
      tag, static_cast<unsigned long>(pid), static_cast<unsigned>(st.wYear),
      static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay),
      static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
      static_cast<unsigned>(st.wSecond),
      static_cast<unsigned>(st.wMilliseconds));
  return std::string(buf);
#else
  (void)tag;
  return std::string();
#endif
}

#ifdef _WIN32
typedef enum MINIDUMP_TYPE : unsigned int {
  MiniDumpNormal = 0x00000000,
  MiniDumpWithDataSegs = 0x00000001,
  MiniDumpWithFullMemory = 0x00000002,
  MiniDumpWithHandleData = 0x00000004,
  MiniDumpWithThreadInfo = 0x00001000,
  MiniDumpWithIndirectlyReferencedMemory = 0x00000040
} MINIDUMP_TYPE;

typedef struct MINIDUMP_EXCEPTION_INFORMATION {
  DWORD ThreadId;
  EXCEPTION_POINTERS *ExceptionPointers;
  BOOL ClientPointers;
} MINIDUMP_EXCEPTION_INFORMATION;

using MiniDumpWriteDumpFn =
    BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                   const MINIDUMP_EXCEPTION_INFORMATION *, void *, void *);
#endif

static void writeMiniDump(EXCEPTION_POINTERS *exceptionPointers,
                          const char *tag) {
#ifdef _WIN32
  HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
  if (!dbghelp) {
    logCrashMessage("CRASH: failed to load dbghelp.dll");
    return;
  }
  auto pMiniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
      GetProcAddress(dbghelp, "MiniDumpWriteDump"));
  if (!pMiniDumpWriteDump) {
    logCrashMessage("CRASH: failed to resolve MiniDumpWriteDump");
    FreeLibrary(dbghelp);
    return;
  }

  const std::string dumpPath = makeDumpPath(tag ? tag : "crash");
  HANDLE hFile = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) {
    logCrashMessage(std::string("CRASH: failed to create dump file ") +
                    dumpPath);
    FreeLibrary(dbghelp);
    return;
  }

  MINIDUMP_EXCEPTION_INFORMATION mei;
  mei.ThreadId = GetCurrentThreadId();
  mei.ExceptionPointers = exceptionPointers;
  mei.ClientPointers = FALSE;

  const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
      MiniDumpWithDataSegs | MiniDumpWithHandleData |
      MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);

  const BOOL ok = pMiniDumpWriteDump(
      GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType,
      exceptionPointers ? &mei : nullptr, nullptr, nullptr);
  CloseHandle(hFile);
  FreeLibrary(dbghelp);

  if (ok) {
    logCrashMessage(std::string("CRASH: wrote minidump ") + dumpPath);
  } else {
    const DWORD err = GetLastError();
    logCrashMessage(std::string("CRASH: MiniDumpWriteDump failed code=") +
                    std::to_string(static_cast<unsigned long>(err)));
    DeleteFileA(dumpPath.c_str());
  }
#else
  (void)exceptionPointers;
  (void)tag;
#endif
}

#ifdef _WIN32
static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *exceptionInfo) {
  std::lock_guard<std::recursive_mutex> lock(gLogMutex);
  logStackTrace("seh");
  writeMiniDump(exceptionInfo, "seh");
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void signalHandler(int signal) {
  std::lock_guard<std::recursive_mutex> lock(gLogMutex);
  if (gCrashLogPath.empty())
    std::_Exit(signal);

  std::ofstream log(gCrashLogPath, std::ios::app);
  if (log.is_open()) {
    log << "CRASH: Signal " << signal << " received." << std::endl;
    log.flush();
    log.close();
  }
#ifdef _WIN32
  logStackTrace("signal");
#endif
  writeMiniDump(nullptr, "signal");
  std::_Exit(signal);
}

void terminateHandler() {
  logCrashMessage("CRASH: std::terminate() called.");
#ifdef _WIN32
  logStackTrace("terminate");
#endif
  writeMiniDump(nullptr, "terminate");
  std::abort();
}

void installCrashHandler(const std::string &logPath) {
  std::lock_guard<std::recursive_mutex> lock(gLogMutex);
  gCrashLogPath = logPath;

#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);
  SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif

  std::ofstream log(gCrashLogPath, std::ios::app);
  if (log.is_open()) {
    log << "Crash handler installed. Log path: " << logPath << std::endl;
    log.flush();
  }

  std::signal(SIGSEGV, signalHandler);
  std::signal(SIGABRT, signalHandler);
  std::signal(SIGFPE, signalHandler);
  std::signal(SIGILL, signalHandler);
  std::set_terminate(terminateHandler);
}

void logCrashMessage(const std::string &msg) {
  std::lock_guard<std::recursive_mutex> lock(gLogMutex);
  if (gCrashLogPath.empty())
    return;

  std::ofstream outFile(gCrashLogPath, std::ios::app);
  if (outFile.is_open()) {
    outFile << msg << std::endl;
    outFile.flush();
  }
}

void waitForDebugger() {
#ifdef _WIN32
  logCrashMessage("Waiting for debugger to attach...");
  while (!IsDebuggerPresent()) {
    Sleep(100);
  }
  logCrashMessage("Debugger attached!");
  DebugBreak();
#else
  logCrashMessage("Waiting for debugger (manual attach)...");
  volatile bool attached = false;
  while (!attached) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
#endif
}
