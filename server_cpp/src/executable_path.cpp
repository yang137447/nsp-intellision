#include "executable_path.hpp"

#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#endif

std::filesystem::path getExecutableDir() {
#if defined(_WIN32)
  std::wstring buffer;
  buffer.resize(32768);
  DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
                                static_cast<DWORD>(buffer.size()));
  if (len == 0 || len >= buffer.size())
    return std::filesystem::current_path();
  buffer.resize(len);
  std::filesystem::path exePath(buffer);
  return exePath.parent_path();
#else
  return std::filesystem::current_path();
#endif
}

