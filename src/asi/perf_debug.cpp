#include "perf_debug.h"
#include "log.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace asi {
namespace {

std::mutex g_mu;
std::atomic<int> g_enabled{-1};  // -1 unknown, 0 off, 1 on

char g_dir[MAX_PATH]{};
char g_perfPath[MAX_PATH]{};
char g_memPath[MAX_PATH]{};
char g_vrPath[MAX_PATH]{};

std::atomic<uint32_t> g_presentN{0};
std::atomic<uint32_t> g_dualN{0};
std::atomic<uint32_t> g_holdN{0};
std::atomic<uint32_t> g_hitchSkipN{0};

std::atomic<uint32_t> g_winDual{0};
std::atomic<uint32_t> g_winHold{0};
std::atomic<uint32_t> g_winHitchSkip{0};
std::atomic<uint32_t> g_winPoseStall{0};  // poseMs >= 20

double g_lastPoseMs = 0.0;
int g_lastPoseErr = 0;
uint32_t g_lastDrawGapMs = 0;
uint32_t g_maxHitchMsWin = 0;

DWORD g_lastSampleTick = 0;
uint32_t g_lastSamplePresent = 0;
DWORD g_lastMemTick = 0;
bool g_perfHeader = false;

bool GetAsiDir(char* out, DWORD outLen) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&GetAsiDir), &self))
    return false;
  if (!GetModuleFileNameA(self, out, outLen))
    return false;
  char* slash = strrchr(out, '\\');
  if (!slash)
    slash = strrchr(out, '/');
  if (!slash)
    return false;
  slash[1] = 0;
  return true;
}

int ReadDebugToggle() {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return 1;
  strcat_s(path, "gtaiv_dxvk_vr.debug");
  const DWORD attr = GetFileAttributesA(path);
  if (attr == INVALID_FILE_ATTRIBUTES)
    return 1;  // default ON
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return 1;
  char buf[16]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return 1;
  int v = 1;
  if (sscanf_s(buf, "%d", &v) == 1)
    return (v != 0) ? 1 : 0;
  if (buf[0] == '0')
    return 0;
  return 1;
}

void AppendLine(const char* path, const char* line) {
  FILE* f = nullptr;
  if (fopen_s(&f, path, "ab") != 0 || !f)
    return;
  fputs(line, f);
  fputc('\n', f);
  fclose(f);
}

void SampleMem(DWORD now) {
  if (g_lastMemTick != 0 && (now - g_lastMemTick) < 2000)
    return;
  g_lastMemTick = now;

  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  if (!GetProcessMemoryInfo(GetCurrentProcess(),
                            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                            sizeof(pmc)))
    return;

  const SIZE_T ws = pmc.WorkingSetSize;
  const SIZE_T priv = pmc.PrivateUsage ? pmc.PrivateUsage : pmc.PagefileUsage;
  char line[256];
  snprintf(line, sizeof(line),
           "t=%lu ws_mb=%.1f priv_mb=%.1f present=%u dual=%u hold=%u hitchSkip=%u",
           static_cast<unsigned long>(now), ws / (1024.0 * 1024.0),
           priv / (1024.0 * 1024.0), g_presentN.load(), g_dualN.load(),
           g_holdN.load(), g_hitchSkipN.load());
  AppendLine(g_memPath, line);
}

void SamplePerf(DWORD now) {
  if (g_lastSampleTick == 0) {
    g_lastSampleTick = now;
    g_lastSamplePresent = g_presentN.load();
    return;
  }
  const DWORD dt = now - g_lastSampleTick;
  if (dt < 1500)
    return;

  const uint32_t pn = g_presentN.load();
  const uint32_t dSub = pn - g_lastSamplePresent;
  const float appFps =
      dt > 0 ? (1000.f * static_cast<float>(dSub) / static_cast<float>(dt)) : 0.f;
  const uint32_t dual = g_winDual.exchange(0);
  const uint32_t hold = g_winHold.exchange(0);
  const uint32_t hitchSkip = g_winHitchSkip.exchange(0);
  const uint32_t poseStall = g_winPoseStall.exchange(0);
  const uint32_t maxHitch = g_maxHitchMsWin;
  g_maxHitchMsWin = 0;

  if (!g_perfHeader) {
    AppendLine(g_perfPath,
               "t_ms,app_fps,submit_n,dt_ms,pose_ms,pose_err,dual,hold,hitch_skip,"
               "pose_stall20,draw_gap_ms,max_hitch_ms,stereo");
    g_perfHeader = true;
  }

  char line[320];
  snprintf(line, sizeof(line),
           "%lu,%.1f,%u,%lu,%.1f,%d,%u,%u,%u,%u,%u,%u,%d",
           static_cast<unsigned long>(now), appFps, pn,
           static_cast<unsigned long>(dt), g_lastPoseMs, g_lastPoseErr, dual, hold,
           hitchSkip, poseStall, g_lastDrawGapMs, maxHitch,
           1 /* stereo path sampled during Mode120 */);
  AppendLine(g_perfPath, line);

  g_lastSampleTick = now;
  g_lastSamplePresent = pn;
}

}  // namespace

