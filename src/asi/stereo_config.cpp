#include "stereo_config.h"
#include "game_timer.h"
#include "hud_layout.h"
#include "log.h"
#include "vr_display.h"

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
// L4D2VR default VRScale ≈ 1.0 (user tunes). 100% = standard. 6DoF lean only (.scale file).
std::atomic<float> g_worldScale{1.0f};
std::atomic<bool> g_loggedScale{false};
// Soft stereo disparity (not WorldScale). 115% ≈ reclaim size without 150%×IPD break.
std::atomic<float> g_stereoScale{1.15f};
std::atomic<bool> g_loggedStereoScale{false};
// Live FOV ADD (CCam+0x60). F7 worldscale presets update this without restart.
std::atomic<float> g_fovAdd{0.f};
std::atomic<bool> g_fovAddLoaded{false};
std::atomic<int> g_worldScalePreset{-1};
std::atomic<bool> g_loggedWorldScalePreset{false};
// Eye-canvas maxDim (F5). Comfort path default 1536. Gen bump unlocks RT size lock.
std::atomic<int> g_canvasMaxDim{1536};
std::atomic<uint32_t> g_canvasMaxDimGen{0};

// F7 visual world-scale: fovadd + stereoscale (NOT SoftInset / LetterboxPrefer).
// Mode120 true-FOV canvas: HIGHER fovadd widens gameTan → StretchRect SRC crop when
// fill>100% → zoom-IN / giant-monitor (log: fovadd=18 → fill≈150%h on G2).
// DEZOOM = LOWER fovadd (less H crop, toward coverTan match ~fovadd 6) + HIGHER
// stereoscale (smaller world). Cap stereoscale 130 (150%×IPD broke fusion).
struct WorldScalePreset {
  const char* name;
  int fovAdd;
  int stereoPct;
};

constexpr WorldScalePreset kWorldScalePresets[] = {
    {"CropMax", 28, 115},   // old Ultra — heaviest H crop / zoom-IN
    {"Crop", 24, 118},      // still crop-heavy
    {"TallFill", 20, 120},  // V near full; H crop ~155%
    {"Mild18", 18, 120},    // prior default-ish; fill≈150%h
    {"Soft16", 16, 122},
    {"Open12", 12, 125},    // DEFAULT — less crop than old Out22
    {"Room8", 8, 128},      // near H-match; more peripheral BB
    {"MatchH6", 6, 130},    // ~100%h coverTan match on G2 16:9
    {"Air4", 4, 130},       // slight under-fill; max stereo size
    {"Window0", 0, 130},    // no fovadd; max stereo (may show V bars)
};
constexpr int kWorldScalePresetN =
    static_cast<int>(sizeof(kWorldScalePresets) / sizeof(kWorldScalePresets[0]));
constexpr int kWorldScaleDefaultIdx = 5;  // Open12

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

