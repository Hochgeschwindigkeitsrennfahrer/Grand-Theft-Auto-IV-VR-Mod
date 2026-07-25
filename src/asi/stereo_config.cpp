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
// 6DoF lean gain: game-meters moved per meter of HMD lean. Higher = smaller world.
// Applied as eyeDelta / leanGain (NOT multiply — that made F7 feel bigger).
std::atomic<float> g_leanGain{1.0f};
std::atomic<bool> g_loggedScale{false};
// Soft stereo disparity (not WorldScale). 115% ≈ reclaim size without 150%×IPD break.
std::atomic<float> g_stereoScale{1.15f};
std::atomic<bool> g_loggedStereoScale{false};

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

void SetLeanGain(float gain) {
  if (gain < 1.0f)
    gain = 1.0f;
  if (gain > 30.0f)
    gain = 30.0f;
  g_leanGain.store(gain);
}

void SaveScaleFile(float gain) {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.scale");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  const int fileVal = static_cast<int>(gain * 100.f + 0.5f);
  std::fprintf(f, "%d\n", fileVal);
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
  SetLeanGain(1.0f);
  SaveScaleFile(1.0f);
  const int cm = static_cast<int>(ReadHmdIpdMeters() * 100.f + 0.5f);
  SetSepCm(cm);
  SaveIpdFile(cm);
  g_loggedScale = false;
  g_loggedIpd = false;
  Log("StereoFusion: defaults VRScale=100%% sep=%dcm (HMD IPD, L4D2VR-style)", cm);
}

void SetStereoScalePercent(int pct) {
  if (pct < 100)
    pct = 100;
  if (pct > 130)
    pct = 130;  // hard cap — 150%×IPD broke fusion
  g_stereoScale.store(static_cast<float>(pct) / 100.f);
}

void SaveStereoScaleFile(int pct) {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.stereoscale");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  std::fprintf(f, "%d\n", pct);
  std::fclose(f);
}

void ReloadStereoScale() {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.stereoscale");
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
  if (pct < 100 || pct > 130)
    return;
  SetStereoScalePercent(pct);
  if (!g_loggedStereoScale.exchange(true))
    Log("StereoScale: %.2f (file) — F6: higher = stronger 3D / smaller feel (cap 130%%)",
        pct / 100.f);
}

