#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

#include <windows.h>

// std::ifstream used for buildid

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
  // Truncate each session so deploy scripts cannot match stale MonoSubmit lines.
  std::ofstream(g_path, std::ios::trunc) << "gtaiv-dxvk-vr ASI interop probe\n";

  // Optional stamp from build-deploy-run.ps1 (gtaiv_dxvk_vr.buildid next to ASI).
  char buildId[128]{};
  {
    std::string idPath = g_path;
    const auto slash = idPath.find_last_of("\\/");
    if (slash != std::string::npos)
      idPath.resize(slash + 1);
    idPath += "gtaiv_dxvk_vr.buildid";
    std::ifstream in(idPath);
    if (in)
      in.getline(buildId, sizeof(buildId));
  }

  Log("ASI loaded sizeof(void*)=%zu path=%s", sizeof(void*), g_path.c_str());
  if (buildId[0])
    Log("ASI_BUILD_ID %s", buildId);
  else
    Log("ASI_BUILD_ID (none)");
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