void SetSepCmFloat(float cm) {
  if (cm < 0.f)
    cm = 0.f;
  if (cm > 500.f)
    cm = 500.f;
  g_sepM.store(cm / 100.f);
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

void SaveIpdFileFloat(float cm) {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.ipd");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  std::fprintf(f, "%.2f\n", cm);
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

void SetStereoScalePercent(int pct) {
  if (pct < 100)
    pct = 100;
  // Mode 126–135 may use stereoscale 135 (stronger disparity / smaller world). 150%×IPD broke fusion.
  const StereoMode m = GetStereoMode();
  const int cap =
      (IsCleanDualFpsLiveLook(m) || IsCleanDualFpsHoldStereo(m) || IsCleanDualLrSync(m) ||
       IsCleanDualMonoLook(m) || IsCleanDualMonoLookPitch(m) || IsCleanDualMonoLookHmdEvery(m) ||
       IsCleanDualMonoLookHmdCheap(m) || IsCleanDualMonoLookBack132(m) ||
       IsCleanDualAlwaysFresh(m))
          ? 140
          : 130;
  if (pct > cap)
    pct = cap;
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
  if (pct < 100 || pct > 140)
    return;
  SetStereoScalePercent(pct);
  if (!g_loggedStereoScale.exchange(true))
    Log("StereoScale: %.2f (file) — F6: higher = stronger 3D / smaller feel (cap 130%%; Mode126≤140)",
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
    SetWorldScalePercent(150);
    SaveScaleFile(150);
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
  int pct = 0;
  if (sscanf_s(buf, "%d", &pct) != 1)
    return;
  if (pct < 25 || pct > 1000)
    return;
  SetWorldScalePercent(pct);
  if (!g_loggedScale.exchange(true))
    Log("WorldScale6DoF: %.2f (file gtaiv_dxvk_vr.scale) — lean always; Mode187+ also "
        "×IPD (UEVR size; F11 cycle); F7 = fovadd/stereoscale presets",
        pct / 100.f);
}

void SetFovAddDegrees(float deg) {
  if (deg < 0.f)
    deg = 0.f;
  if (deg > 40.f)
    deg = 40.f;
  g_fovAdd.store(deg);
  g_fovAddLoaded.store(true);
}

void SaveFovAddFile(int deg) {
  if (deg < 0)
    deg = 0;
  if (deg > 40)
    deg = 40;
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.fovadd");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  std::fprintf(f, "%d\n", deg);
  std::fclose(f);
}

void SaveWorldScalePresetIndex(int idx) {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "gtaiv_dxvk_vr.worldscale");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  std::fprintf(f, "%d\n", idx);
  std::fclose(f);
}

void ApplyWorldScalePreset(int idx, bool fromHotkey) {
  if (idx < 0 || idx >= kWorldScalePresetN)
    idx = kWorldScaleDefaultIdx;
  const WorldScalePreset& p = kWorldScalePresets[idx];
  SetFovAddDegrees(static_cast<float>(p.fovAdd));
  SaveFovAddFile(p.fovAdd);
  SetStereoScalePercent(p.stereoPct);
  SaveStereoScaleFile(p.stereoPct);
  g_worldScalePreset.store(idx);
  SaveWorldScalePresetIndex(idx);
  g_loggedStereoScale.store(true);
  if (fromHotkey || !g_loggedWorldScalePreset.exchange(true)) {
    Log("F7 WorldScale: [%d/%d] %s — fovadd=%d stereoscale=%d%% "
        "(DEZOOM=lower fovadd/less crop + higher stereo; no SoftInset; 6DoF=.scale)",
        idx + 1, kWorldScalePresetN, p.name, p.fovAdd, p.stereoPct);
  }
}

void ReloadWorldScaleVisual() {
  char path[MAX_PATH]{};
  int idx = kWorldScaleDefaultIdx;
  if (GetAsiDir(path, MAX_PATH)) {
    strcat_s(path, "gtaiv_dxvk_vr.worldscale");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") == 0 && f) {
      char buf[16]{};
      const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      int v = -1;
      if (n > 0 && sscanf_s(buf, "%d", &v) == 1 && v >= 0 && v < kWorldScalePresetN)
        idx = v;
    }
  }
  ApplyWorldScalePreset(idx, false);
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
  float cm = 0.f;
  if (sscanf_s(buf, "%f", &cm) != 1)
    return;
  if (cm < 0.f || cm > 500.f)
    return;
  SetSepCmFloat(cm);
  if (!g_loggedIpd.exchange(true)) {
    if (cm < 0.05f)
      Log("StereoSep: %.2f cm (file) — near-zero parallax. Set gtaiv_dxvk_vr.ipd higher "
          "(or F8) for stronger 3D",
          cm);
    else
      Log("StereoSep: %.2f cm (file gtaiv_dxvk_vr.ipd) — F8 cycles 1,2,3,4,6,7,10", cm);
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

int ParseModeFile(const char* buf, size_t n) {
  // Full integer parse — do NOT prefer 2-digit first ("190" must not become 19).
  // Old digit-prefix tables stopped at 189 and made Mode190 load as Mode19 (mono).
  (void)n;
  int v = -1;
  if (sscanf_s(buf, "%d", &v) == 1 && v >= 0 && v < 1000)
    return v;
  return -1;
}

}  // namespace

void EnsureVrSquareCommandlineReady();
void EnsureFpProfileDefaults();
void ForceFpFovDegrees(int forward, int rear, int foot);
void EnsureVehicleCamOffDefaults();
void EnsureModelCenteredCamOffsets();  // Mode216: camoff/vehcamoff/eyefwd all zero
void ReloadCanvasMaxDim();
void SetCanvasMaxDim(uint32_t dim, bool fromHotkey);
void SetEyeForwardCm(int cm);

static void ApplyMode162SquareWideBase() {
  EnsureFpProfileDefaults();
  ForceFpFovDegrees(100, 100, 100);
  SetEyeForwardCm(0);
  ApplyWorldScalePreset(9, false);
  ReloadCanvasMaxDim();
  if (GetCanvasMaxDim() < 2048)
    SetCanvasMaxDim(2048, false);
  EnsureVrSquareCommandlineReady();
}

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
  if (v == static_cast<int>(StereoMode::HeadOwnedCamFullPose)) {
    v = static_cast<int>(StereoMode::HeadOwnedCamSpike);
    Log("StereoMode: requested 46 is DISABLED (post-load freeze); using protected mode 45");
  }
  int prev = g_mode.load();
  if (v >= 0 && (v <= 53 || IsCleanDualLookMove(static_cast<StereoMode>(v)) ||
                 IsExternalFpHost(static_cast<StereoMode>(v)) ||
                 IsHeadHideNativeOurs(static_cast<StereoMode>(v)) ||
                 IsTrueStereoFamily(static_cast<StereoMode>(v)))) {
    prev = g_mode.exchange(v);
    if (!g_loggedMode.exchange(true) || prev != v)
      Log("StereoMode: %d (file gtaiv_dxvk_vr.stereo)", v);
  } else if (v >= 0) {
    Log("StereoMode: REJECTED %d from file (kept %d) — this ASI build does not accept "
        "that mode. Deploy the matching out-asi build, or use a known mode (203/204).",
        v, prev);
  }

  // hud.dat is global — Mode 166/167 write VR inset; other modes restore stock.
  if (IsOursFpHudLayout(static_cast<StereoMode>(v))) {
    // Ensure runs below on mode-enter.
  } else {
    RestoreHudDatStock();
  }

  // Apply once when entering the mode (not every reload).
  if (prev != v && (IsExternalFpHost(static_cast<StereoMode>(v)) ||
                    IsHeadHideNativeOurs(static_cast<StereoMode>(v)))) {
    ApplyGeometryCanvasDefaults();
    ReloadIpdScale();
    ReloadStereoScale();
    if (v == static_cast<int>(StereoMode::TrueStereoCamRightIpdFlip)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      {
        const float ipdCm = ReadHmdIpdMeters() * 100.f;
        SetSepCmFloat(ipdCm);
        SaveIpdFileFloat(ipdCm);
      }
      SetStereoScalePercent(100);
      SaveStereoScaleFile(100);
      Log("Mode240: TRUESTEREO CAM-RIGHT-IPD-FLIP on 203 seam (@0x4D8BF0) — Mode239 + "
          "swapped ±half sign (pivot OK on 239, fuse dead both ways = likely inverted). "
          "kill=239");
    } else if (v == static_cast<int>(StereoMode::TrueStereoCamRightIpd)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      {
        const float ipdCm = ReadHmdIpdMeters() * 100.f;
        SetSepCmFloat(ipdCm);
        SaveIpdFileFloat(ipdCm);
      }
      SetStereoScalePercent(100);
      SaveStereoScaleFile(100);
      Log("Mode239: TRUESTEREO CAM-RIGHT-IPD on 203 seam (@0x4D8BF0) — Mode236 + ±half "
          "sep along post-yaw CAMERA right only (parallel; no HMD-right, no EyeToHead). "
          "kill=237");
    } else if (v == static_cast<int>(StereoMode::TrueStereoUevrIpd)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      {
        const float ipdCm = ReadHmdIpdMeters() * 100.f;
        SetSepCmFloat(ipdCm);
        SaveIpdFileFloat(ipdCm);
      }
      SetStereoScalePercent(100);
      SaveStereoScaleFile(100);
      Log("Mode238: TRUESTEREO UEVR-IPD on 203 seam (@0x4D8BF0) — Mode236 + EyeToHead in "
          "CAMERA local frame after stick yaw (praydog-style; no L4D2 HMD-right/EyeZ). "
          "kill=237");
    } else if (v == static_cast<int>(StereoMode::TrueStereoLateralIpd)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      {
        const float ipdCm = ReadHmdIpdMeters() * 100.f;
        SetSepCmFloat(ipdCm);
        SaveIpdFileFloat(ipdCm);
      }
      SetStereoScalePercent(100);
      SaveStereoScaleFile(100);
      Log("Mode237: TRUESTEREO LATERAL-IPD on 203 seam (@0x4D8BF0) — Mode236 LeftPublish + "
          "zero per-eye EyeZ forward (L/R share one pivot; left/right only). Jacket / "
          "stick-yaw sweet-spot fix. kill=236");
    } else if (v == static_cast<int>(StereoMode::TrueStereoLeftPublish)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      {
        const float ipdCm = ReadHmdIpdMeters() * 100.f;
        SetSepCmFloat(ipdCm);
        SaveIpdFileFloat(ipdCm);
      }
      SetStereoScalePercent(100);
      SaveStereoScaleFile(100);
      Log("Mode236: TRUESTEREO LEFT-PUBLISH on 203 seam (@0x4D8BF0) — Mode235 gate + "
          "publish gameTan from Left eye only (Right = REJECT eyeR). Stops L/R ~3%% "
          "Direct bounds flip-flop. kill=235");
    } else if (v == static_cast<int>(StereoMode::TrueStereoStablePublish)) {
      // Mode 235: Direct geometry + gated measured publish (no CTimer / no SameState latch).
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      {
        const float ipdCm = ReadHmdIpdMeters() * 100.f;
        SetSepCmFloat(ipdCm);
        SaveIpdFileFloat(ipdCm);
      }
      SetStereoScalePercent(100);
      SaveStereoScaleFile(100);
      Log("Mode235: TRUESTEREO STABLE-PUBLISH on 203 seam (@0x4D8BF0) — Mode233 Direct + "
          "reject wild measTan (near-square, [0.5,2.5], center, 25%% jump); keep last "
          "good gameTan; no fpfov raise on REJECT. kill=233");
    } else if (v == static_cast<int>(StereoMode::TrueStereoSameState)) {
      // §4.5: Direct geometry + pair yaw/eye latch + CTimer step=0 (exposure dt proxy).
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      {
        const float ipdCm = ReadHmdIpdMeters() * 100.f;
        SetSepCmFloat(ipdCm);
        SaveIpdFileFloat(ipdCm);
      }
      SetStereoScalePercent(100);
      SaveStereoScaleFile(100);
      Log("Mode234: TRUESTEREO SAME-STATE on 203 seam (@0x4D8BF0) — Mode233 Direct + "
          "pair latch (controller/vehicle yaw + ped eye) + CTimer step=0 across dual "
          "(exposure dt proxy). kill=233 — watch walk/audio like prior 234 on 204");
    } else if (v == static_cast<int>(StereoMode::TrueStereoDirect)) {
      // §4.4: Cover geometry + 1:1 BB→eye + float Submit bounds (canvas dead weight).
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      {
        const float ipdCm = ReadHmdIpdMeters() * 100.f;
        SetSepCmFloat(ipdCm);
        SaveIpdFileFloat(ipdCm);
      }
      SetStereoScalePercent(100);
      SaveStereoScaleFile(100);
      Log("Mode233: TRUESTEREO DIRECT on 203 seam (@0x4D8BF0) — 1:1 BB→eye (no canvas "
          "StretchRect), Submit float VRTextureBounds from measured tangents; "
          "under-cover falls back to 232 canvas. kill=232");
    } else if (v == static_cast<int>(StereoMode::TrueStereoCover)) {
      // §4.3: Exact geometry + raise fpfov until measTan >= cover (canvas crops only).
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      {
        const float ipdCm = ReadHmdIpdMeters() * 100.f;
        SetSepCmFloat(ipdCm);
        SaveIpdFileFloat(ipdCm);
      }
      SetStereoScalePercent(100);
      SaveStereoScaleFile(100);
      Log("Mode232: TRUESTEREO COVER on 203 seam (@0x4D8BF0) — Mode231 Exact + raise "
          "fpfov until measTan>=cover (StereoCalib); canvas crops only. kill=231");
    } else if (v == static_cast<int>(StereoMode::TrueStereoExact)) {
      // 203 seam + honest geometry: measured gameTan, HMD IPD, stereoscale 1.00.
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      {
        const float ipdCm = ReadHmdIpdMeters() * 100.f;
        SetSepCmFloat(ipdCm);
        SaveIpdFileFloat(ipdCm);
      }
      SetStereoScalePercent(100);
      SaveStereoScaleFile(100);
      Log("Mode231: TRUESTEREO EXACT on 203 seam (@0x4D8BF0) — measured gameTan "
          "(StereoCalib PUBLISH), sep=HMD IPD, stereoscale forced 1.00, no toe-in, "
          "worldscale not×baseline; black bars OK. kill=203");
    } else if (v == static_cast<int>(StereoMode::TrueStereoCalibProbe)) {
      // Mode 203 entry state — probe measures what 203 renders (203-seam retry).
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode230: TRUESTEREO PROBE on 203 seam (@0x4D8BF0) — Mode203 behavior + "
          "READ-ONLY projection measurement (log 'StereoCalib:'). Run 60s, read "
          "errH/errV. kill=203");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual204DeferredHead)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureModelCenteredCamOffsets();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode216: OURS+PARENT-DUAL-204-DEFERRED-HEAD — model-centered HEAD (no "
          "camoff/vehcamoff/eyefwd); kill=204");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual203CloseIpdToeIn)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      SetSepCmFloat(0.25f);
      SaveIpdFileFloat(0.25f);
      Log("Mode215: OURS+PARENT-DUAL-203-CLOSE-IPD+TOEIN — IPD 0.25cm + mild toe-in "
          "~1.5m; kill=214/203");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual203CloseIpdQtr)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      SetSepCmFloat(0.25f);
      SaveIpdFileFloat(0.25f);
      Log("Mode214: OURS+PARENT-DUAL-203-CLOSE-IPD-QTR — IPD 0.25cm (bit closer than "
          "213); kill=213/203");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual203CloseIpdHalf)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      SetSepCmFloat(0.5f);
      SaveIpdFileFloat(0.5f);
      Log("Mode213: OURS+PARENT-DUAL-203-CLOSE-IPD-HALF — IPD 0.5cm (bit closer than "
          "212); kill=212/203");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual203CloseIpd)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      SetSepCm(1);
      SaveIpdFile(1);
      Log("Mode212: MIXED — IPD 1cm getting there, wants closer; kill=203");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual203ToeInStrong)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode211: MIXED — strong toe-in still not fully fused; kill=203");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual203ToeIn)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode210: MIXED — toe-in ~1.5m mild near help, still not fused; kill=203");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual203Outer)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode209: REJECT — OUTER @0x4D8E80 same-image (no fusion); use 203; kill=203");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDualTry6)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode208: REJECT — mid-jmp @0x4DA2BA; use 204; kill=204");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDualTry5)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode207: REJECT — tiny @0x541DC0 same-image both eyes (no 3D/fuse); "
          "use 204; kill=204");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDualTry4)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode206: MIXED — no bounce/phone washout but HOT ~35+/ES + car freeze; "
          "prefer 204; kill=204");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDualTry3)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode205: REJECT — math helper @0x4DDD50 crashed after load; use 204; "
          "kill=204");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDualTry2)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      // Preserve user IPD (203 milestone proved IPD alone is not the near-double fix).
      Log("Mode204: OURS+PARENT-DUAL-TRY2 — Mode203 pose-latch dual on Mode198 parent "
          "fn=0x4DE020 (ret 0x4DE0DC thiscall-56); kill=203 MILESTONE");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual204CamRightFlip)) {
      // Mode204 fused seam + cam-right flip (same IPD win as 241 checkpoint).
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode242: Mode204 + CAM-RIGHT-IPD FLIP (@0x4DE020) — same L/R as checkpoint 241; "
          "kill=204 / 241");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual203CamRightFlipLateLatch)) {
      // Mode241 stereo + Mode136-style late-latch Submit (shared pose for L+R).
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode243: Mode241 + LATE-LATCH Submit_TextureWithPose (shared L/R pose at "
          "Submit; @0x4D8BF0 cam-right flip). kill=241 / 203");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual203CamRightFlip)) {
      // Stock 203 geometry + cam-right flipped IPD only (no TrueStereo measure/Direct).
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode241: Mode203 + CAM-RIGHT-IPD FLIP only (@0x4D8BF0) — no TrueStereo "
          "measure/Direct/publish (stops gameTan FOV spazz). Fuse L/R from Mode240. "
          "kill=203");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDualPose)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode203: MILESTONE — TRUE 3D VR checkpoint (pose latch @0x4D8BF0); "
          "snapshot prebuilt/mode203; kill=200/191");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDualVs)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      SetSepCm(6);
      SaveIpdFile(6);
      Log("Mode202: REJECT — VS-translate fuses when still, blurs on look (not true "
          "same-tick view); use 203; kill=200/191");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDualFreeze)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode201: REJECT — cam freeze; use 202 VS dual or 200; kill=200/191");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentDual)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode200: PASS smooth — parent dual CCam±IPD; fusion weak → try 202; kill=191");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentCount)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode199: PASS — Mode191 dual + COUNT @0x4D8BF0 avg~1/ES; next=200 parent dual; "
          "kill=191");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickParentProbe)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode198: OURS+SAME-TICK-PARENT-PROBE — Mode191 dual + ONE VsRet stack "
          "sample/EndScene (hunt ~1x/frame above 0x220D0; no VQ storm); kill=191");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickOwnerCount)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode197: FAILED — 0x220D0 is per-draw hot (COUNT trampoline worse FPS + "
          "flicker vs 191); hook NOT installed; behaves as Mode191; kill=191");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickOwnerObserve)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode196: DONE/DISABLED observe — use 197 COUNT or 191; prior 1FPS+crash; "
          "kill=191");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickDrawRight)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode194: OURS+SAME-TICK-DRAW-RIGHT — Left BuildRootA + Right "
          "DrawScene+PushLiveCam; distinct L/R angles but NO fusion; kill=191/192");
    } else if (v == static_cast<int>(StereoMode::OursFpHeadBoneThiscall)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode193: OURS+HEAD-BONE — pair-freeze + MONO DrawScene×1 (door-safe; weak "
          "parallax); kill=187");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickPhaseRight)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode192: OURS+SAME-TICK-PHASE-RIGHT — Mode191 Left BuildRootA + Right "
          "PhaseTriplet only (cut dual post/sky); kill=191 MILESTONE");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickBuildDual)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode191: MILESTONE LKG — BuildRootA x2 L/R CopyMat (no CodePause; audio OK); "
          "flicker when looking up — kill=187/185");
    } else if (v == static_cast<int>(StereoMode::OursFpHeadBoneReturn)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode190: FAILED HEAD-BONE — ScriptHook natives from CopyMat crash after "
          "load; bone OFF (=Mode187). Kill=187/186");
    } else if (v == static_cast<int>(StereoMode::OursFpSameTickDual)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      ResolveGameTimer();
      Log("Mode189: OURS+SAME-TICK — Mode187 + CTimer step=0 + ms pin + CodePause "
          "+ full cam freeze across L/R dual; kill=187/185");
    } else if (v == static_cast<int>(StereoMode::OursFpHeadBoneWorldScale)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);
      SaveScaleFile(100);
      Log("Mode188: FAILED HEAD-BONE — out-ptr path; use stereo=190 instead; kill=187");
    } else if (v == static_cast<int>(StereoMode::OursFpTrueWorldScale)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      SetWorldScalePercent(100);  // start matching Mode186; F11 raises size feel
      SaveScaleFile(100);
      Log("Mode187: OURS+TRUE-WORLDSCALE — Mode186 + scale×IPD (UEVR size; F11 cycle "
          "100/125/150/175/200); kill=186/185");
    } else if (v == static_cast<int>(StereoMode::OursFpZoomOut)) {
      ApplyMode162SquareWideBase();
      ForceFpFovDegrees(110, 110, 110);  // one change vs Mode185 (fpfov 100→110)
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode186: OURS+ZOOM-OUT — Mode185 PhaseRight dual + fpfov=110 (near objects "
          "smaller; not canvas zoom); kill=185/170");
    } else if (v == static_cast<int>(StereoMode::OursFpPhaseRightDual)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode185: OURS+PHASE-RIGHT-DUAL — Mode170 dual+canvas + Right via "
          "PhaseA→PhaseC→DrawScene; kill=170");
    } else if (v == static_cast<int>(StereoMode::OursFpTimeFreeze)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode184: FAILED TIME-FREEZE — freeze armed, flicker unchanged; poke OFF "
          "(settings crash risk); kill=170");
    } else if (v == static_cast<int>(StereoMode::OursFpPhoneCanvasInset)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode183: OURS+PHONE-CANVAS-INSET — Mode170 dual+canvas + soft BB crop "
          "right12%%/bottom18%% (phone up+left; no Submit UV / no SET_MOBILE); kill=170/175");
    } else if (v == static_cast<int>(StereoMode::OursFpPhoneStereo)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode180: FAILED Submit-UV-on-canvas — prefer 183 / 175; kill=170");
    } else if (v == static_cast<int>(StereoMode::OursFpNoColorFill)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode179: OURS+NO-COLORFILL — Mode178 full cam freeze + skip canvas ColorFill "
          "(keep cover crop); kill=178/170");
    } else if (v == static_cast<int>(StereoMode::OursFpSameFrameFreeze)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode178: OURS+SAME-FRAME-FREEZE — Mode170 dual+canvas + freeze FULL pre-IPD "
          "cam (basis+pos) across L/R; kill=170/167");
    } else if (v == static_cast<int>(StereoMode::OursFpAer)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode177: OURS+AER — FAILED (flicker/warp); prefer 178; kill=175/170/167");
    } else if (v == static_cast<int>(StereoMode::OursFpStereoSimple)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode176: OURS+STEREO-SIMPLE — FAILED (flicker back, no 3D); prefer 177 AER / 175; "
          "kill=175/170/167");
    } else if (v == static_cast<int>(StereoMode::OursFpBbPhoneNudge)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode175: OURS+BB-PHONE-NUDGE — phone GOOD; next=176 stereo-simple; "
          "kill=174/170/167");
    } else if (v == static_cast<int>(StereoMode::OursFpBbPhone)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode174: OURS+BB-MONO+PHONE — FLICKER GONE; phone still low/right → prefer 175; "
          "kill=173/170/167");
    } else if (v == static_cast<int>(StereoMode::OursFpSameLPhone)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode173: OURS+SAME-L+PHONE — still flickered sameL=1 → prefer 174 BB path; "
          "kill=172/170/167");
    } else if (v == static_cast<int>(StereoMode::OursFpMonoPair)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode172: OURS+MONO-PAIR — still flickered (Left-crop→Right eye); prefer 173; "
          "kill=171/170/167");
    } else if (v == static_cast<int>(StereoMode::OursFpPairFreeze)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode171: OURS+PAIR-FREEZE — Mode170 everyN=1 + freeze ped eye ORIGIN "
          "across L/R DrawScene; still flickers on foot → prefer 172; kill=170/167");
    } else if (v == static_cast<int>(StereoMode::OursFpFreshDual)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode170: OURS+FRESH-DUAL — Mode169 + everyN=1 (no off-tick HOLD), "
          "still NO Flush, hitch→HOLD; driving flicker → prefer 171; kill=169/167");
    } else if (v == static_cast<int>(StereoMode::OursFpSafeFlicker)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode169: OURS+SAFE-FLICKER — Mode167 cam/HUD + safe dual "
          "(dualn=2, atomic L/R, no skip-gate, NO Flush, hitch→HOLD last stereo); "
          "HMD stutter from HOLD — prefer 170; kill=167/162 (168=DEVICE_LOST)");
    } else if (v == static_cast<int>(StereoMode::OursFpStableCarHud)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode168: OURS+STABLE+CAR+HUD — FAILED DEVICE_LOST (everyN=1+Flush); "
          "prefer Mode169; kill=167/162");
    } else if (v == static_cast<int>(StereoMode::OursFpCarHud)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      EnsureHudOffDefaults();
      Log("Mode167: OURS+CAR+HUD — Mode165 cam (FindPlayerVehicle sit) + Mode166 HUD inset "
          "(+mission directions); kill=165/166/162");
    } else if (v == static_cast<int>(StereoMode::OursFpHudLayout)) {
      ApplyMode162SquareWideBase();
      EnsureHudOffDefaults();
      Log("Mode166: OURS+HUD-LAYOUT — inset corners→inward (top stronger); live mem "
          "poke (no menu jump); leave166 restores stock hud.dat");
    } else if (v == static_cast<int>(StereoMode::OursFpEnterCarFp)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      Log("Mode165: OURS+ENTER-CAR-FP — keep FP during enter + Mode164 in-car head; "
          "FindPlayerVehicle sit (clears on foot); flash gate ON; kill=164/162");
    } else if (v == static_cast<int>(StereoMode::OursFpInCarHead)) {
      ApplyMode162SquareWideBase();
      EnsureVehicleCamOffDefaults();
      Log("Mode164: OURS+IN-CAR-HEAD — eye further back in seat (vehcamoff default 0 -12 0); "
          "flash gate ON; kill=163/162");
    } else if (v == static_cast<int>(StereoMode::OursFpFlashGate)) {
      ApplyMode162SquareWideBase();
      Log("Mode163: OURS+FLASH-GATE — skip only tiny env RT (maxDim<=512); "
          "do NOT skip half-BB fog/LOD buffers; kill=162");
    } else if (v == static_cast<int>(StereoMode::OursFpSquareWideFov)) {
      ApplyMode162SquareWideBase();
      Log("Mode162: OURS+SQUARE-WIDE-FOV — square-aspect + overscan=0%% + fpfov=100 "
          "(zoom-out vs 161@90; no bars); kill=161/160/158");
    } else if (v == static_cast<int>(StereoMode::OursFpSquareZeroOverscan)) {
      EnsureFpProfileDefaults();
      ForceFpFovDegrees(90, 90, 90);
      SetEyeForwardCm(0);
      ApplyWorldScalePreset(9, false);
      ReloadCanvasMaxDim();
      if (GetCanvasMaxDim() < 2048)
        SetCanvasMaxDim(2048, false);
      EnsureVrSquareCommandlineReady();
      Log("Mode161: OURS+SQUARE-ZERO-OS — square-aspect + overscan=0%% (exact fit A/B); "
          "fpfov=90; kill=160/158/120");
    } else if (v == static_cast<int>(StereoMode::OursFpSquareLowOverscan)) {
      EnsureFpProfileDefaults();
      SetEyeForwardCm(0);
      ApplyWorldScalePreset(9, false);
      ReloadCanvasMaxDim();
      if (GetCanvasMaxDim() < 2048)
        SetCanvasMaxDim(2048, false);
      EnsureVrSquareCommandlineReady();
      Log("Mode160: OURS+SQUARE-LOW-OS — square-aspect + overscan≈2%% "
          "(less crop than 159; keep commandline.txt 1440sq); kill=158/159/120");
    } else if (v == static_cast<int>(StereoMode::OursFpStrongOverscan)) {
      EnsureFpProfileDefaults();
      SetEyeForwardCm(0);
      ApplyWorldScalePreset(9, false);
      ReloadCanvasMaxDim();
      if (GetCanvasMaxDim() < 2048)
        SetCanvasMaxDim(2048, false);
      EnsureVrSquareCommandlineReady();
      Log("Mode159: OURS+STRONG-OVERSCAN — Mode158 stack + overscan≈10%% "
          "(more bar shrink; 158=LKG); kill=158/157/120");
    } else if (v == static_cast<int>(StereoMode::OursFpSquareRes)) {
      EnsureFpProfileDefaults();
      SetEyeForwardCm(0);
      ApplyWorldScalePreset(9, false);
      ReloadCanvasMaxDim();
      if (GetCanvasMaxDim() < 2048)
        SetCanvasMaxDim(2048, false);
      EnsureVrSquareCommandlineReady();
      Log("Mode158: OURS+SQUARE-RES — Mode157 overscan FOV; rename "
          "commandline.txt.vr-square -> commandline.txt then FULL restart "
          "(1440x1440; closer aspect = fewer bars); kill=157/156/120");
    } else if (v == static_cast<int>(StereoMode::OursFpMildOverscan)) {
      EnsureFpProfileDefaults();
      SetEyeForwardCm(0);
      ApplyWorldScalePreset(9, false);
      ReloadCanvasMaxDim();
      if (GetCanvasMaxDim() < 2048)
        SetCanvasMaxDim(2048, false);
      Log("Mode157: OURS+OVERSCAN — Mode156 cam/FOV + under-publish overscan≈6%% "
          "(shrink bars; tiny edge crop; keep aspect); kill=156/154/120");
    } else if (v == static_cast<int>(StereoMode::OursFpEyeCenterLowRes)) {
      EnsureFpProfileDefaults();
      SetEyeForwardCm(0);
      ApplyWorldScalePreset(9, false);
      // Highest fixed eye-canvas unless gtaiv_dxvk_vr.vres already set.
      ReloadCanvasMaxDim();
      if (GetCanvasMaxDim() < 2048)
        SetCanvasMaxDim(2048, false);
      Log("Mode156: OURS+EYE-LOW+VRES — Mode154 aspect-fit; ped-fixed eyeH=65cm pedFwd=8cm; "
          "F5 cycles eye-canvas maxDim; FirstPerson OFF; kill=154/155/120");
    } else if (v == static_cast<int>(StereoMode::OursFpEyeCenterCam)) {
      EnsureFpProfileDefaults();
      SetEyeForwardCm(0);  // look-axis eyefwd ignored in ApplyHmdToCam for 155
      ApplyWorldScalePreset(9, false);
      Log("Mode155: OURS+EYE-CENTER — Mode154 aspect-fit FOV; ped-fixed pivot "
          "(no 6DoF lean; eyeH=78cm pedFwd=8cm); FirstPerson OFF; kill=154/153/120");
    } else if (v == static_cast<int>(StereoMode::OursFpWideAspectFit)) {
      EnsureFpProfileDefaults();
      SetEyeForwardCm(0);
      ApplyWorldScalePreset(9, false);
      Log("Mode154: OURS+FP-WIDE aspect-fit — CCam=fpfov; gameTan=FIT(BB aspect→cover); "
          "fixes 153 squeeze/V-stretch; NOT square cmdline; kill=153/152/120");
    } else if (v == static_cast<int>(StereoMode::OursFpWideUnderPublish)) {
      EnsureFpProfileDefaults();
      SetEyeForwardCm(0);
      // Under-publish: CCam=fpfov widens BB; canvas locked to cover (not from CCam).
      // fovadd unused for absolute write — keep Window0 file clean.
      ApplyWorldScalePreset(9, false);
      Log("Mode153: OURS+FP-WIDE under-publish — CCam=fpfov (widen BB); gameTan=COVER "
          "(no CCam publish); eyefwd=0; FirstPerson OFF; kill=152/120/0");
    } else if (v == static_cast<int>(StereoMode::OursFpFovProfile)) {
      EnsureFpProfileDefaults();
      // FirstPerson presence: FOV 90 was script-cam; our CCam=90 caused crop zoom-IN.
      // Match "see jacket": eyefwd=0 (not 42 head-embed) + Window0 fovadd=0. F7 works again.
      SetEyeForwardCm(0);
      ApplyWorldScalePreset(9, false);  // Window0 — fovadd=0 stereoscale=130
      Log("Mode152: OURS+FP-PRESENCE — eyefwd=0 (look down→jacket like FirstPerson); "
          "fovadd=0 Window0 (F7 live); native hide; FirstPerson.asi OFF; kill=120/0");
    } else if (v == static_cast<int>(StereoMode::HeadHideNativeOurs)) {
      Log("Mode151: HEADHIDE-OURS — Mode120 dual + our HMD cam + native SET_DRAW hide; "
          "rename FirstPerson.asi→.off; kill=120/0");
    } else if (v == static_cast<int>(StereoMode::HeadHideNativeWithFp)) {
      Log("Mode150: HEADHIDE+FP — Mode147 soft-move + native SET_DRAW hide (FP cam kept); "
          "kill=147/0");
    } else if (v == static_cast<int>(StereoMode::ExternalFpHostSoftMoveStick)) {
      Log("Mode148: SOFT-MOVE stick-alt — Mode147 without stick invert; kill=147/144/0");
    } else if (v == static_cast<int>(StereoMode::ExternalFpHostSoftMove)) {
      Log("Mode147: SOFT-MOVE — mouse cam (144 signs, slow pitch); ped heading floats ONLY "
          "(no matrix); walk look-dir; stick LR invert fix; kill=144/0");
    } else if (v == static_cast<int>(StereoMode::ExternalFpHostLookMoveYaw)) {
      Log("Mode146: EXTERNAL-FP LOOKMOVE+YAW — Mode145 + half mouse yaw; kill=145/144/0");
    } else if (v == static_cast<int>(StereoMode::ExternalFpHostLookMove)) {
      Log("Mode145: EXTERNAL-FP LOOKMOVE — Mode144 signs; ped faces HMD (walk look-dir); "
          "mouse pitch-only + slower pitch + deadzone; need HMD=0; kill=144/0/120");
    } else if (v == static_cast<int>(StereoMode::ExternalFpHostLookPitchAlt)) {
      Log("Mode144: EXTERNAL-FP PITCH-ALT — Mode143 clamp/rate; Mode142 pitch sign; "
          "yaw=142; need FirstPerson HMD=0; kill=143/142/0");
    } else if (v == static_cast<int>(StereoMode::ExternalFpHostLookPitch)) {
      Log("Mode143: EXTERNAL-FP PITCH — yaw=142 (−); pitch unlock (sign flip + per-axis "
          "clamp + high pitch); need FirstPerson HMD=0 (not Oculus); kill=142/140/0");
    } else if (v == static_cast<int>(StereoMode::ExternalFpHostLookAlt)) {
      Log("Mode142: EXTERNAL-FP LOOK-ALT — Mode141 pitch/rate; OPPOSITE yaw vs 141; "
          "FirstPerson cam/hide/FOV kept; kill=141/140/0");
    } else if (v == static_cast<int>(StereoMode::ExternalFpHostLookFix)) {
      Log("Mode141: EXTERNAL-FP LOOK-FIX — yaw un-invert + every-frame + fast pitch "
          "(FirstPerson GET_MOUSE_INPUT path); dual IPD; kill=140/0/120");
    } else {
      Log("Mode140: EXTERNAL-FP HOST + DUAL — FirstPerson owns cam/hide/FOV; "
          "our ASI DrawScene×2 IPD-only (no HMD CopyMat); HMD→mouse legacy; dualn like 120; "
          "Mode120 frozen; kill stereo=0 or 120");
    }
  } else if (prev != v && v == static_cast<int>(StereoMode::StereoFusion)) {
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
                           v == static_cast<int>(StereoMode::HeadOwnedCamFullPose) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamLeveledPitchFlip) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamPitchStable) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamPedCoupled) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamStereoAlways) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamStereoAer) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamStereoSwap) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamStereoSoftGuard) ||
                           IsCleanDualLookMove(static_cast<StereoMode>(v)))) {
    ApplyGeometryCanvasDefaults();
    ReloadIpdScale();
    ReloadWorldScale();
    if (IsCleanDualLookMove(static_cast<StereoMode>(v))) {
      ReloadWorldScaleVisual();  // F7 presets (fovadd+stereoscale); default Out
      // Mode 123/125: MatchH6. Mode 126–135: Window0 (stronger dezoom) then cover refine.
      if (v == static_cast<int>(StereoMode::CleanDualFpsLiveLook) ||
          v == static_cast<int>(StereoMode::CleanDualFpsPoseTight) ||
          v == static_cast<int>(StereoMode::CleanDualFpsHoldStereo) ||
          v == static_cast<int>(StereoMode::CleanDualLrSync) ||
          v == static_cast<int>(StereoMode::CleanDualMonoLook) ||
          v == static_cast<int>(StereoMode::CleanDualMonoLookPitch) ||
          v == static_cast<int>(StereoMode::CleanDualMonoLookHmdEvery) ||
          v == static_cast<int>(StereoMode::CleanDualMonoLookHmdCheap) ||
          v == static_cast<int>(StereoMode::CleanDualMonoLookBack132) ||
          v == static_cast<int>(StereoMode::CleanDualAlwaysFresh)) {
        ApplyWorldScalePreset(9, false);  // Window0 — fovadd=0 stereoscale=130
        if (v == static_cast<int>(StereoMode::CleanDualAlwaysFresh))
          Log("Mode135: ALWAYS-FRESH default → Window0; cover refine + stereoscale 135; "
              "look→BB→L AND BB→R EVERY (low thresh); calm everyN=1 dual; POSEHOLD→fresh "
              "monolook (never bare HOLD); hitch→LIVELOOK; not same-tex; kill=120");
        else if (v == static_cast<int>(StereoMode::CleanDualMonoLookBack132))
          Log("Mode134: FAILED HOLD freeze — prefer 135; Window0; cover refine + stereoscale 135; "
              "look→BB→L AND BB→R EVERY EndScene (pitch≥2.5° + sustain3; ≥600ms hyst); "
              "NOT same-tex Submit (133 FAILED); calm dualn=3 + POSEHOLD last stereo; hitch 32");
        else if (v == static_cast<int>(StereoMode::CleanDualMonoLookHmdCheap))
          Log("Mode133: FAILED same-tex — prefer 134/132; Window0; cover refine + stereoscale 135; "
              "look→BB→L only + Submit same tex L+R EVERY EndScene (pitch≥2.5° + "
              "sustain3; ≥600ms hyst); calm dualn=3 + POSEHOLD last stereo; hitch 32");
        else if (v == static_cast<int>(StereoMode::CleanDualMonoLookHmdEvery))
          Log("Mode132: MONOLOOK-HMD-EVERY default → Window0; cover refine + stereoscale 135; "
              "look→identical BB both eyes (pitch≥2.5° + sustain3; ≥600ms hyst; "
              "BB→both EVERY EndScene); calm dualn=3 + POSEHOLD last stereo; hitch 32");
        else if (v == static_cast<int>(StereoMode::CleanDualMonoLookPitch))
          Log("Mode131: MONOLOOK-PITCH default → Window0; cover refine + stereoscale 135; "
              "look→identical BB both eyes (pitch≥2.5° + sustain3; ≥600ms hyst; "
              "BB refresh every 3); calm dualn=3 + POSEHOLD last stereo; hitch 32");
        else if (v == static_cast<int>(StereoMode::CleanDualMonoLook))
          Log("Mode130: MONOLOOK default → Window0; cover refine + stereoscale 135; "
              "look→identical BB both eyes ≥350ms hyst; calm dualn=3 HOLD; "
              "pose extends mono; hitch 32; no LOOK-FORCE dual");
        else if (v == static_cast<int>(StereoMode::CleanDualLrSync))
          Log("Mode129: L/R SYNC default → Window0; cover refine + stereoscale 135; "
              "look→force same-tick dual; calm dualn=3 HOLD; pose→mono BB ≥200ms "
              "hyst; hitch 32; no mismatched HOLD pair");
        else if (v == static_cast<int>(StereoMode::CleanDualFpsHoldStereo))
          Log("Mode128: FPS NO-LIVELOOK default → Window0; cover refine + "
              "stereoscale 135; POSEHOLD≥12ms/5 + HARD≥24ms/8 HOLD last stereo; "
              "dualn=3; hitch 32; no look-rate LIVELOOK");
        else if (v == static_cast<int>(StereoMode::CleanDualFpsPoseTight))
          Log("Mode127: FPS+LIVELOOK TIGHT default → Window0; cover refine + "
              "stereoscale 135; POSEHOLD≥12ms/5 + HARD≥24ms/8; dualn=3; hitch 32");
        else
          Log("Mode126: FPS+LIVELOOK default → Window0 (fovadd=0); cover refine + "
              "stereoscale 135; POSEHOLD; live BB both eyes on look");
      } else if (v == static_cast<int>(StereoMode::CleanDualFillMatch) ||
                 v == static_cast<int>(StereoMode::CleanDualPerfFillCombo)) {
        ApplyWorldScalePreset(7, false);  // MatchH6 — ~100%h cover on G2
        Log("Mode%d: FILLMATCH default → MatchH6 (fovadd=6 stereoscale=130); "
            "cover refine via TryApplyCoverMatchedFovAdd",
            v);
      } else if (v == static_cast<int>(StereoMode::CleanDualZoomScale)) {
        ApplyWorldScalePreset(7, false);  // start dezoom zone (MatchH6)
        Log("Mode124: ZOOMSCALE default → MatchH6; F7 still cycles full ladder "
            "(DEZOOM = lower fovadd)");
      } else if (v == static_cast<int>(StereoMode::CleanDualLateLatch)) {
        Log("Mode136: LATE-LATCH → Mode120 content (dualn=2 HOLD, no monolook); "
            "every Submit TextureWithPose pose sampled immediately before Submit; "
            "kill=120");
      }
    } else
      ReloadStereoScale();
  } else {
    ReloadIpdScale();
    ReloadWorldScale();
    if (IsCleanDualLookMove(static_cast<StereoMode>(v))) {
      ReloadWorldScaleVisual();
      if (v == static_cast<int>(StereoMode::CleanDualFpsLiveLook) ||
          v == static_cast<int>(StereoMode::CleanDualFpsPoseTight) ||
          v == static_cast<int>(StereoMode::CleanDualFpsHoldStereo) ||
          v == static_cast<int>(StereoMode::CleanDualLrSync) ||
          v == static_cast<int>(StereoMode::CleanDualMonoLook) ||
          v == static_cast<int>(StereoMode::CleanDualMonoLookPitch) ||
          v == static_cast<int>(StereoMode::CleanDualMonoLookHmdEvery) ||
          v == static_cast<int>(StereoMode::CleanDualMonoLookHmdCheap) ||
          v == static_cast<int>(StereoMode::CleanDualMonoLookBack132) ||
          v == static_cast<int>(StereoMode::CleanDualAlwaysFresh))
        ApplyWorldScalePreset(9, false);
      else if (v == static_cast<int>(StereoMode::CleanDualFillMatch) ||
               v == static_cast<int>(StereoMode::CleanDualPerfFillCombo))
        ApplyWorldScalePreset(7, false);
      else if (v == static_cast<int>(StereoMode::CleanDualZoomScale))
        ApplyWorldScalePreset(7, false);
      // Mode 136: keep Mode120 worldscale visual (no Window0 / cover force).
    } else
      ReloadStereoScale();
  }
}