void PerfDebugInit() {
  if (!GetAsiDir(g_dir, MAX_PATH))
    return;
  snprintf(g_perfPath, sizeof(g_perfPath), "%sgtaiv_dxvk_vr.perf.csv", g_dir);
  snprintf(g_memPath, sizeof(g_memPath), "%sgtaiv_dxvk_vr.mem.log", g_dir);
  snprintf(g_vrPath, sizeof(g_vrPath), "%sgtaiv_dxvk_vr.vr.log", g_dir);

  const int on = ReadDebugToggle();
  g_enabled.store(on);
  if (!on) {
    Log("PerfDebug: OFF (gtaiv_dxvk_vr.debug=0)");
    return;
  }

  // Archive prior session files, then truncate so digests are clean.
  ArchiveSessionLog(g_perfPath);
  ArchiveSessionLog(g_memPath);
  ArchiveSessionLog(g_vrPath);
  {
    FILE* f = nullptr;
    if (fopen_s(&f, g_perfPath, "wb") == 0 && f)
      fclose(f);
    if (fopen_s(&f, g_memPath, "wb") == 0 && f) {
      fputs("gtaiv-dxvk-vr mem sample (WorkingSet/PrivateBytes)\n", f);
      fclose(f);
    }
    if (fopen_s(&f, g_vrPath, "wb") == 0 && f) {
      fputs("gtaiv-dxvk-vr VR/compositor rare notes\n", f);
      fclose(f);
    }
  }
  g_perfHeader = false;
  g_lastSampleTick = 0;
  g_lastMemTick = 0;
  Log("PerfDebug: ON — sampled %s + %s (+ rare %s); toggle gtaiv_dxvk_vr.debug",
      "gtaiv_dxvk_vr.perf.csv", "gtaiv_dxvk_vr.mem.log", "gtaiv_dxvk_vr.vr.log");
}

bool PerfDebugEnabled() {
  int v = g_enabled.load();
  if (v < 0) {
    v = ReadDebugToggle();
    g_enabled.store(v);
  }
  return v != 0;
}

void PerfDebugOnPoseWait(double poseMs, int poseErr) {
  if (!PerfDebugEnabled())
    return;
  g_lastPoseMs = poseMs;
  g_lastPoseErr = poseErr;
  if (poseMs >= 20.0)
    g_winPoseStall.fetch_add(1);
  // Rare: log stalls / errors to .vr.log (not main log spam).
  if (poseMs >= 20.0 || poseErr != 0) {
    char line[160];
    snprintf(line, sizeof(line), "WaitGetPoses ms=%.1f err=%d", poseMs, poseErr);
    AppendLine(g_vrPath, line);
  }
}

void PerfDebugNoteDrawScene(bool didDual, bool hold, uint32_t gapMs) {
  if (!PerfDebugEnabled())
    return;
  g_lastDrawGapMs = gapMs;
  if (gapMs > g_maxHitchMsWin)
    g_maxHitchMsWin = gapMs;
  if (didDual) {
    g_dualN.fetch_add(1);
    g_winDual.fetch_add(1);
  } else if (gapMs >= 50u) {
    g_hitchSkipN.fetch_add(1);
    g_winHitchSkip.fetch_add(1);
  } else if (hold) {
    g_holdN.fetch_add(1);
    g_winHold.fetch_add(1);
  }
}

void PerfDebugOnPresent(bool /*stereoSubmitted*/, bool /*dualTick*/, bool /*holdTick*/,
                        uint32_t /*drawGapMs*/) {
  if (!PerfDebugEnabled())
    return;
  g_presentN.fetch_add(1);
  const DWORD now = GetTickCount();
  std::lock_guard<std::mutex> lock(g_mu);
  SamplePerf(now);
  SampleMem(now);
}

void PerfDebugVrNote(const char* fmt, ...) {
  if (!PerfDebugEnabled() || !g_vrPath[0])
    return;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::lock_guard<std::mutex> lock(g_mu);
  AppendLine(g_vrPath, buf);
}

}  // namespace asi