// Mode 14/26 first entry: only fill MISSING knobs. Never overwrite a user-tuned
// IPD/scale file — headset 2026-07-24: best 3D IPD was wiped on mode reload.
void ApplyGeometryCanvasDefaults() {
  char path[MAX_PATH]{};
  bool haveIpd = false, haveScale = false, haveStereo = false;
  if (GetAsiDir(path, MAX_PATH)) {
    char ipdPath[MAX_PATH], scalePath[MAX_PATH], ssPath[MAX_PATH];
    strcpy_s(ipdPath, path); strcat_s(ipdPath, "gtaiv_dxvk_vr.ipd");
    strcpy_s(scalePath, path); strcat_s(scalePath, "gtaiv_dxvk_vr.scale");
    strcpy_s(ssPath, path); strcat_s(ssPath, "gtaiv_dxvk_vr.stereoscale");
    haveIpd = GetFileAttributesA(ipdPath) != INVALID_FILE_ATTRIBUTES;
    haveScale = GetFileAttributesA(scalePath) != INVALID_FILE_ATTRIBUTES;
    haveStereo = GetFileAttributesA(ssPath) != INVALID_FILE_ATTRIBUTES;
  }
  if (!haveScale) {
    SetLeanGain(1.0f);
    SaveScaleFile(1.0f);
  }
  if (!haveStereo) {
    SetStereoScalePercent(115);
    SaveStereoScaleFile(115);
  }
  if (!haveIpd) {
    const int cm = static_cast<int>(ReadHmdIpdMeters() * 100.f + 0.5f);
    SetSepCm(cm);
    SaveIpdFile(cm);
  }
  // Reload* are defined below — call via the public path after mode select.
  g_loggedScale = false;
  g_loggedIpd = false;
  g_loggedStereoScale = false;
  Log("GeometryCanvas: preserve user knob files (ipd=%d scale=%d stereoscale=%d)",
      haveIpd ? 1 : 0, haveScale ? 1 : 0, haveStereo ? 1 : 0);
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
  int fileVal = 0;
  if (sscanf_s(buf, "%d", &fileVal) != 1)
    return;
  if (fileVal < 100 || fileVal > 3000)
    return;
  const float gain = static_cast<float>(fileVal) / 100.f;
  SetLeanGain(gain);
  if (!g_loggedScale.exchange(true))
    Log("LeanGain: %.2f (file gtaiv_dxvk_vr.scale=%d) — F7 cycles up = smaller world",
        gain, fileVal);
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
  if (!g_loggedIpd.exchange(true)) {
    if (cm == 0)
      Log("StereoSep: 0 cm (file) — NO PARALLAX / flat image. Set gtaiv_dxvk_vr.ipd to 1+ "
          "(or F8; presets start at 1cm) for 3D");
    else
      Log("StereoSep: %d cm (file gtaiv_dxvk_vr.ipd) — F8 cycles 1,2,3,4,6,7,10", cm);
  }
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

int RemapLoadedStereoMode(int v) {
  // Removed / dangerous camera experiments — never arm from stale stereo files.
  if (v >= 46 && v <= 61) {
    Log("StereoMode: requested %d is DISABLED — using 45 (see docs/RE_OFFSETS.md)", v);
    return 45;
  }
  // Mode 71 in-place view+0x80 WRITE froze twice (2026-07-25).
  if (v == 71) {
    Log("StereoMode: requested 71 TEMPORAL-INJECT is DISABLED (freeze) — using 45");
    return 45;
  }
  // Mode 73 BuildRootA×2 same-frame: stable FPS but world streaming never finishes
  // (empty world + load spinner 2026-07-25). Fall back to Mode 72 temporal.
  // Mode 74 = gated same-frame (dual only after in-world); do NOT remap 74.
  if (v == 73) {
    Log("StereoMode: requested 73 SAME-FRAME dual DISABLED (empty world/streaming) — using 72");
    return 72;
  }
  return v;
}

int ParseModeFile(const char* buf, size_t n) {
  // Two-digit modes 10..74 (45 = LKG; 71→45; 72 temporal; 73→72; 74 gated same-frame).
  if (n >= 2 && buf[0] >= '1' && buf[0] <= '7' && buf[1] >= '0' && buf[1] <= '9') {
    const int v = 10 * (buf[0] - '0') + (buf[1] - '0');
    if (v <= 74)
      return RemapLoadedStereoMode(v);
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
  if (v >= 0 && v <= 74) {
    prev = g_mode.exchange(v);
    if (!g_loggedMode.exchange(true) || prev != v)
      Log("StereoMode: %d (file gtaiv_dxvk_vr.stereo)", v);
  }

  // Apply once when entering the mode (not every reload).
  if (prev != v && v == static_cast<int>(StereoMode::StereoFusion)) {
    ApplyStereoFusionDefaults();
  } else if (prev != v && (v == static_cast<int>(StereoMode::RecordDualReplayShift) ||
                           v == static_cast<int>(StereoMode::GeometryCanvas) ||
                           v == static_cast<int>(StereoMode::SameFrameWrapDual) ||
                           v == static_cast<int>(StereoMode::ReplayRootProbe) ||
                           v == static_cast<int>(StereoMode::VsParentCountProbe) ||
                           v == static_cast<int>(StereoMode::PhaseDualDeviceVs) ||
                           v == static_cast<int>(StereoMode::SameFrameReplayDual) ||
                           v == static_cast<int>(StereoMode::SameFrameVsParentDual) ||
                           v == static_cast<int>(StereoMode::SameFrameLateVsParentDual) ||
                           v == static_cast<int>(StereoMode::SameFrameVsRetCallerDual) ||
                           v == static_cast<int>(StereoMode::FovRecomputeSite) ||
                           v == static_cast<int>(StereoMode::FovRecomputeTrueCanvas) ||
                           v == static_cast<int>(StereoMode::FovCanvasComfort) ||
                           v == static_cast<int>(StereoMode::AerPoseSubmit) ||
                           v == static_cast<int>(StereoMode::FovCanvasLowMotion) ||
                           v == static_cast<int>(StereoMode::FovCanvasMotionGuard) ||
                           v == static_cast<int>(StereoMode::ReplayCallChainProbe) ||
                           v == static_cast<int>(StereoMode::ReplayOwnerCountProbe) ||
                           v == static_cast<int>(StereoMode::FovCanvasMotionGuardFast) ||
                           v == static_cast<int>(StereoMode::FovCanvasMotionGuardRtLock) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamSpike) ||
                           v == static_cast<int>(StereoMode::RePatternValidate) ||
                           v == static_cast<int>(StereoMode::SameFrameSeamGate) ||
                           v == static_cast<int>(StereoMode::ViewConstCountProbe) ||
                           v == static_cast<int>(StereoMode::ReplayVtable178Log) ||
                           v == static_cast<int>(StereoMode::PublishSyncCountProbe) ||
                           v == static_cast<int>(StereoMode::ViewMatWriterCountProbe) ||
                           v == static_cast<int>(StereoMode::ReplayDispatchCountProbe) ||
                           v == static_cast<int>(StereoMode::ReplayDispatchMatPeek) ||
                           v == static_cast<int>(StereoMode::ReplayDispatchActivePeek) ||
                           v == static_cast<int>(StereoMode::ReplayDispatchTemporalInject) ||
                           v == static_cast<int>(StereoMode::VsConstTemporalInject) ||
                           v == static_cast<int>(StereoMode::SameFrameVsConstDual) ||
                           v == static_cast<int>(StereoMode::SameFrameVsConstGatedDual))) {
    ApplyGeometryCanvasDefaults();
    ReloadIpdScale();
    ReloadWorldScale();
    ReloadStereoScale();
  } else {
    ReloadIpdScale();
    ReloadWorldScale();
    ReloadStereoScale();
  }
}

void WriteStereoModeFile(int mode) {
  if (mode < 0 || mode > 74)
    return;
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.stereo");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  std::fprintf(f, "%d\n", mode);
  std::fclose(f);
  g_mode.store(mode);
  Log("StereoMode: wrote file=%d (safe default / fallback)", mode);
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
         mode == StereoMode::ExecViewPhaseDual || mode == StereoMode::ExecViewConstDual ||
         mode == StereoMode::BuildDualViewShift || mode == StereoMode::RecordDualReplayShift ||
         mode == StereoMode::SameFrameWrapDual || mode == StereoMode::ReplayRootProbe ||
         mode == StereoMode::VsParentCountProbe || mode == StereoMode::PhaseDualDeviceVs ||
         mode == StereoMode::SameFrameReplayDual || mode == StereoMode::SameFrameVsParentDual ||
         mode == StereoMode::SameFrameLateVsParentDual ||
         mode == StereoMode::SameFrameVsRetCallerDual ||
         mode == StereoMode::FovRecomputeSite ||
         mode == StereoMode::FovRecomputeTrueCanvas ||
         mode == StereoMode::FovCanvasComfort ||
         mode == StereoMode::AerPoseSubmit ||
         mode == StereoMode::FovCanvasLowMotion ||
         mode == StereoMode::FovCanvasMotionGuard ||
         mode == StereoMode::ReplayCallChainProbe ||
         mode == StereoMode::ReplayOwnerCountProbe ||
         mode == StereoMode::FovCanvasMotionGuardFast ||
         mode == StereoMode::FovCanvasMotionGuardRtLock ||
         mode == StereoMode::HeadOwnedCamSpike || mode == StereoMode::RePatternValidate ||
         mode == StereoMode::SameFrameSeamGate || mode == StereoMode::ViewConstCountProbe ||
         mode == StereoMode::ReplayVtable178Log || mode == StereoMode::PublishSyncCountProbe ||
         mode == StereoMode::ViewMatWriterCountProbe ||
         mode == StereoMode::ReplayDispatchCountProbe ||
         mode == StereoMode::ReplayDispatchMatPeek ||
         mode == StereoMode::ReplayDispatchActivePeek ||
         mode == StereoMode::ReplayDispatchTemporalInject ||
         mode == StereoMode::VsConstTemporalInject ||
         mode == StereoMode::SameFrameVsConstDual ||
         mode == StereoMode::SameFrameVsConstGatedDual;
}

bool IsHeadOwnedCamFamily(StereoMode mode) {
  return mode == StereoMode::HeadOwnedCamSpike || mode == StereoMode::RePatternValidate ||
         mode == StereoMode::SameFrameSeamGate || mode == StereoMode::ViewConstCountProbe ||
         mode == StereoMode::ReplayVtable178Log || mode == StereoMode::PublishSyncCountProbe ||
         mode == StereoMode::ViewMatWriterCountProbe ||
         mode == StereoMode::ReplayDispatchCountProbe ||
         mode == StereoMode::ReplayDispatchMatPeek ||
         mode == StereoMode::ReplayDispatchActivePeek ||
         mode == StereoMode::ReplayDispatchTemporalInject ||
         mode == StereoMode::VsConstTemporalInject ||
         mode == StereoMode::SameFrameVsConstDual ||
         mode == StereoMode::SameFrameVsConstGatedDual;
}

float GetStereoSepMeters() {
  return g_sepM.load();
}

float GetStereoIpdScale() {
  // Relative to ~6cm reference (L4D2 m_IpdScale ≈ sep / nativeIpd).
  return g_sepM.load() / 0.06f;
}

float GetLeanGain() {
  return g_leanGain.load();
}

float GetWorldScale() {
  return g_leanGain.load();
}

float GetStereoScale() {
  return g_stereoScale.load();
}

void PollIpdScaleHotkey() {
  static bool wasF8 = false;
  static bool primed = false;
  // Prime: load ipd file BEFORE any F8 (ReloadStereoMode only runs at CamMatrix arm
  // ~360 submits). Also ignore key already held at first EndScene (was 6→7 overwrite).
  if (!primed) {
    ReloadIpdScale();
    wasF8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    primed = true;
    return;
  }
  if (!KeyPressedEdge(VK_F8, &wasF8))
    return;

  // Floor 1cm (not 0): user 2026-07-24 — 2cm almost fused, needs closer still.
  static const int kPresetsCm[] = {1, 2, 3, 4, 6, 7, 10};
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
  Log("StereoSep: %d cm (F8) — L4D2 IpdScale knob (presets 1..10; file still 0..500)", cm);
}

float GetEyeForwardMeters() {
  static std::atomic<int> s_cm{-1};
  int cm = s_cm.load();
  if (cm < 0) {
    // 42cm: past skull/hair. With PedHide on, user can lower via eyefwd file.
    cm = 42;
    char buf[16]{};
    int v = 0;
    if (ReadSmallFile("gtaiv_dxvk_vr.eyefwd", buf, sizeof(buf)) > 0 &&
        sscanf_s(buf, "%d", &v) == 1 && v >= 0 && v <= 100) {
      cm = v;
      Log("Config: eyeForward=%d cm (gtaiv_dxvk_vr.eyefwd) — head embed / past hair", cm);
    } else {
      Log("Config: eyeForward=%d cm (default) — with PedHide try 12–20 for less swing", cm);
    }
    s_cm.store(cm);
  }
  return static_cast<float>(cm) / 100.f;
}

void GetCamOffsetMeters(float* outRight, float* outForward, float* outUp) {
  static std::atomic<bool> s_read{false};
  static float s_x = 0.f, s_y = 0.f, s_z = 0.f;
  if (!s_read.exchange(true)) {
    char buf[64]{};
    int x = 0, y = 0, z = 0;
    if (ReadSmallFile("gtaiv_dxvk_vr.camoff", buf, sizeof(buf)) > 0 &&
        sscanf_s(buf, "%d %d %d", &x, &y, &z) == 3 && x >= -50 && x <= 50 && y >= -50 &&
        y <= 50 && z >= -50 && z <= 50) {
      s_x = static_cast<float>(x) / 100.f;
      s_y = static_cast<float>(y) / 100.f;
      s_z = static_cast<float>(z) / 100.f;
      Log("Config: camoff right=%d fwd=%d up=%d cm (Inspiration FPX/FPY/FPZ style)", x, y, z);
    }
  }
  if (outRight)
    *outRight = s_x;
  if (outForward)
    *outForward = s_y;
  if (outUp)
    *outUp = s_z;
}

bool IsPedHideEnabled() {
  static std::atomic<int> s_mode{-1};
  int m = s_mode.load();
  if (m < 0) {
    // Default ON — Inspiration head/hair hide. Kill: write 0 to pedhide file.
    m = 1;
    char buf[16]{};
    int v = 0;
    if (ReadSmallFile("gtaiv_dxvk_vr.pedhide", buf, sizeof(buf)) > 0 &&
        sscanf_s(buf, "%d", &v) == 1 && (v == 0 || v == 1)) {
      m = v;
    }
    Log("Config: PedHide=%s (gtaiv_dxvk_vr.pedhide) — Inspiration SetDraw path; kill=0",
        m ? "ON" : "OFF");
    s_mode.store(m);
  }
  return m == 1;
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

float GetFovAddDegrees() {
  static std::atomic<float> s_add{-1.f};
  if (s_add.load() < 0.f) {
    char buf[16]{};
    int deg = 0;
    float add = 0.f;
    if (ReadSmallFile("gtaiv_dxvk_vr.fovadd", buf, sizeof(buf)) > 0 &&
        sscanf_s(buf, "%d", &deg) == 1 && deg >= 0 && deg <= 40) {
      add = static_cast<float>(deg);
      Log("Config: fovadd=%.0f deg (gtaiv_dxvk_vr.fovadd) — ADD at CCam+0x60 after recompute; "
          "kill: delete file or set 0",
          add);
    } else {
      Log("Config: fovadd=0 (absent) — Mode 35 FOV site READ-ONLY log");
    }
    s_add.store(add);
  }
  return s_add.load();
}

void PollWorldScaleHotkey() {
  static bool wasF7 = false;
  if (!KeyPressedEdge(VK_F7, &wasF7))
    return;

  // 6DoF only — does not affect stereo IPD. Higher leanGain = less lean = smaller world.
  // 11 steps to 30.0 — fine control near 1.0, wide reach for huge-world feel.
  static const float kLeanGains[] = {1.0f,  1.25f, 1.5f,  2.0f, 2.5f, 3.0f,
                                     4.0f,  5.0f,  8.0f,  12.0f, 30.0f};
  constexpr int kN = 11;
  const float cur = g_leanGain.load();
  int idx = 0;
  float best = 999.f;
  for (int i = 0; i < kN; ++i) {
    const float d = std::fabs(kLeanGains[i] - cur);
    if (d < best) {
      best = d;
      idx = i;
    }
  }
  idx = (idx + 1) % kN;
  const float gain = kLeanGains[idx];
  SetLeanGain(gain);
  SaveScaleFile(gain);
  Log("F7: world smaller — leanGain=%.2f (step %d/%d)", gain, idx + 1, kN);
}

void PollStereoScaleHotkey() {
  static bool wasF6 = false;
  if (!KeyPressedEdge(VK_F6, &wasF6))
    return;

  static const int kPresets[] = {100, 110, 115, 120, 125, 130};
  constexpr int kN = 6;
  const int curPct = static_cast<int>(g_stereoScale.load() * 100.f + 0.5f);
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
  SetStereoScalePercent(pct);
  SaveStereoScaleFile(pct);
  Log("StereoScale: %.2f (F6) — HIGHER = stronger 3D / smaller world (cap 1.30)", pct / 100.f);
}

}  // namespace asi