void WriteStereoModeFile(int mode) {
  if (mode < 0 ||
      (mode > 53 && !IsCleanDualLookMove(static_cast<StereoMode>(mode)) &&
       !IsExternalFpHost(static_cast<StereoMode>(mode)) &&
       !IsHeadHideNativeOurs(static_cast<StereoMode>(mode))))
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
         mode == StereoMode::HeadOwnedCamSpike ||
         mode == StereoMode::HeadOwnedCamFullPose ||
         mode == StereoMode::HeadOwnedCamLeveledPitchFlip ||
         mode == StereoMode::HeadOwnedCamPitchStable ||
         mode == StereoMode::HeadOwnedCamPedCoupled ||
         mode == StereoMode::HeadOwnedCamStereoAlways ||
         mode == StereoMode::HeadOwnedCamStereoAer ||
         mode == StereoMode::HeadOwnedCamStereoSwap ||
         mode == StereoMode::HeadOwnedCamStereoSoftGuard ||
         IsCleanDualLookMove(mode);
}

bool UsesMotionGuardStereo(StereoMode mode) {
  // Mode 120/121: motion-guard OFF (tight guard killed FPS with StretchRects).
  if (IsCleanDualLookMove(mode))
    return false;
  return mode == StereoMode::FovCanvasMotionGuard ||
         mode == StereoMode::ReplayCallChainProbe ||
         mode == StereoMode::ReplayOwnerCountProbe ||
         mode == StereoMode::FovCanvasMotionGuardFast ||
         mode == StereoMode::FovCanvasMotionGuardRtLock ||
         mode == StereoMode::HeadOwnedCamSpike ||
         mode == StereoMode::HeadOwnedCamFullPose ||
         mode == StereoMode::HeadOwnedCamLeveledPitchFlip ||
         mode == StereoMode::HeadOwnedCamPitchStable ||
         mode == StereoMode::HeadOwnedCamPedCoupled;
}

