#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <windows.h>

namespace asi {
namespace {

std::mutex g_mu;
std::string g_path;

std::string GameDirLogPath() {
  char mod[MAX_PATH]{};
  HMODULE self = nullptr;
  GetModuleHandleExA(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(&GameDirLogPath),
      &self);
  GetModuleFileNameA(self, mod, MAX_PATH);
  std::string p(mod);
  const auto slash = p.find_last_of("\\/");
  if (slash != std::string::npos)
    p.resize(slash + 1);
  p += "gtaiv_dxvk_vr.log";
  return p;
}

}  // namespace

void LogInit() {
  g_path = GameDirLogPath();
  // Truncate each session
  std::ofstream(g_path, std::ios::trunc) << "gtaiv-dxvk-vr ASI interop probe\n";
  Log("ASI loaded sizeof(void*)=%zu path=%s", sizeof(void*), g_path.c_str());
}

void Log(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  std::lock_guard<std::mutex> lock(g_mu);
  std::ofstream f(g_path, std::ios::app);
  if (f)
    f << buf << '\n';
  OutputDebugStringA(buf);
  OutputDebugStringA("\n");
}

}  // namespace asi
