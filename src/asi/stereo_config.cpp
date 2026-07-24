#include "stereo_config.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>

#include <openvr.h>

namespace asi {
namespace {

std::atomic<int> g_mode{static_cast<int>(StereoMode::Off)};
std::atomic<bool> g_loggedMode{false};
std::atomic<float> g_sepM{0.06f};
std::atomic<bool> g_loggedIpd{false};
// L4D2VR default VRScale ≈ 1.0 (user tunes). 100% = standard.
std::atomic<float> g_worldScale{1.0f};
std::atomic<bool> g_loggedScale{false};

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

void SetSepCm(int cm) {
  if (cm < 0)
    cm = 0;
  if (cm > 500)
    cm = 500;
  g_sepM.store(static_cast<float>(cm) / 100.f);
}

void SaveIpdFile(int cm) {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.ipd");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  std::fprintf(f, "%d\n", cm);
  std::fclose(f);
}

void SetWorldScalePercent(int pct) {
  if (pct < 25)
    pct = 25;
  if (pct > 1000)
    pct = 1000;
  g_worldScale.store(static_cast<float>(pct) / 100.f);
}

void SaveScaleFile(int pct) {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.scale");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  std::fprintf(f, "%d\n", pct);
  std::fclose(f);
}

float ReadHmdIpdMeters() {
  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys)
    return 0.06f;
  const vr::HmdMatrix34_t e2h = sys->GetEyeToHeadTransform(vr::Eye_Right);
  const float ipd = std::fabs(e2h.m[0][3]) * 2.f;
  if (ipd > 0.04f && ipd < 0.12f)
    return ipd;
  return 0.06f;
}

// Mode 13: L4D2VR-style defaults — VRScale=1.0, IPD from HMD EyeToHead.
void ApplyStereoFusionDefaults() {
  SetWorldScalePercent(100);
  SaveScaleFile(100);
  const int cm = static_cast<int>(ReadHmdIpdMeters() * 100.f + 0.5f);
  SetSepCm(cm);
  SaveIpdFile(cm);
  g_loggedScale = false;
  g_loggedIpd = false;
  Log("StereoFusion: defaults VRScale=100%% sep=%dcm (HMD IPD, L4D2VR-style)", cm);
}

void ReloadWorldScale() {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.scale");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return;
  char buf[16]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return;
  int pct = 0;
  if (sscanf_s(buf, "%d", &pct) != 1)
    return;
  if (pct < 25 || pct > 1000)
    return;
  SetWorldScalePercent(pct);
  if (!g_loggedScale.exchange(true))
    Log("WorldScale: %.2f (file) — F7: HIGHER = smaller world (L4D2 VRScale)", pct / 100.f);
}

void ReloadIpdScale() {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.ipd");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return;
  char buf[16]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return;
  int cm = 0;
  if (sscanf_s(buf, "%d", &cm) != 1)
    return;
  if (cm < 0 || cm > 500)
    return;
  SetSepCm(cm);
  if (!g_loggedIpd.exchange(true))
    Log("StereoSep: %d cm (file gtaiv_dxvk_vr.ipd) — F8 cycles", cm);
}

// Read a small text config file next to the ASI. Returns bytes read (0 = absent).
size_t ReadSmallFile(const char* name, char* buf, size_t bufLen) {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return 0;
  strcat_s(path, name);
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return 0;
  const size_t n = fread(buf, 1, bufLen - 1, f);
  fclose(f);
  buf[n] = 0;
  return n;
}

bool KeyPressedEdge(int vk, bool* wasDown) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool edge = down && !*wasDown;
  *wasDown = down;
  return edge;
}

int ParseModeFile(const char* buf, size_t n) {
  if (n >= 2 && buf[0] >= '1' && buf[0] <= '2' && buf[1] >= '0' && buf[1] <= '9') {
    const int v = 10 * (buf[0] - '0') + (buf[1] - '0');
    if (v <= 24)
      return v;
  }
  if (n >= 1 && buf[0] >= '0' && buf[0] <= '9')
    return buf[0] - '0';
  return -1;
}

}  // namespace

void ReloadStereoMode() {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.stereo");

  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) {
    ReloadIpdScale();
    ReloadWorldScale();
    return;
  }
  char buf[8]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  int v = -1;
  if (n > 0)
    v = ParseModeFile(buf, n);
  int prev = g_mode.load();
  if (v >= 0 && v <= 24) {
    prev = g_mode.exchange(v);
    if (!g_loggedMode.exchange(true) || prev != v)
      Log("StereoMode: %d (file gtaiv_dxvk_vr.stereo)", v);
  }

  // Mode 13: apply L4D2 standard scale/IPD once when entering the mode (not every reload).
  if (static_cast<StereoMode>(g_mode.load()) == StereoMode::StereoFusion && prev != v &&
      v == static_cast<int>(StereoMode::StereoFusion)) {
    ApplyStereoFusionDefaults();
  } else {
    ReloadIpdScale();
    ReloadWorldScale();
  }
}

StereoMode GetStereoMode() {
  return static_cast<StereoMode>(g_mode.load());
}

bool IsTemporalStereoMode(StereoMode mode) {
  return mode == StereoMode::DualRtSubmit ||
         (mode >= StereoMode::StereoFusion && mode <= StereoMode::CamFovWrite);
}

bool UsesAngleCorrectCanvas(StereoMode mode) {
  return (mode >= StereoMode::GeometryCanvas && mode <= StereoMode::CamFovWrite) ||
         mode == StereoMode::SameFrameRootDual || mode == StereoMode::ExecViewDual ||
         mode == StereoMode::ExecViewPhaseDual || mode == StereoMode::ExecViewConstDual;
}