bool UsesStereoAlwaysDistinct(StereoMode mode) {
  // Mode 120 is same-frame dual, not AER temporal always-distinct.
  return mode == StereoMode::HeadOwnedCamStereoAlways ||
         mode == StereoMode::HeadOwnedCamStereoAer ||
         mode == StereoMode::HeadOwnedCamStereoSwap ||
         mode == StereoMode::HeadOwnedCamStereoSoftGuard;
}

bool UsesAerPoseSubmit(StereoMode mode) {
  // Mode 120: real WaitGetPoses+Submit_Default. Mode 136 uses late-latch path
  // (IsCleanDualLateLatch) separately — still Submit_TextureWithPose, but pose
  // is sampled immediately before Submit, not capture-time AER.
  // Mode 177: capture-time pose on the stale eye so SteamVR can reproject.
  return mode == StereoMode::AerPoseSubmit ||
         mode == StereoMode::HeadOwnedCamStereoAer ||
         mode == StereoMode::HeadOwnedCamStereoSoftGuard ||
         mode == StereoMode::OursFpAer;
}

bool UsesPedCoupledYaw(StereoMode mode) {
  // Mode 120: pedCoupled OFF — look_move owns body facing; cam is pure HMD.
  if (IsCleanDualLookMove(mode))
    return false;
  return mode == StereoMode::HeadOwnedCamPedCoupled || UsesStereoAlwaysDistinct(mode);
}

bool UsesPreCaptureCamRefresh(StereoMode mode) {
  return mode == StereoMode::HeadOwnedCamPitchStable ||
         mode == StereoMode::HeadOwnedCamPedCoupled || UsesStereoAlwaysDistinct(mode) ||
         IsCleanDualLookMove(mode);
}

bool UsesRtLockFovGate(StereoMode mode) {
  return mode == StereoMode::FovCanvasMotionGuardRtLock ||
         mode == StereoMode::HeadOwnedCamSpike ||
         mode == StereoMode::HeadOwnedCamFullPose ||
         mode == StereoMode::HeadOwnedCamLeveledPitchFlip ||
         mode == StereoMode::HeadOwnedCamPitchStable ||
         mode == StereoMode::HeadOwnedCamPedCoupled || UsesStereoAlwaysDistinct(mode) ||
         IsCleanDualLookMove(mode);
}

bool IsCleanDualLookMove(StereoMode mode) {
  return mode == StereoMode::CleanDualLookMove ||
         mode == StereoMode::CleanDualLookMoveDualN3 ||
         mode == StereoMode::CleanDualPerfLookHold ||
         mode == StereoMode::CleanDualFillMatch ||
         mode == StereoMode::CleanDualZoomScale ||
         mode == StereoMode::CleanDualPerfFillCombo ||
         mode == StereoMode::CleanDualFpsLiveLook ||
         mode == StereoMode::CleanDualFpsPoseTight ||
         mode == StereoMode::CleanDualFpsHoldStereo ||
         mode == StereoMode::CleanDualLrSync ||
         mode == StereoMode::CleanDualMonoLook ||
         mode == StereoMode::CleanDualMonoLookPitch ||
         mode == StereoMode::CleanDualMonoLookHmdEvery ||
         mode == StereoMode::CleanDualMonoLookHmdCheap ||
         mode == StereoMode::CleanDualMonoLookBack132 ||
         mode == StereoMode::CleanDualAlwaysFresh ||
         mode == StereoMode::CleanDualLateLatch;
}

bool IsCleanDualPerfLookHold(StereoMode mode) {
  return mode == StereoMode::CleanDualPerfLookHold ||
         mode == StereoMode::CleanDualPerfFillCombo;
}

bool IsCleanDualFillMatch(StereoMode mode) {
  return mode == StereoMode::CleanDualFillMatch ||
         mode == StereoMode::CleanDualPerfFillCombo ||
         mode == StereoMode::CleanDualFpsLiveLook ||
         mode == StereoMode::CleanDualFpsPoseTight ||
         mode == StereoMode::CleanDualFpsHoldStereo ||
         mode == StereoMode::CleanDualLrSync ||
         mode == StereoMode::CleanDualMonoLook ||
         mode == StereoMode::CleanDualMonoLookPitch ||
         mode == StereoMode::CleanDualMonoLookHmdEvery ||
         mode == StereoMode::CleanDualMonoLookHmdCheap ||
         mode == StereoMode::CleanDualMonoLookBack132 ||
         mode == StereoMode::CleanDualAlwaysFresh;
}

bool IsCleanDualZoomScale(StereoMode mode) {
  return mode == StereoMode::CleanDualZoomScale;
}

bool IsCleanDualPerfFillCombo(StereoMode mode) {
  return mode == StereoMode::CleanDualPerfFillCombo;
}

bool IsCleanDualFpsLiveLook(StereoMode mode) {
  return mode == StereoMode::CleanDualFpsLiveLook ||
         mode == StereoMode::CleanDualFpsPoseTight;
}

bool IsCleanDualFpsPoseTight(StereoMode mode) {
  return mode == StereoMode::CleanDualFpsPoseTight ||
         mode == StereoMode::CleanDualFpsHoldStereo ||
         mode == StereoMode::CleanDualLrSync ||
         mode == StereoMode::CleanDualMonoLook ||
         mode == StereoMode::CleanDualMonoLookPitch ||
         mode == StereoMode::CleanDualMonoLookHmdEvery ||
         mode == StereoMode::CleanDualMonoLookHmdCheap ||
         mode == StereoMode::CleanDualMonoLookBack132 ||
         mode == StereoMode::CleanDualAlwaysFresh;
}

bool IsCleanDualFpsHoldStereo(StereoMode mode) {
  return mode == StereoMode::CleanDualFpsHoldStereo;
}

bool IsCleanDualLrSync(StereoMode mode) {
  return mode == StereoMode::CleanDualLrSync;
}

bool IsCleanDualMonoLook(StereoMode mode) {
  return mode == StereoMode::CleanDualMonoLook;
}

bool IsCleanDualMonoLookPitch(StereoMode mode) {
  return mode == StereoMode::CleanDualMonoLookPitch;
}

bool IsCleanDualMonoLookHmdEvery(StereoMode mode) {
  // Mode 134 = explicit 132 twin (BB→L+R every; never same-tex).
  return mode == StereoMode::CleanDualMonoLookHmdEvery ||
         mode == StereoMode::CleanDualMonoLookBack132;
}

bool IsCleanDualMonoLookHmdCheap(StereoMode mode) {
  return mode == StereoMode::CleanDualMonoLookHmdCheap;
}

bool IsCleanDualMonoLookBack132(StereoMode mode) {
  return mode == StereoMode::CleanDualMonoLookBack132;
}

bool IsCleanDualAlwaysFresh(StereoMode mode) {
  return mode == StereoMode::CleanDualAlwaysFresh;
}

bool IsCleanDualLateLatch(StereoMode mode) {
  return mode == StereoMode::CleanDualLateLatch;
}

bool IsExternalFpHost(StereoMode mode) {
  return mode == StereoMode::ExternalFpHost ||
         mode == StereoMode::ExternalFpHostLookFix ||
         mode == StereoMode::ExternalFpHostLookAlt ||
         mode == StereoMode::ExternalFpHostLookPitch ||
         mode == StereoMode::ExternalFpHostLookPitchAlt ||
         mode == StereoMode::ExternalFpHostLookMove ||
         mode == StereoMode::ExternalFpHostLookMoveYaw ||
         mode == StereoMode::ExternalFpHostSoftMove ||
         mode == StereoMode::ExternalFpHostSoftMoveStick ||
         mode == StereoMode::HeadHideNativeWithFp;
}

