#include "glue.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

namespace gtaiv {
namespace {

std::mutex g_log_mu;
std::string g_log_path = "gtaiv_dxvk_vr.log";

}  // namespace

void LogInit(const char* path) {
  if (path && *path)
    g_log_path = path;
  LogWrite("log init path=%s sizeof(void*)=%zu", g_log_path.c_str(), sizeof(void*));
}

void LogWrite(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  std::lock_guard<std::mutex> lock(g_log_mu);
  std::ofstream f(g_log_path, std::ios::app);
  if (f)
    f << buf << '\n';
}

bool LoadConfig(const char* path, Config* out) {
  if (!out)
    return false;
  *out = Config{};
  // Minimal: file presence is enough for now; real ini parser later.
  if (!path)
    return true;
  std::ifstream f(path);
  if (!f) {
    LogWrite("config missing (using defaults): %s", path);
    return false;
  }
  LogWrite("config found: %s", path);
  return true;
}

bool OpenVrInit() {
  LogWrite("OpenVrInit: STUB — wire openvr_api after SteamVR + flat d3d9.dll");
  return false;
}

void OpenVrShutdown() {
  LogWrite("OpenVrShutdown: STUB");
}

bool TryMonoSubmitPlaceholder() {
  LogWrite("TryMonoSubmitPlaceholder: STUB — need GetVRDesc + IVRCompositor::Submit");
  return false;
}

}  // namespace gtaiv
