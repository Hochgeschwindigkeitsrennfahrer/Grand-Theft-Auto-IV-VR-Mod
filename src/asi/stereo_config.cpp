#include "stereo_config.h"
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
    const int cm =
        static_cast<int>(ReadHmdIpdMeters() * 100.f + 0.5f);
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
    Log("WorldScale6DoF: %.2f (file gtaiv_dxvk_vr.scale) — lean only; F7 = visual presets",
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

int ParseModeFile(const char* buf, size_t n) {
  // Three-digit Mode 120–136 (clean dual+lookmove family; 120=LKG, 121+=experiments).
  // Two-digit modes 10..57 otherwise. Mode 46 remaps to protected 45 below.
  if (n >= 3 && buf[0] == '1' && buf[1] == '3' && buf[2] >= '0' && buf[2] <= '6')
    return 130 + (buf[2] - '0');
  if (n >= 3 && buf[0] == '1' && buf[1] == '2' && buf[2] >= '0' && buf[2] <= '9')
    return 120 + (buf[2] - '0');
  if (n >= 2 && buf[0] >= '1' && buf[0] <= '5' && buf[1] >= '0' && buf[1] <= '9') {
    const int v = 10 * (buf[0] - '0') + (buf[1] - '0');
    if (v <= 57)
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
  if (v == static_cast<int>(StereoMode::HeadOwnedCamFullPose)) {
    v = static_cast<int>(StereoMode::HeadOwnedCamSpike);
    Log("StereoMode: requested 46 is DISABLED (post-load freeze); using protected mode 45");
  }
  int prev = g_mode.load();
  if (v >= 0 &&
      (v <= 57 || IsCleanDualLookMove(static_cast<StereoMode>(v)))) {
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
                           v == static_cast<int>(StereoMode::HeadOwnedCamFullPose) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamLeveledPitchFlip) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamPitchStable) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamPedCoupled) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamStereoAlways) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamStereoAer) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamStereoSwap) ||
                           v == static_cast<int>(StereoMode::HeadOwnedCamStereoSoftGuard) ||
                           IsOpenXrDirectMode(static_cast<StereoMode>(v)) ||
                           IsCleanDualLookMove(static_cast<StereoMode>(v)))) {
    const bool directOpenXr = IsOpenXrDirectMode(static_cast<StereoMode>(v));
    // Direct OpenXR must not query vr::VRSystem or create legacy OpenVR
    // defaults. Existing user files are still read below; missing files keep
    // the neutral in-memory defaults (WorldScale 1.0, IPD unused in Mode 55).
    if (!directOpenXr)
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
      (mode > 57 && !IsCleanDualLookMove(static_cast<StereoMode>(mode))))
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

bool IsOpenXrDirectMode(StereoMode mode) {
  return mode == StereoMode::OpenXrSameFrameWvp ||
         mode == StereoMode::OpenXrImmersiveMono ||
         mode == StereoMode::OpenXrDrawSceneStereo ||
         mode == StereoMode::OpenXrTemporalStereo;
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
         IsOpenXrDirectMode(mode) ||
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
         mode == StereoMode::HeadOwnedCamStereoSoftGuard ||
         mode == StereoMode::OpenXrTemporalStereo;
}

bool UsesAerPoseSubmit(StereoMode mode) {
  // Mode 120: real WaitGetPoses+Submit_Default. Mode 136 uses late-latch path
  // (IsCleanDualLateLatch) separately — still Submit_TextureWithPose, but pose
  // is sampled immediately before Submit, not capture-time AER.
  return mode == StereoMode::AerPoseSubmit ||
         mode == StereoMode::HeadOwnedCamStereoAer ||
         mode == StereoMode::HeadOwnedCamStereoSoftGuard;
}

bool UsesPedCoupledYaw(StereoMode mode) {
  // Mode 120: pedCoupled OFF — look_move owns body facing; cam is pure HMD.
  if (IsCleanDualLookMove(mode))
    return false;
  return mode == StereoMode::HeadOwnedCamPedCoupled ||
         mode == StereoMode::OpenXrImmersiveMono ||
         mode == StereoMode::OpenXrDrawSceneStereo ||
         UsesStereoAlwaysDistinct(mode);
}

bool UsesPreCaptureCamRefresh(StereoMode mode) {
  return mode == StereoMode::HeadOwnedCamPitchStable ||
         mode == StereoMode::HeadOwnedCamPedCoupled ||
         mode == StereoMode::OpenXrImmersiveMono ||
         mode == StereoMode::OpenXrDrawSceneStereo ||
         UsesStereoAlwaysDistinct(mode) ||
         IsCleanDualLookMove(mode);
}

bool UsesRtLockFovGate(StereoMode mode) {
  return mode == StereoMode::FovCanvasMotionGuardRtLock ||
         mode == StereoMode::HeadOwnedCamSpike ||
         mode == StereoMode::HeadOwnedCamFullPose ||
         mode == StereoMode::HeadOwnedCamLeveledPitchFlip ||
         mode == StereoMode::HeadOwnedCamPitchStable ||
         mode == StereoMode::HeadOwnedCamPedCoupled ||
         mode == StereoMode::OpenXrImmersiveMono ||
         mode == StereoMode::OpenXrDrawSceneStereo ||
         UsesStereoAlwaysDistinct(mode) ||
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