bool IsExternalFpHostLookFix(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostLookFix;
}

bool IsExternalFpHostLookAlt(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostLookAlt;
}

bool IsExternalFpHostLookPitch(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostLookPitch;
}

bool IsExternalFpHostLookPitchAlt(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostLookPitchAlt;
}

bool IsExternalFpHostLookMove(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostLookMove;
}

bool IsExternalFpHostLookMoveYaw(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostLookMoveYaw;
}

bool IsExternalFpHostSoftMove(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostSoftMove ||
         mode == StereoMode::HeadHideNativeWithFp;
}

bool IsExternalFpHostSoftMoveStick(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostSoftMoveStick;
}

bool IsHeadHideNativeWithFp(StereoMode mode) {
  return mode == StereoMode::HeadHideNativeWithFp;
}

static bool IsOursFpPost162(StereoMode mode) {
  return mode == StereoMode::OursFpFlashGate || mode == StereoMode::OursFpInCarHead ||
         mode == StereoMode::OursFpEnterCarFp || mode == StereoMode::OursFpHudLayout ||
         mode == StereoMode::OursFpCarHud || mode == StereoMode::OursFpStableCarHud ||
         mode == StereoMode::OursFpSafeFlicker || mode == StereoMode::OursFpFreshDual ||
         mode == StereoMode::OursFpPairFreeze || mode == StereoMode::OursFpMonoPair ||
         mode == StereoMode::OursFpSameLPhone || mode == StereoMode::OursFpBbPhone ||
         mode == StereoMode::OursFpBbPhoneNudge || mode == StereoMode::OursFpStereoSimple ||
         mode == StereoMode::OursFpAer || mode == StereoMode::OursFpSameFrameFreeze ||
         mode == StereoMode::OursFpNoColorFill || mode == StereoMode::OursFpPhoneStereo ||
         mode == StereoMode::OursFpPhoneCanvasInset || mode == StereoMode::OursFpTimeFreeze ||
         mode == StereoMode::OursFpPhaseRightDual || mode == StereoMode::OursFpZoomOut ||
         mode == StereoMode::OursFpTrueWorldScale ||
         (mode == StereoMode::OursFpHeadBoneWorldScale || mode == StereoMode::OursFpSameTickDual ||
          mode == StereoMode::OursFpHeadBoneReturn ||
          mode == StereoMode::OursFpHeadBoneThiscall ||
          IsOursFpSameTickBuildDual(mode));
}

bool IsHeadHideNativeOurs(StereoMode mode) {
  return mode == StereoMode::HeadHideNativeOurs || mode == StereoMode::OursFpFovProfile ||
         mode == StereoMode::OursFpWideUnderPublish || mode == StereoMode::OursFpWideAspectFit ||
         mode == StereoMode::OursFpEyeCenterCam || mode == StereoMode::OursFpEyeCenterLowRes ||
         mode == StereoMode::OursFpMildOverscan || mode == StereoMode::OursFpSquareRes ||
         mode == StereoMode::OursFpStrongOverscan || mode == StereoMode::OursFpSquareLowOverscan ||
         mode == StereoMode::OursFpSquareZeroOverscan || mode == StereoMode::OursFpSquareWideFov ||
         IsOursFpPost162(mode);
}

bool IsOursFpFovProfile(StereoMode mode) {
  return mode == StereoMode::OursFpFovProfile;
}

bool IsOursFpWideUnderPublish(StereoMode mode) {
  return mode == StereoMode::OursFpWideUnderPublish;
}

bool IsOursFpWideAspectFit(StereoMode mode) {
  return mode == StereoMode::OursFpWideAspectFit || mode == StereoMode::OursFpEyeCenterCam ||
         mode == StereoMode::OursFpEyeCenterLowRes || mode == StereoMode::OursFpMildOverscan ||
         mode == StereoMode::OursFpSquareRes || mode == StereoMode::OursFpStrongOverscan ||
         mode == StereoMode::OursFpSquareLowOverscan ||
         mode == StereoMode::OursFpSquareZeroOverscan ||
         mode == StereoMode::OursFpSquareWideFov || IsOursFpPost162(mode);
}

bool IsOursFpEyeCenterCam(StereoMode mode) {
  return mode == StereoMode::OursFpEyeCenterCam || mode == StereoMode::OursFpEyeCenterLowRes ||
         mode == StereoMode::OursFpMildOverscan || mode == StereoMode::OursFpSquareRes ||
         mode == StereoMode::OursFpStrongOverscan || mode == StereoMode::OursFpSquareLowOverscan ||
         mode == StereoMode::OursFpSquareZeroOverscan ||
         mode == StereoMode::OursFpSquareWideFov || IsOursFpPost162(mode);
}

bool IsOursFpEyeCenterLow(StereoMode mode) {
  return mode == StereoMode::OursFpEyeCenterLowRes || mode == StereoMode::OursFpMildOverscan ||
         mode == StereoMode::OursFpSquareRes || mode == StereoMode::OursFpStrongOverscan ||
         mode == StereoMode::OursFpSquareLowOverscan ||
         mode == StereoMode::OursFpSquareZeroOverscan ||
         mode == StereoMode::OursFpSquareWideFov || IsOursFpPost162(mode);
}

bool IsOursFpMildOverscan(StereoMode mode) {
  return mode == StereoMode::OursFpMildOverscan || mode == StereoMode::OursFpSquareRes ||
         mode == StereoMode::OursFpStrongOverscan || mode == StereoMode::OursFpSquareLowOverscan ||
         mode == StereoMode::OursFpSquareZeroOverscan ||
         mode == StereoMode::OursFpSquareWideFov || IsOursFpPost162(mode);
}

bool IsOursFpSquareRes(StereoMode mode) {
  return mode == StereoMode::OursFpSquareRes || mode == StereoMode::OursFpStrongOverscan ||
         mode == StereoMode::OursFpSquareLowOverscan ||
         mode == StereoMode::OursFpSquareZeroOverscan ||
         mode == StereoMode::OursFpSquareWideFov || IsOursFpPost162(mode);
}

bool IsOursFpStrongOverscan(StereoMode mode) {
  return mode == StereoMode::OursFpStrongOverscan;
}

bool IsOursFpSquareLowOverscan(StereoMode mode) {
  return mode == StereoMode::OursFpSquareLowOverscan;
}

bool IsOursFpSquareZeroOverscan(StereoMode mode) {
  return mode == StereoMode::OursFpSquareZeroOverscan ||
         mode == StereoMode::OursFpSquareWideFov || IsOursFpPost162(mode);
}

bool IsOursFpSquareWideFov(StereoMode mode) {
  return mode == StereoMode::OursFpSquareWideFov || IsOursFpPost162(mode);
}

bool IsOursFpFlashGate(StereoMode mode) {
  return mode == StereoMode::OursFpFlashGate || mode == StereoMode::OursFpInCarHead ||
         mode == StereoMode::OursFpEnterCarFp || mode == StereoMode::OursFpHudLayout ||
         mode == StereoMode::OursFpCarHud;
}

bool IsOursFpFlashStable(StereoMode mode) {
  return mode == StereoMode::OursFpStableCarHud;
}

bool IsOursFpSafeFlicker(StereoMode mode) {
  return mode == StereoMode::OursFpSafeFlicker;
}

bool IsOursFpFreshDual(StereoMode mode) {
  return mode == StereoMode::OursFpFreshDual || mode == StereoMode::OursFpPairFreeze ||
         mode == StereoMode::OursFpMonoPair || mode == StereoMode::OursFpSameLPhone ||
         mode == StereoMode::OursFpStereoSimple || mode == StereoMode::OursFpSameFrameFreeze ||
         mode == StereoMode::OursFpNoColorFill || mode == StereoMode::OursFpPhoneStereo ||
         mode == StereoMode::OursFpPhoneCanvasInset || mode == StereoMode::OursFpTimeFreeze ||
         mode == StereoMode::OursFpPhaseRightDual || mode == StereoMode::OursFpZoomOut ||
         mode == StereoMode::OursFpTrueWorldScale ||
         (mode == StereoMode::OursFpHeadBoneWorldScale || mode == StereoMode::OursFpSameTickDual ||
          mode == StereoMode::OursFpHeadBoneReturn ||
          mode == StereoMode::OursFpHeadBoneThiscall ||
          IsOursFpSameTickBuildDual(mode));
}

bool IsOursFpPairFreeze(StereoMode mode) {
  return mode == StereoMode::OursFpPairFreeze;
}

bool IsOursFpSameFrameFreeze(StereoMode mode) {
  return mode == StereoMode::OursFpSameFrameFreeze || mode == StereoMode::OursFpNoColorFill ||
         mode == StereoMode::OursFpPhoneStereo;
}

bool IsOursFpNoColorFill(StereoMode mode) {
  return mode == StereoMode::OursFpNoColorFill || mode == StereoMode::OursFpPhoneStereo;
}

bool IsOursFpDualCamFreeze(StereoMode mode) {
  // Mode193: freeze one HEAD-bone eye for the whole L+R pair (stops stereo image jump).
  // Mode201: freeze full pre-IPD basis for parent dual (look-ghost / fusion).
  return IsOursFpPairFreeze(mode) || IsOursFpSameFrameFreeze(mode) ||
         IsOursFpSameTickDual(mode) || IsOursFpHeadBoneCam(mode) ||
         IsOursFpSameTickParentDualFreeze(mode);
}

bool IsOursFpMonoPair(StereoMode mode) {
  // Mode193: ONE DrawScene only. Dual×2 AVs hard at interior doors (Roman apt);
  // SEH catches it but process still dies after. Keep bone attach + IPD; weaker
  // parallax (same BB → both eyes with eye-canvas crop).
  return mode == StereoMode::OursFpMonoPair || mode == StereoMode::OursFpSameLPhone ||
         IsOursFpHeadBoneCam(mode);
}

bool IsOursFpSameLPhone(StereoMode mode) {
  return mode == StereoMode::OursFpSameLPhone;
}

bool IsOursFpBbPhone(StereoMode mode) {
  return mode == StereoMode::OursFpBbPhone || mode == StereoMode::OursFpBbPhoneNudge;
}

bool IsOursFpBbPhoneNudge(StereoMode mode) {
  return mode == StereoMode::OursFpBbPhoneNudge;
}

bool IsOursFpStereoSimple(StereoMode mode) {
  return mode == StereoMode::OursFpStereoSimple;
}

bool IsOursFpAer(StereoMode mode) {
  return mode == StereoMode::OursFpAer;
}

bool IsOursFpPhoneBounds(StereoMode mode) {
  // BB-mono / AER / failed-180 only — NEVER Mode183 (soft canvas inset instead).
  return mode == StereoMode::OursFpBbPhoneNudge || mode == StereoMode::OursFpStereoSimple ||
         mode == StereoMode::OursFpAer || mode == StereoMode::OursFpPhoneStereo;
}

bool IsOursFpPhoneCanvasInset(StereoMode mode) {
  return mode == StereoMode::OursFpPhoneCanvasInset;
}

bool IsOursFpTimeFreeze(StereoMode mode) {
  // Mode184 poke stays off; Mode189 uses the stronger same-tick path.
  return mode == StereoMode::OursFpTimeFreeze;
}

bool IsOursFpSameTickDual(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickDual;
}

bool IsOursFpSameTickBuildDual(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickBuildDual ||
         mode == StereoMode::OursFpSameTickPhaseRight ||
         mode == StereoMode::OursFpSameTickDrawRight ||
         mode == StereoMode::OursFpSameTickOwnerObserve ||
         mode == StereoMode::OursFpSameTickOwnerCount ||
         mode == StereoMode::OursFpSameTickParentProbe ||
         mode == StereoMode::OursFpSameTickParentCount ||
         mode == StereoMode::OursFpSameTickParentDual ||
         mode == StereoMode::OursFpSameTickParentDualFreeze ||
         mode == StereoMode::OursFpSameTickParentDualVs ||
         mode == StereoMode::OursFpSameTickParentDualPose ||
         mode == StereoMode::OursFpSameTickParentDualTry2 ||
         mode == StereoMode::OursFpSameTickParentDualTry3 ||
         mode == StereoMode::OursFpSameTickParentDualTry4 ||
         mode == StereoMode::OursFpSameTickParentDualTry5 ||
         mode == StereoMode::OursFpSameTickParentDualTry6 ||
         mode == StereoMode::OursFpSameTickParentDual203Outer ||
         mode == StereoMode::OursFpSameTickParentDual203ToeIn ||
         mode == StereoMode::OursFpSameTickParentDual203ToeInStrong ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpd ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdHalf ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdQtr ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdToeIn ||
         mode == StereoMode::OursFpSameTickParentDual204DeferredHead ||
         mode == StereoMode::OursFpSameTickParentDual203CamRightFlip ||
         mode == StereoMode::OursFpSameTickParentDual204CamRightFlip ||
         mode == StereoMode::OursFpSameTickParentDual203CamRightFlipLateLatch ||
         IsTrueStereoFamily(mode);  // 230 on 203 seam inherits Post162 / ped-hide / accept
}

bool IsOursFpSameTickPhaseRight(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickPhaseRight;
}

bool IsOursFpSameTickDrawRight(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickDrawRight;
}

bool IsOursFpSameTickOwnerObserve(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickOwnerObserve;
}

bool IsOursFpSameTickOwnerCount(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickOwnerCount;
}

bool IsOursFpSameTickParentProbe(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentProbe;
}

bool IsOursFpSameTickParentCount(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentCount;
}

bool IsOursFpSameTickParentDual(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual ||
         mode == StereoMode::OursFpSameTickParentDualFreeze ||
         mode == StereoMode::OursFpSameTickParentDualVs ||
         mode == StereoMode::OursFpSameTickParentDualPose ||
         mode == StereoMode::OursFpSameTickParentDualTry2 ||
         mode == StereoMode::OursFpSameTickParentDualTry3 ||
         mode == StereoMode::OursFpSameTickParentDualTry4 ||
         mode == StereoMode::OursFpSameTickParentDualTry5 ||
         mode == StereoMode::OursFpSameTickParentDualTry6 ||
         mode == StereoMode::OursFpSameTickParentDual203Outer ||
         mode == StereoMode::OursFpSameTickParentDual203ToeIn ||
         mode == StereoMode::OursFpSameTickParentDual203ToeInStrong ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpd ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdHalf ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdQtr ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdToeIn ||
         mode == StereoMode::OursFpSameTickParentDual204DeferredHead ||
         mode == StereoMode::OursFpSameTickParentDual203CamRightFlip ||
         mode == StereoMode::OursFpSameTickParentDual204CamRightFlip ||
         mode == StereoMode::OursFpSameTickParentDual203CamRightFlipLateLatch ||
         IsTrueStereoFamily(mode);
}

