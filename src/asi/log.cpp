#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include <windows.h>

// std::ifstream used for buildid

namespace asi {
namespace {

std::mutex g_mu;
std::string g_path;

// Street hitch cut: open/append/close per Log line was ~45k EyeProj lines of disk I/O.
// Keep a small ring buffer and flush every ~100ms or when full.
constexpr size_t kBufCap = 48 * 1024;
char g_buf[kBufCap];
size_t g_bufLen = 0;
DWORD g_lastFlushTick = 0;

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

void FlushUnlocked(bool force) {
  if (g_bufLen == 0)
    return;
  const DWORD now = GetTickCount();
  if (!force && g_bufLen < (kBufCap * 3 / 4) && g_lastFlushTick != 0 &&
      (now - g_lastFlushTick) < 100u)
    return;
  std::ofstream f(g_path, std::ios::app);
  if (f)
    f.write(g_buf, static_cast<std::streamsize>(g_bufLen));
  g_bufLen = 0;
  g_lastFlushTick = now;
}

}  // namespace

void LogInit() {
  g_path = GameDirLogPath();
  // Truncate each session so deploy scripts cannot match stale MonoSubmit lines.
  std::ofstream(g_path, std::ios::trunc) << "gtaiv-dxvk-vr ASI interop probe\n";
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_bufLen = 0;
    g_lastFlushTick = GetTickCount();
  }

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
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  const size_t lineLen = std::strlen(line);
  OutputDebugStringA(line);
  OutputDebugStringA("\n");

  std::lock_guard<std::mutex> lock(g_mu);
  // Force flush important one-shot lines so deploy/smoke scripts see them promptly.
  const bool important =
      std::strstr(line, "ASI_BUILD_ID") || std::strstr(line, "StereoMode:") ||
      std::strstr(line, "Mode88:") || std::strstr(line, "Mode87:") ||
      std::strstr(line, "Mode89:") || std::strstr(line, "DeviceReset:") ||
      std::strstr(line, "EXCEPTION") || std::strstr(line, "FOVPROOF:");
  if (g_bufLen + lineLen + 2 > kBufCap)
    FlushUnlocked(true);
  if (g_bufLen + lineLen + 2 <= kBufCap) {
    std::memcpy(g_buf + g_bufLen, line, lineLen);
    g_bufLen += lineLen;
    g_buf[g_bufLen++] = '\n';
  } else {
    // Line alone too large — write direct.
    std::ofstream f(g_path, std::ios::app);
    if (f)
      f << line << '\n';
  }
  FlushUnlocked(important);
}

}  // namespace asi