float GetStereoSepMeters() {
  return g_sepM.load();
}

float GetStereoIpdScale() {
  // Relative to ~6cm reference (L4D2 m_IpdScale ≈ sep / nativeIpd).
  return g_sepM.load() / 0.06f;
}

float GetWorldScale() {
  return g_worldScale.load();
}

void PollIpdScaleHotkey() {
  static bool wasF8 = false;
  if (!KeyPressedEdge(VK_F8, &wasF8))
    return;

  static const int kPresetsCm[] = {0, 2, 3, 4, 6, 7, 10};
  constexpr int kN = 7;
  const int curCm = static_cast<int>(g_sepM.load() * 100.f + 0.5f);
  int idx = 0;
  int best = 999;
  for (int i = 0; i < kN; ++i) {
    const int d = kPresetsCm[i] > curCm ? kPresetsCm[i] - curCm : curCm - kPresetsCm[i];
    if (d < best) {
      best = d;
      idx = i;
    }
  }
  idx = (idx + 1) % kN;
  const int cm = kPresetsCm[idx];
  SetSepCm(cm);
  SaveIpdFile(cm);
  Log("StereoSep: %d cm (F8) — L4D2 IpdScale knob", cm);
}

float GetEyeForwardMeters() {
  static std::atomic<int> s_cm{-1};
  int cm = s_cm.load();
  if (cm < 0) {
    cm = 38;  // legacy default (see past the ped skull without natives)
    char buf[16]{};
    int v = 0;
    if (ReadSmallFile("gtaiv_dxvk_vr.eyefwd", buf, sizeof(buf)) > 0 &&
        sscanf_s(buf, "%d", &v) == 1 && v >= 0 && v <= 100) {
      cm = v;
      Log("Config: eyeForward=%d cm (gtaiv_dxvk_vr.eyefwd)", cm);
    }
    s_cm.store(cm);
  }
  return static_cast<float>(cm) / 100.f;
}

bool IsFreeMoveEnabled() {
  static std::atomic<int> s_mode{-1};
  int m = s_mode.load();
  if (m < 0) {
    m = 0;
    char buf[16]{};
    int v = 0;
    if (ReadSmallFile("gtaiv_dxvk_vr.movemode", buf, sizeof(buf)) > 0 &&
        sscanf_s(buf, "%d", &v) == 1 && v >= 0 && v <= 1) {
      m = v;
      if (m == 1)
        Log("Config: FREE MOVE on (gtaiv_dxvk_vr.movemode=1) — ped heading not forced");
    }
    s_mode.store(m);
  }
  return m == 1;
}

int GetVehicleFollowMode() {
  static std::atomic<int> s_mode{-1};
  int m = s_mode.load();
  if (m < 0) {
    m = 0;
    char buf[16]{};
    int v = 0;
    if (ReadSmallFile("gtaiv_dxvk_vr.vehfollow", buf, sizeof(buf)) > 0 &&
        sscanf_s(buf, "%d", &v) == 1 && v >= 0 && v <= 2) {
      m = v;
      if (m != 0)
        Log("Config: vehicle heading-follow %s (gtaiv_dxvk_vr.vehfollow=%d)",
            m == 2 ? "on INVERTED" : "on", m);
    }
    s_mode.store(m);
  }
  return m;
}

bool GetFovPatchConfig(int* offsetBytes, float* scale) {
  if (!offsetBytes || !scale)
    return false;
  static std::atomic<bool> s_read{false};
  static std::atomic<int> s_off{0};
  static std::atomic<float> s_scale{0.f};
  if (!s_read.exchange(true)) {
    char buf[32]{};
    int off = 0, pct = 0;
    if (ReadSmallFile("gtaiv_dxvk_vr.fovpatch", buf, sizeof(buf)) > 0 &&
        sscanf_s(buf, "%d %d", &off, &pct) == 2 && off >= -512 && off <= 512 && pct >= 50 &&
        pct <= 300) {
      s_off.store(off);
      s_scale.store(static_cast<float>(pct) / 100.f);
      Log("Config: fovpatch off=%d scale=%.2f (gtaiv_dxvk_vr.fovpatch)", off, pct / 100.f);
    }
  }
  if (s_scale.load() <= 0.f)
    return false;
  *offsetBytes = s_off.load();
  *scale = s_scale.load();
  return true;
}

void PollWorldScaleHotkey() {
  static bool wasF7 = false;
  if (!KeyPressedEdge(VK_F7, &wasF7))
    return;

  // L4D2VR-style VRScale around 1.0; higher → world feels smaller.
  static const int kPresets[] = {50, 75, 100, 125, 150, 200, 300};
  constexpr int kN = 7;
  const int curPct = static_cast<int>(g_worldScale.load() * 100.f + 0.5f);
  int idx = 0;
  int best = 999;
  for (int i = 0; i < kN; ++i) {
    const int d = kPresets[i] > curPct ? kPresets[i] - curPct : curPct - kPresets[i];
    if (d < best) {
      best = d;
      idx = i;
    }
  }
  idx = (idx + 1) % kN;
  const int pct = kPresets[idx];
  SetWorldScalePercent(pct);
  SaveScaleFile(pct);
  Log("WorldScale: %.2f (F7) — L4D2 VRScale; HIGHER = smaller world", pct / 100.f);
}

}  // namespace asi