bool IsOursFpSameTickParentDualFreeze(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDualFreeze;
}

bool IsOursFpSameTickParentDualVs(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDualVs;
}

bool IsOursFpSameTickParentDualPose(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDualPose ||
         mode == StereoMode::OursFpSameTickParentDualTry2 ||
         mode == StereoMode::OursFpSameTickParentDualTry3 ||
         mode == StereoMode::OursFpSameTickParentDualTry4 ||
         mode == StereoMode::OursFpSameTickParentDualTry5 ||
         mode == StereoMode::OursFpSameTickParentDualTry6 ||
         mode == StereoMode::OursFpSameTickParentDual203Outer ||
         mode == StereoMode::OursFpSameTickParentDual203ToeIn ||
         mode == StereoMode::OursFpSameTickParentDual203ToeInStrong ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpd ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdHalf ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdQtr ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdToeIn ||
         mode == StereoMode::OursFpSameTickParentDual204DeferredHead ||
         mode == StereoMode::OursFpSameTickParentDual203CamRightFlip ||
         mode == StereoMode::OursFpSameTickParentDual204CamRightFlip ||
         mode == StereoMode::OursFpSameTickParentDual203CamRightFlipLateLatch ||
         IsTrueStereoFamily(mode);  // 203 seam + pose latch (NOT Try2/204)
}

bool IsOursFpSameTickParentDualTry2(StereoMode mode) {
  // Do NOT include TrueStereo — that would force the 204 @0x4DE020 seam.
  // Mode 242 IS the 204 seam + cam-right flip.
  return mode == StereoMode::OursFpSameTickParentDualTry2 ||
         mode == StereoMode::OursFpSameTickParentDual204DeferredHead ||
         mode == StereoMode::OursFpSameTickParentDual204CamRightFlip;
}

bool IsOursFpSameTickParentDualTry3(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDualTry3;
}

bool IsOursFpSameTickParentDualTry4(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDualTry4;
}

bool IsOursFpSameTickParentDualTry5(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDualTry5;
}

bool IsOursFpSameTickParentDualTry6(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDualTry6;
}

bool IsOursFpSameTickParentDual203Outer(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual203Outer;
}

bool IsOursFpSameTickParentDual203ToeIn(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual203ToeIn ||
         mode == StereoMode::OursFpSameTickParentDual203ToeInStrong ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdToeIn;
}

bool IsOursFpSameTickParentDual203ToeInStrong(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual203ToeInStrong;
}

bool IsOursFpSameTickParentDual203CloseIpd(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual203CloseIpd ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdHalf ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdQtr ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdToeIn;
}

bool IsOursFpSameTickParentDual203CloseIpdHalf(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual203CloseIpdHalf ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdQtr ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdToeIn;
}

bool IsOursFpSameTickParentDual203CloseIpdQtr(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual203CloseIpdQtr ||
         mode == StereoMode::OursFpSameTickParentDual203CloseIpdToeIn;
}

bool IsOursFpSameTickParentDual203CloseIpdToeIn(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual203CloseIpdToeIn;
}

bool IsOursFpDeferredHeadBone(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual204DeferredHead;
}

bool IsTrueStereoFamily(StereoMode mode) {
  return mode == StereoMode::TrueStereoCalibProbe || mode == StereoMode::TrueStereoExact ||
         mode == StereoMode::TrueStereoCover || mode == StereoMode::TrueStereoDirect ||
         mode == StereoMode::TrueStereoSameState || mode == StereoMode::TrueStereoStablePublish ||
         mode == StereoMode::TrueStereoLeftPublish || mode == StereoMode::TrueStereoLateralIpd ||
         mode == StereoMode::TrueStereoUevrIpd || mode == StereoMode::TrueStereoCamRightIpd ||
         mode == StereoMode::TrueStereoCamRightIpdFlip;
}

bool IsStereoCalibProbe(StereoMode mode) {
  return mode == StereoMode::TrueStereoCalibProbe;
}

bool IsTrueStereoExact(StereoMode mode) {
  return mode == StereoMode::TrueStereoExact || mode == StereoMode::TrueStereoCover ||
         mode == StereoMode::TrueStereoDirect || mode == StereoMode::TrueStereoSameState ||
         mode == StereoMode::TrueStereoStablePublish || mode == StereoMode::TrueStereoLeftPublish ||
         mode == StereoMode::TrueStereoLateralIpd || mode == StereoMode::TrueStereoUevrIpd ||
         mode == StereoMode::TrueStereoCamRightIpd || mode == StereoMode::TrueStereoCamRightIpdFlip;
}

bool IsTrueStereoCover(StereoMode mode) {
  return mode == StereoMode::TrueStereoCover || mode == StereoMode::TrueStereoDirect ||
         mode == StereoMode::TrueStereoSameState || mode == StereoMode::TrueStereoStablePublish ||
         mode == StereoMode::TrueStereoLeftPublish || mode == StereoMode::TrueStereoLateralIpd ||
         mode == StereoMode::TrueStereoUevrIpd || mode == StereoMode::TrueStereoCamRightIpd ||
         mode == StereoMode::TrueStereoCamRightIpdFlip;
}

bool IsTrueStereoDirect(StereoMode mode) {
  return mode == StereoMode::TrueStereoDirect || mode == StereoMode::TrueStereoSameState ||
         mode == StereoMode::TrueStereoStablePublish || mode == StereoMode::TrueStereoLeftPublish ||
         mode == StereoMode::TrueStereoLateralIpd || mode == StereoMode::TrueStereoUevrIpd ||
         mode == StereoMode::TrueStereoCamRightIpd || mode == StereoMode::TrueStereoCamRightIpdFlip;
}

bool IsTrueStereoSameState(StereoMode mode) {
  return mode == StereoMode::TrueStereoSameState;
}

bool IsTrueStereoStablePublish(StereoMode mode) {
  return mode == StereoMode::TrueStereoStablePublish || mode == StereoMode::TrueStereoLeftPublish ||
         mode == StereoMode::TrueStereoLateralIpd || mode == StereoMode::TrueStereoUevrIpd ||
         mode == StereoMode::TrueStereoCamRightIpd || mode == StereoMode::TrueStereoCamRightIpdFlip;
}

bool IsTrueStereoLeftPublish(StereoMode mode) {
  return mode == StereoMode::TrueStereoLeftPublish || mode == StereoMode::TrueStereoLateralIpd ||
         mode == StereoMode::TrueStereoUevrIpd || mode == StereoMode::TrueStereoCamRightIpd ||
         mode == StereoMode::TrueStereoCamRightIpdFlip;
}

bool IsTrueStereoLateralIpd(StereoMode mode) {
  return mode == StereoMode::TrueStereoLateralIpd;
}

bool IsTrueStereoUevrIpd(StereoMode mode) {
  return mode == StereoMode::TrueStereoUevrIpd;
}

bool IsTrueStereoCamRightIpd(StereoMode mode) {
  return mode == StereoMode::TrueStereoCamRightIpd || mode == StereoMode::TrueStereoCamRightIpdFlip;
}

bool IsTrueStereoCamRightIpdFlip(StereoMode mode) {
  return mode == StereoMode::TrueStereoCamRightIpdFlip;
}

bool IsOursFp203CamRightFlip(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual203CamRightFlip ||
         mode == StereoMode::OursFpSameTickParentDual203CamRightFlipLateLatch;
}

bool IsOursFp204CamRightFlip(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual204CamRightFlip;
}

bool IsOursFp203LateLatch(StereoMode mode) {
  return mode == StereoMode::OursFpSameTickParentDual203CamRightFlipLateLatch;
}

bool UsesCamRightIpd(StereoMode mode) {
  return IsTrueStereoCamRightIpd(mode) || IsOursFp203CamRightFlip(mode) ||
         IsOursFp204CamRightFlip(mode);
}

bool UsesCamRightIpdFlip(StereoMode mode) {
  return IsTrueStereoCamRightIpdFlip(mode) || IsOursFp203CamRightFlip(mode) ||
         IsOursFp204CamRightFlip(mode);
}

bool UsesLateLatchSubmit(StereoMode mode) {
  return IsCleanDualLateLatch(mode) || IsOursFp203LateLatch(mode);
}

bool IsOursFpHeadBoneCam(StereoMode mode) {
  return mode == StereoMode::OursFpHeadBoneThiscall;
}

bool IsOursFpTrueWorldScale(StereoMode mode) {
  // Mode 231+: worldscale stays 6DoF-lean only — must NOT multiply the stereo baseline.
  if (IsTrueStereoExact(mode))
    return false;
  return mode == StereoMode::OursFpTrueWorldScale || IsOursFpHeadBoneCam(mode) ||
         IsOursFpSameTickDual(mode) || IsOursFpSameTickBuildDual(mode);
}

bool IsOursFpZoomOut(StereoMode mode) {
  return mode == StereoMode::OursFpZoomOut || IsOursFpTrueWorldScale(mode);
}

bool IsOursFpPhaseRightDual(StereoMode mode) {
  return mode == StereoMode::OursFpPhaseRightDual || IsOursFpZoomOut(mode);
}

bool IsOursFpUiLift(StereoMode mode) {
  return mode == StereoMode::OursFpMonoPair || mode == StereoMode::OursFpSameLPhone;
}

bool IsOursFpAtomicEyePair(StereoMode mode) {
  return mode == StereoMode::OursFpStableCarHud || mode == StereoMode::OursFpSafeFlicker ||
         mode == StereoMode::OursFpFreshDual || mode == StereoMode::OursFpPairFreeze ||
         mode == StereoMode::OursFpMonoPair || mode == StereoMode::OursFpSameLPhone ||
         mode == StereoMode::OursFpStereoSimple || mode == StereoMode::OursFpSameFrameFreeze ||
         mode == StereoMode::OursFpNoColorFill || mode == StereoMode::OursFpPhoneStereo ||
         mode == StereoMode::OursFpPhoneCanvasInset || mode == StereoMode::OursFpTimeFreeze ||
         mode == StereoMode::OursFpPhaseRightDual || mode == StereoMode::OursFpZoomOut ||
         mode == StereoMode::OursFpTrueWorldScale ||
         (mode == StereoMode::OursFpHeadBoneWorldScale || mode == StereoMode::OursFpSameTickDual ||
          mode == StereoMode::OursFpHeadBoneReturn ||
          mode == StereoMode::OursFpHeadBoneThiscall ||
          IsOursFpSameTickBuildDual(mode));
}

bool IsOursFpAlwaysBbCapture(StereoMode mode) {
  return mode == StereoMode::OursFpStableCarHud || mode == StereoMode::OursFpSafeFlicker ||
         mode == StereoMode::OursFpFreshDual || mode == StereoMode::OursFpPairFreeze ||
         mode == StereoMode::OursFpMonoPair || mode == StereoMode::OursFpSameLPhone ||
         mode == StereoMode::OursFpStereoSimple || mode == StereoMode::OursFpSameFrameFreeze ||
         mode == StereoMode::OursFpNoColorFill || mode == StereoMode::OursFpPhoneStereo ||
         mode == StereoMode::OursFpPhoneCanvasInset || mode == StereoMode::OursFpTimeFreeze ||
         mode == StereoMode::OursFpPhaseRightDual || mode == StereoMode::OursFpZoomOut ||
         mode == StereoMode::OursFpTrueWorldScale ||
         (mode == StereoMode::OursFpHeadBoneWorldScale || mode == StereoMode::OursFpSameTickDual ||
          mode == StereoMode::OursFpHeadBoneReturn ||
          mode == StereoMode::OursFpHeadBoneThiscall ||
          IsOursFpSameTickBuildDual(mode));
}

bool IsOursFpInCarHead(StereoMode mode) {
  return mode == StereoMode::OursFpInCarHead || mode == StereoMode::OursFpEnterCarFp ||
         mode == StereoMode::OursFpCarHud || mode == StereoMode::OursFpStableCarHud ||
         mode == StereoMode::OursFpSafeFlicker || mode == StereoMode::OursFpFreshDual ||
         mode == StereoMode::OursFpPairFreeze || mode == StereoMode::OursFpMonoPair ||
         mode == StereoMode::OursFpSameLPhone || mode == StereoMode::OursFpBbPhone ||
         mode == StereoMode::OursFpBbPhoneNudge || mode == StereoMode::OursFpStereoSimple ||
         mode == StereoMode::OursFpAer || mode == StereoMode::OursFpSameFrameFreeze ||
         mode == StereoMode::OursFpNoColorFill || mode == StereoMode::OursFpPhoneStereo ||
         mode == StereoMode::OursFpPhoneCanvasInset || mode == StereoMode::OursFpTimeFreeze ||
         mode == StereoMode::OursFpPhaseRightDual || mode == StereoMode::OursFpZoomOut ||
         mode == StereoMode::OursFpTrueWorldScale ||
         (mode == StereoMode::OursFpHeadBoneWorldScale || mode == StereoMode::OursFpSameTickDual ||
          mode == StereoMode::OursFpHeadBoneReturn ||
          mode == StereoMode::OursFpHeadBoneThiscall ||
          IsOursFpSameTickBuildDual(mode));
}

bool IsOursFpEnterCarFp(StereoMode mode) {
  return mode == StereoMode::OursFpEnterCarFp || mode == StereoMode::OursFpCarHud ||
         mode == StereoMode::OursFpStableCarHud || mode == StereoMode::OursFpSafeFlicker ||
         mode == StereoMode::OursFpFreshDual || mode == StereoMode::OursFpPairFreeze ||
         mode == StereoMode::OursFpMonoPair || mode == StereoMode::OursFpSameLPhone ||
         mode == StereoMode::OursFpBbPhone || mode == StereoMode::OursFpBbPhoneNudge ||
         mode == StereoMode::OursFpStereoSimple || mode == StereoMode::OursFpAer ||
         mode == StereoMode::OursFpSameFrameFreeze || mode == StereoMode::OursFpNoColorFill ||
         mode == StereoMode::OursFpPhoneStereo || mode == StereoMode::OursFpPhoneCanvasInset ||
         mode == StereoMode::OursFpTimeFreeze || mode == StereoMode::OursFpPhaseRightDual ||
         mode == StereoMode::OursFpZoomOut || mode == StereoMode::OursFpTrueWorldScale ||
         (mode == StereoMode::OursFpHeadBoneWorldScale || mode == StereoMode::OursFpSameTickDual ||
          mode == StereoMode::OursFpHeadBoneReturn ||
          mode == StereoMode::OursFpHeadBoneThiscall ||
          IsOursFpSameTickBuildDual(mode));
}

bool IsOursFpHudLayout(StereoMode mode) {
  return mode == StereoMode::OursFpHudLayout || mode == StereoMode::OursFpCarHud ||
         mode == StereoMode::OursFpStableCarHud || mode == StereoMode::OursFpSafeFlicker ||
         mode == StereoMode::OursFpFreshDual || mode == StereoMode::OursFpPairFreeze ||
         mode == StereoMode::OursFpMonoPair || mode == StereoMode::OursFpSameLPhone ||
         mode == StereoMode::OursFpBbPhone || mode == StereoMode::OursFpBbPhoneNudge ||
         mode == StereoMode::OursFpStereoSimple || mode == StereoMode::OursFpAer ||
         mode == StereoMode::OursFpSameFrameFreeze || mode == StereoMode::OursFpNoColorFill ||
         mode == StereoMode::OursFpPhoneStereo || mode == StereoMode::OursFpPhoneCanvasInset ||
         mode == StereoMode::OursFpTimeFreeze || mode == StereoMode::OursFpPhaseRightDual ||
         mode == StereoMode::OursFpZoomOut || mode == StereoMode::OursFpTrueWorldScale ||
         (mode == StereoMode::OursFpHeadBoneWorldScale || mode == StereoMode::OursFpSameTickDual ||
          mode == StereoMode::OursFpHeadBoneReturn ||
          mode == StereoMode::OursFpHeadBoneThiscall ||
          IsOursFpSameTickBuildDual(mode));
}

bool IsOursFpCarHud(StereoMode mode) {
  return mode == StereoMode::OursFpCarHud || mode == StereoMode::OursFpStableCarHud ||
         mode == StereoMode::OursFpSafeFlicker || mode == StereoMode::OursFpFreshDual ||
         mode == StereoMode::OursFpPairFreeze || mode == StereoMode::OursFpMonoPair ||
         mode == StereoMode::OursFpSameLPhone || mode == StereoMode::OursFpBbPhone ||
         mode == StereoMode::OursFpBbPhoneNudge || mode == StereoMode::OursFpStereoSimple ||
         mode == StereoMode::OursFpAer || mode == StereoMode::OursFpSameFrameFreeze ||
         mode == StereoMode::OursFpNoColorFill || mode == StereoMode::OursFpPhoneStereo ||
         mode == StereoMode::OursFpPhoneCanvasInset || mode == StereoMode::OursFpTimeFreeze ||
         mode == StereoMode::OursFpPhaseRightDual || mode == StereoMode::OursFpZoomOut ||
         mode == StereoMode::OursFpTrueWorldScale ||
         (mode == StereoMode::OursFpHeadBoneWorldScale || mode == StereoMode::OursFpSameTickDual ||
          mode == StereoMode::OursFpHeadBoneReturn ||
          mode == StereoMode::OursFpHeadBoneThiscall ||
          IsOursFpSameTickBuildDual(mode));
}

bool IsOursFpStableCarHud(StereoMode mode) {
  return mode == StereoMode::OursFpStableCarHud;
}

float GetUnderPublishOverscan() {
  const StereoMode sm = GetStereoMode();
  if (!IsOursFpMildOverscan(sm))
    return 1.f;
  // Mode 161–166: exact fit (0% overscan).
  if (IsOursFpSquareZeroOverscan(sm))
    return 1.0f;
  // Mode 160: square-aspect — barely any overscan.
  if (IsOursFpSquareLowOverscan(sm))
    return 1.02f;
  // Mode 159: stronger bar shrink on widescreen / pre-square.
  if (IsOursFpStrongOverscan(sm))
    return 1.10f;
  // Mode 158 with near-square BB: soft overscan.
  if (sm == StereoMode::OursFpSquareRes) {
    const float a = GetBackbufferAspect();
    if (a > 0.85f && a < 1.25f)
      return 1.03f;
  }
  return 1.06f;  // Mode 157 / 158 widescreen
}

bool WantsExternalFpLookMove(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostLookMove ||
         mode == StereoMode::ExternalFpHostLookMoveYaw ||
         mode == StereoMode::ExternalFpHostSoftMove ||
         mode == StereoMode::ExternalFpHostSoftMoveStick ||
         mode == StereoMode::HeadHideNativeWithFp ||
         mode == StereoMode::HeadHideNativeOurs ||
         mode == StereoMode::OursFpFovProfile ||
         mode == StereoMode::OursFpWideUnderPublish ||
         mode == StereoMode::OursFpWideAspectFit ||
         mode == StereoMode::OursFpEyeCenterCam ||
         mode == StereoMode::OursFpEyeCenterLowRes ||
         mode == StereoMode::OursFpMildOverscan ||
         mode == StereoMode::OursFpSquareRes ||
         mode == StereoMode::OursFpStrongOverscan ||
         mode == StereoMode::OursFpSquareLowOverscan ||
         mode == StereoMode::OursFpSquareZeroOverscan ||
         mode == StereoMode::OursFpSquareWideFov ||
         IsOursFpPost162(mode);
}

bool WantsSoftPedHeading(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostSoftMove ||
         mode == StereoMode::ExternalFpHostSoftMoveStick ||
         mode == StereoMode::HeadHideNativeWithFp;
}

bool WantsFpStickInvert(StereoMode mode) {
  return mode == StereoMode::ExternalFpHostSoftMove ||
         mode == StereoMode::HeadHideNativeWithFp;
}

bool WantsNativePedHide(StereoMode mode) {
  return mode == StereoMode::HeadHideNativeWithFp ||
         mode == StereoMode::HeadHideNativeOurs ||
         mode == StereoMode::OursFpFovProfile ||
         mode == StereoMode::OursFpWideUnderPublish ||
         mode == StereoMode::OursFpWideAspectFit ||
         mode == StereoMode::OursFpEyeCenterCam ||
         mode == StereoMode::OursFpEyeCenterLowRes ||
         mode == StereoMode::OursFpMildOverscan ||
         mode == StereoMode::OursFpSquareRes ||
         mode == StereoMode::OursFpStrongOverscan ||
         mode == StereoMode::OursFpSquareLowOverscan ||
         mode == StereoMode::OursFpSquareZeroOverscan ||
         mode == StereoMode::OursFpSquareWideFov ||
         IsOursFpPost162(mode);
}

bool WantsFpAbsoluteFov(StereoMode mode) {
  return mode == StereoMode::OursFpWideUnderPublish ||
         mode == StereoMode::OursFpWideAspectFit ||
         mode == StereoMode::OursFpEyeCenterCam ||
         mode == StereoMode::OursFpEyeCenterLowRes ||
         mode == StereoMode::OursFpMildOverscan ||
         mode == StereoMode::OursFpSquareRes ||
         mode == StereoMode::OursFpStrongOverscan ||
         mode == StereoMode::OursFpSquareLowOverscan ||
         mode == StereoMode::OursFpSquareZeroOverscan ||
         mode == StereoMode::OursFpSquareWideFov ||
         IsOursFpPost162(mode);
}

bool UsesDrawSceneDualPath(StereoMode mode) {
  return IsCleanDualLookMove(mode) || IsExternalFpHost(mode) || IsHeadHideNativeOurs(mode);
}

namespace {

void WriteSmallConfigIfMissing(const char* name, const char* contents) {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, name);
  if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
    return;
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  std::fputs(contents, f);
  std::fclose(f);
  Log("Config: wrote default %s", name);
}

void WriteSmallConfigAlways(const char* name, const char* contents) {
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, name);
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  std::fputs(contents, f);
  std::fclose(f);
  Log("Config: wrote %s", name);
}

struct FpFovCache {
  bool loaded = false;
  float forward = 90.f;
  float rear = 90.f;
  float foot = 90.f;
  float mouseScale = 1.f;
  float joyScale = 1.f;
};

FpFovCache& FpCache() {
  static FpFovCache c;
  return c;
}

void LoadFpProfileOnce() {
  FpFovCache& c = FpCache();
  if (c.loaded)
    return;
  c.loaded = true;
  char buf[64]{};
  int a = 90, b = 90, d = 90;
  if (ReadSmallFile("gtaiv_dxvk_vr.fpfov", buf, sizeof(buf)) > 0 &&
      sscanf_s(buf, "%d %d %d", &a, &b, &d) >= 1) {
    if (a >= 40 && a <= 120)
      c.forward = static_cast<float>(a);
    if (b >= 40 && b <= 120)
      c.rear = static_cast<float>(b);
    if (d >= 40 && d <= 120)
      c.foot = static_cast<float>(d);
    Log("Config: fpfov forward=%.0f rear=%.0f foot=%.0f (FirstPerson.ini style; not 111)",
        c.forward, c.rear, c.foot);
  } else {
    Log("Config: fpfov default 90 90 90 (write gtaiv_dxvk_vr.fpfov to change)");
  }
  int ms = 50;
  if (ReadSmallFile("gtaiv_dxvk_vr.mousesens", buf, sizeof(buf)) > 0 &&
      sscanf_s(buf, "%d", &ms) == 1 && ms >= 1 && ms <= 200) {
    c.mouseScale = static_cast<float>(ms) / 50.f;
    Log("Config: mousesens=%d (scale=%.2f vs FirstPerson MouseSens 50)", ms, c.mouseScale);
  }
  int j1 = 20;
  int j2 = 30;
  if (ReadSmallFile("gtaiv_dxvk_vr.joysens", buf, sizeof(buf)) > 0 &&
      sscanf_s(buf, "%d %d", &j1, &j2) >= 1 && j1 >= 1 && j1 <= 100) {
    c.joyScale = static_cast<float>(j1) / 20.f;
    Log("Config: joysens lev1=%d lev2=%d (scale=%.2f vs JoySensLev1 20)", j1, j2, c.joyScale);
  }
}

}  // namespace

void EnsureFpProfileDefaults() {
  // Match user's working FirstPerson.ini FOV 90 (not inspo 111).
  WriteSmallConfigIfMissing("gtaiv_dxvk_vr.fpfov", "90 90 90\n");
  WriteSmallConfigIfMissing("gtaiv_dxvk_vr.mousesens", "50\n");
  WriteSmallConfigIfMissing("gtaiv_dxvk_vr.joysens", "20 30\n");
  WriteSmallConfigIfMissing("gtaiv_dxvk_vr.camoff", "0 0 0\n");
  FpCache().loaded = false;
  LoadFpProfileOnce();
}

void ForceFpFovDegrees(int forward, int rear, int foot) {
  if (forward < 40)
    forward = 40;
  if (forward > 120)
    forward = 120;
  if (rear < 40)
    rear = 40;
  if (rear > 120)
    rear = 120;
  if (foot < 40)
    foot = 40;
  if (foot > 120)
    foot = 120;
  char line[32]{};
  sprintf_s(line, "%d %d %d\n", forward, rear, foot);
  WriteSmallConfigAlways("gtaiv_dxvk_vr.fpfov", line);
  FpCache().loaded = false;
  LoadFpProfileOnce();
  Log("Config: forced fpfov %d %d %d (Mode162 wide zoom-out)", forward, rear, foot);
}

namespace {
struct VehCamOffCache {
  bool loaded = false;
  float right = 0.f;
  float forward = -0.12f;  // −12 cm (trucks looked odd at −20)
  float up = 0.f;
};
VehCamOffCache& VehCamCache() {
  static VehCamOffCache c;
  return c;
}
void LoadVehCamOffOnce() {
  VehCamOffCache& c = VehCamCache();
  if (c.loaded)
    return;
  c.loaded = true;
  char buf[64]{};
  int x = 0, y = -12, z = 0;
  if (ReadSmallFile("gtaiv_dxvk_vr.vehcamoff", buf, sizeof(buf)) > 0 &&
      sscanf_s(buf, "%d %d %d", &x, &y, &z) >= 1) {
    if (x >= -50 && x <= 50)
      c.right = static_cast<float>(x) / 100.f;
    if (y >= -50 && y <= 50)
      c.forward = static_cast<float>(y) / 100.f;
    if (z >= -50 && z <= 50)
      c.up = static_cast<float>(z) / 100.f;
    Log("Config: vehcamoff right=%d fwd=%d up=%d cm (Mode164/165 in-car)", x, y, z);
  } else {
    Log("Config: vehcamoff default 0 -12 0 cm");
  }
}
}  // namespace

void EnsureVehicleCamOffDefaults() {
  // Force milder rearward default (trucks odd at −20; ahead a bit vs prior).
  WriteSmallConfigAlways("gtaiv_dxvk_vr.vehcamoff", "0 -12 0\n");
  VehCamCache().loaded = false;
  LoadVehCamOffOnce();
}

void EnsureModelCenteredCamOffsets() {
  // Mode216: no foot/car cam nudges — pivot stays on HEAD bone.
  WriteSmallConfigAlways("gtaiv_dxvk_vr.vehcamoff", "0 0 0\n");
  WriteSmallConfigAlways("gtaiv_dxvk_vr.camoff", "0 0 0\n");
  VehCamCache().loaded = false;
  LoadVehCamOffOnce();
  SetEyeForwardCm(0);
  Log("Config: Mode216 model-centered — camoff/vehcamoff/eyefwd forced 0");
}

void GetVehicleCamOffsetMeters(float* outRight, float* outForward, float* outUp) {
  LoadVehCamOffOnce();
  if (outRight)
    *outRight = VehCamCache().right;
  if (outForward)
    *outForward = VehCamCache().forward;
  if (outUp)
    *outUp = VehCamCache().up;
}

void EnsureVrSquareCommandlineReady() {
  // square-aspect square render — with our FOV under-publish (not alone).
  // User renames to commandline.txt and fully restarts GTA (Steam/CE reads at boot).
  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return;
  strcat_s(path, "commandline.txt.vr-square");
  if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
    Log("Mode158: commandline.txt.vr-square already present — rename to commandline.txt "
        "and FULL restart for 1440x1440 (backup old commandline.txt first)");
    return;
  }
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return;
  std::fputs("-windowed\n"
             "-width 1440\n"
             "-height 1440\n"
             "-norestrictions\n",
             f);
  std::fclose(f);
  Log("Mode158: wrote commandline.txt.vr-square (1440x1440). "
      "Backup commandline.txt if any, rename .vr-square -> commandline.txt, FULL restart. "
      "Revert: delete commandline.txt or restore backup (kill=157)");
}

float GetFpForwardFovDegrees() {
  LoadFpProfileOnce();
  return FpCache().forward;
}

float GetFpRearFovDegrees() {
  LoadFpProfileOnce();
  return FpCache().rear;
}

float GetFpFootFovDegrees() {
  LoadFpProfileOnce();
  return FpCache().foot;
}

float GetFpMouseSensScale() {
  LoadFpProfileOnce();
  return FpCache().mouseScale;
}

float GetFpJoySensScale() {
  LoadFpProfileOnce();
  return FpCache().joyScale;
}

// Mode 123: once HMD cover FOV is known, pick fovadd so published gameTanH ≈ coverH.
// Inverse of PublishGameFovFromCCamDegrees (kEng=58.7/45). Assumes live CCam base ≈45°
// (FusionFix FOV=0); clamps 0..18 so we never re-enter Crop/Mild zoom-IN.
void TryApplyCoverMatchedFovAdd() {
  if (!IsCleanDualFillMatch(GetStereoMode()))
    return;
  static std::atomic<bool> s_done{false};
  if (s_done.load())
    return;
  float coverH = 0.f, coverV = 0.f;
  if (!GetCoverFovTangents(&coverH, &coverV) || !(coverH > 0.05f))
    return;
  const float aspect = GetBackbufferAspect();
  const float a = (aspect > 0.5f && aspect < 3.f) ? aspect : (16.f / 9.f);
  // Want tanH_game ≈ coverH → tanV = coverH/aspect → vDeg → ccamDeg.
  constexpr float kEng = 58.7f / 45.f;
  const float tanV = coverH / a;
  if (!(tanV > 0.05f) || !(tanV < 5.f))
    return;
  const float vDeg = 2.f * std::atan(tanV) * (180.f / 3.14159265f);
  const float targetCcam = vDeg / kEng;
  constexpr float kBaseCcam = 45.f;  // FusionFix FieldOfView=0 typical CE baseline
  float add = targetCcam - kBaseCcam;
  if (add < 0.f)
    add = 0.f;
  if (add > 18.f)
    add = 18.f;  // never Mild18+ crop zone for Vollbild path
  SetFovAddDegrees(add);
  SaveFovAddFile(static_cast<int>(add + 0.5f));
  // Mode 126–135: stereoscale 135 (smaller world / dezoom feel). Others stay at 130 cap.
  const StereoMode cur = GetStereoMode();
  const int stereoPct =
      (IsCleanDualFpsLiveLook(cur) || IsCleanDualFpsHoldStereo(cur) || IsCleanDualLrSync(cur) ||
       IsCleanDualMonoLook(cur) || IsCleanDualMonoLookPitch(cur) || IsCleanDualMonoLookHmdEvery(cur) ||
       IsCleanDualMonoLookHmdCheap(cur) || IsCleanDualMonoLookBack132(cur) ||
       IsCleanDualAlwaysFresh(cur))
          ? 135
          : 130;
  SetStereoScalePercent(stereoPct);
  SaveStereoScaleFile(stereoPct);
  s_done.store(true);
  const float fillH = 100.f;  // by construction (H-match)
  const float gameTanV = tanV;
  const float fillV = (coverV > 0.05f) ? (gameTanV / coverV) * 100.f : 0.f;
  Log("Mode%d: COVER-MATCH fovadd=%.0f stereoscale=%d coverTan=(%.3f,%.3f) "
      "target fill≈%.0f%%h/%.0f%%v (V bars expected on 16:9→G2; no SoftInset)",
      static_cast<int>(GetStereoMode()), add, stereoPct, coverH, coverV, fillH, fillV);
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

float GetStereoScale() {
  // Mode 231+: ignore stereoscale file — always 1.00 (real IPD disparity).
  if (IsTrueStereoExact(GetStereoMode()))
    return 1.f;
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

namespace {
std::atomic<int> g_eyeFwdCm{-1};
}  // namespace

float GetEyeForwardMeters() {
  int cm = g_eyeFwdCm.load();
  if (cm < 0) {
    // 42cm: past skull/hair. Mode 152 forces 0 (FirstPerson-style body visible).
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
    g_eyeFwdCm.store(cm);
  }
  return static_cast<float>(cm) / 100.f;
}

void SetEyeForwardCm(int cm) {
  if (cm < 0)
    cm = 0;
  if (cm > 100)
    cm = 100;
  char path[MAX_PATH]{};
  if (GetAsiDir(path, MAX_PATH)) {
    strcat_s(path, "gtaiv_dxvk_vr.eyefwd");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
      std::fprintf(f, "%d\n", cm);
      std::fclose(f);
    }
  }
  g_eyeFwdCm.store(cm);
  Log("Config: eyeForward set live=%d cm (0=FirstPerson-style; look down sees jacket)", cm);
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
  if (!g_fovAddLoaded.load()) {
    char buf[16]{};
    int deg = 0;
    float add = 0.f;
    if (ReadSmallFile("gtaiv_dxvk_vr.fovadd", buf, sizeof(buf)) > 0 &&
        sscanf_s(buf, "%d", &deg) == 1 && deg >= 0 && deg <= 40) {
      add = static_cast<float>(deg);
      Log("Config: fovadd=%.0f deg (gtaiv_dxvk_vr.fovadd) — ADD at CCam+0x60 after recompute; "
          "F7 cycles visual WorldScale; kill: delete file or set 0",
          add);
    } else {
      Log("Config: fovadd=0 (absent) — Mode 35 FOV site READ-ONLY until F7/worldscale");
    }
    g_fovAdd.store(add);
    g_fovAddLoaded.store(true);
  }
  return g_fovAdd.load();
}

void PollWorldScaleHotkey() {
  static bool wasF7 = false;
  if (!KeyPressedEdge(VK_F7, &wasF7))
    return;

  // Visual world size (fovadd + stereoscale). Live — no RT recreate, no SoftInset.
  int idx = g_worldScalePreset.load();
  if (idx < 0 || idx >= kWorldScalePresetN)
    idx = kWorldScaleDefaultIdx;
  idx = (idx + 1) % kWorldScalePresetN;
  ApplyWorldScalePreset(idx, true);
}

void PollTrueWorldScaleHotkey() {
  if (!IsOursFpTrueWorldScale(GetStereoMode()))
    return;
  static bool wasF11 = false;
  if (!KeyPressedEdge(VK_F11, &wasF11))
    return;

  static const int kPresets[] = {100, 125, 150, 175, 200};
  constexpr int kN = 5;
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
  Log("TrueWorldScale: %d%% (F11) — HIGHER = smaller world (scale×IPD uncapped; "
      "fusion may break above ~150%%); kill=.scale=100 or stereo=186",
      pct);
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

uint32_t GetCanvasMaxDim() {
  int v = g_canvasMaxDim.load();
  if (v < 512)
    v = 512;
  if (v > 4096)
    v = 4096;
  return static_cast<uint32_t>(v);
}

uint32_t GetCanvasMaxDimGeneration() {
  return g_canvasMaxDimGen.load();
}

void SetCanvasMaxDim(uint32_t dim, bool fromHotkey) {
  if (dim < 512)
    dim = 512;
  if (dim > 4096)
    dim = 4096;
  g_canvasMaxDim.store(static_cast<int>(dim));
  g_canvasMaxDimGen.fetch_add(1);

  char path[MAX_PATH]{};
  if (GetAsiDir(path, MAX_PATH)) {
    strcat_s(path, "gtaiv_dxvk_vr.vres");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
      std::fprintf(f, "%u\n", dim);
      std::fclose(f);
    }
  }

  if (fromHotkey)
    Log("VrRes: F5 eye-canvas maxDim=%u (saved gtaiv_dxvk_vr.vres; RTs recreate next frame; "
        "SteamVR SS unchanged)",
        dim);
  else
    Log("VrRes: eye-canvas maxDim=%u (gtaiv_dxvk_vr.vres; F5 cycles 1024..2048 + recommended)",
        dim);
}

void ReloadCanvasMaxDim() {
  char buf[16]{};
  int v = 0;
  if (ReadSmallFile("gtaiv_dxvk_vr.vres", buf, sizeof(buf)) > 0 &&
      sscanf_s(buf, "%d", &v) == 1 && v >= 512 && v <= 4096) {
    g_canvasMaxDim.store(v);
    g_canvasMaxDimGen.fetch_add(1);
    Log("Config: vres maxDim=%d (gtaiv_dxvk_vr.vres) — eye-canvas cap; F5 cycles", v);
    return;
  }
  // No file: keep current atomic (Mode enter / default 1536).
}

void PollVrResHotkey() {
  // Mode 156: cycle eye-canvas maxDim. Also allow on 154/155 family for A/B.
  if (!IsHeadHideNativeOurs(GetStereoMode()) && !IsCleanDualLookMove(GetStereoMode()))
    return;
  static bool wasF5 = false;
  static bool primed = false;
  if (!primed) {
    ReloadCanvasMaxDim();
    wasF5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    primed = true;
    return;
  }
  if (!KeyPressedEdge(VK_F5, &wasF5))
    return;

  // Increments: common VR RT caps + SteamVR recommended (clamped).
  static const uint32_t kFixed[] = {1024, 1280, 1536, 1792, 2048};
  constexpr int kFixedN = 5;
  uint32_t rec = 0;
  if (vr::VRSystem()) {
    uint32_t rw = 0, rh = 0;
    vr::VRSystem()->GetRecommendedRenderTargetSize(&rw, &rh);
    rec = rw > rh ? rw : rh;
    if (rec < 512)
      rec = 0;
    if (rec > 4096)
      rec = 4096;
  }

  const uint32_t cur = GetCanvasMaxDim();
  int idx = 0;
  int best = 999999;
  for (int i = 0; i < kFixedN; ++i) {
    const int d = static_cast<int>(kFixed[i] > cur ? kFixed[i] - cur : cur - kFixed[i]);
    if (d < best) {
      best = d;
      idx = i;
    }
  }
  // If current matches recommended closer than fixed, treat as past last fixed.
  if (rec > 0) {
    const int dRec = static_cast<int>(rec > cur ? rec - cur : cur - rec);
    if (dRec < best)
      idx = kFixedN;  // "at recommended" → next = first fixed
  }
  const int nSteps = kFixedN + (rec > 0 ? 1 : 0);
  idx = (idx + 1) % nSteps;
  const uint32_t next = (idx < kFixedN) ? kFixed[idx] : rec;
  SetCanvasMaxDim(next, true);
  if (idx >= kFixedN)
    Log("VrRes: F5 picked SteamVR recommended maxDim=%u", next);
}

// --- in-game overlay menu accessors ----------------------------------------
// Thin wrappers over the same set+save pairs the F5..F11 cyclers use, so the
// menu cannot drift from the hotkeys. Clamping stays inside the Set* helpers.

void MenuSetSepCm(int cm) {
  SetSepCm(cm);
  SaveIpdFile(MenuGetSepCm());
  Log("StereoSep: %d cm (menu) — L4D2 IpdScale knob", MenuGetSepCm());
}

int MenuGetSepCm() {
  return static_cast<int>(g_sepM.load() * 100.f + 0.5f);
}

void MenuSetStereoScalePercent(int pct) {
  SetStereoScalePercent(pct);
  const int applied = MenuGetStereoScalePercent();
  SaveStereoScaleFile(applied);
  Log("StereoScale: %.2f (menu) — HIGHER = stronger 3D / smaller world", applied / 100.f);
}

int MenuGetStereoScalePercent() {
  return static_cast<int>(g_stereoScale.load() * 100.f + 0.5f);
}

void MenuSetWorldScalePercent(int pct) {
  SetWorldScalePercent(pct);
  const int applied = MenuGetWorldScalePercent();
  SaveScaleFile(applied);
  Log("TrueWorldScale: %d%% (menu) — HIGHER = smaller world; fusion may break above ~150%%",
      applied);
}

int MenuGetWorldScalePercent() {
  return static_cast<int>(g_worldScale.load() * 100.f + 0.5f);
}

void MenuSetWorldScalePreset(int idx) {
  if (idx < 0 || idx >= kWorldScalePresetN)
    return;
  ApplyWorldScalePreset(idx, true);
}

int MenuGetWorldScalePreset() {
  const int idx = g_worldScalePreset.load();
  return (idx < 0 || idx >= kWorldScalePresetN) ? kWorldScaleDefaultIdx : idx;
}

int MenuWorldScalePresetCount() {
  return kWorldScalePresetN;
}

const char* MenuWorldScalePresetName(int idx) {
  if (idx < 0 || idx >= kWorldScalePresetN)
    return "?";
  return kWorldScalePresets[idx].name;
}

void MenuSetFovAddDegrees(int deg) {
  if (deg < 0)
    deg = 0;
  if (deg > 40)
    deg = 40;
  SetFovAddDegrees(static_cast<float>(deg));
  SaveFovAddFile(deg);
  Log("FovAdd: %d deg (menu) — HIGHER = more canvas crop / zoom-IN", deg);
}

int MenuGetFovAddDegrees() {
  return static_cast<int>(GetFovAddDegrees() + 0.5f);
}

int MenuGetEyeForwardCm() {
  // GetEyeForwardMeters() also performs the one-time file read + log.
  return static_cast<int>(GetEyeForwardMeters() * 100.f + 0.5f);
}

}  // namespace asi
