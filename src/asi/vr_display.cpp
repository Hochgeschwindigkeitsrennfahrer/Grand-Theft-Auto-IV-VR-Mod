#include "vr_display.h"
#include "log.h"
#include "stereo_config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <openvr.h>

namespace {
float Max4(float a, float b, float c, float d) {
  return (std::max)((std::max)(a, b), (std::max)(c, d));
}
}  // namespace

namespace asi {
namespace {

std::atomic<bool> g_loggedInfo{false};
std::atomic<bool> g_boundsReady{false};
vr::VRTextureBounds_t g_boundsL{};
vr::VRTextureBounds_t g_boundsR{};
float g_tanHalfH = 0.f;
float g_tanHalfV = 0.f;
// Raw per-eye frustum tangents (left/top negative), cached with the bounds.
float g_rawL[4]{};  // l, r, t, b
float g_rawR[4]{};

std::atomic<float> g_gameTanH{std::tan(0.5f * 70.f * 3.14159265f / 180.f)};
std::atomic<float> g_gameTanV{std::tan(0.5f * 70.f * 3.14159265f / 180.f) * 9.f / 16.f};
std::atomic<bool> g_loggedInset{false};
// Mode 36: true engine FOV from CCam+0x60 (after fovadd). Blocks D3DTS_PROJECTION
// probe from clobbering — Rage ignores Set/GetTransform projection (Mode 15 dead).
std::atomic<bool> g_fovFromCCam{false};
std::atomic<float> g_bbAspect{16.f / 9.f};
std::atomic<uint32_t> g_fovPublishGen{0};
// Mode 37: latch tangents for one L→R pair (same canvas geometry both eyes).
std::atomic<bool> g_pairLatchOn{false};
std::atomic<float> g_pairLatchH{0.f};
std::atomic<float> g_pairLatchV{0.f};

float FovFromProjectionRaw(float left, float right) {
  const float fovRad = std::atan(right) - std::atan(left);
  return fovRad * (180.f / 3.14159265f);
}

void EnsureBoundsCache() {
  if (g_boundsReady.load())
    return;
  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys)
    return;

  float lL = 0.f, lR = 0.f, lT = 0.f, lB = 0.f;
  float rL = 0.f, rR = 0.f, rT = 0.f, rB = 0.f;
  sys->GetProjectionRaw(vr::Eye_Left, &lL, &lR, &lT, &lB);
  sys->GetProjectionRaw(vr::Eye_Right, &rL, &rR, &rT, &rB);

  const float tanH = Max4(-lL, lR, -rL, rR);
  const float tanV = Max4(-lT, lB, -rT, rB);
  if (!(tanH > 0.05f) || !(tanV > 0.05f))
    return;

  g_tanHalfH = tanH;
  g_tanHalfV = tanV;

  g_rawL[0] = lL;
  g_rawL[1] = lR;
  g_rawL[2] = lT;
  g_rawL[3] = lB;
  g_rawR[0] = rL;
  g_rawR[1] = rR;
  g_rawR[2] = rT;
  g_rawR[3] = rB;

  // L4D2 cover-crop (unused while we submit native FOV).
  g_boundsL.uMin = 0.5f + 0.5f * lL / tanH;
  g_boundsL.uMax = 0.5f + 0.5f * lR / tanH;
  g_boundsL.vMin = 0.5f - 0.5f * lB / tanV;
  g_boundsL.vMax = 0.5f - 0.5f * lT / tanV;
  g_boundsR.uMin = 0.5f + 0.5f * rL / tanH;
  g_boundsR.uMax = 0.5f + 0.5f * rR / tanH;
  g_boundsR.vMin = 0.5f - 0.5f * rB / tanV;
  g_boundsR.vMax = 0.5f - 0.5f * rT / tanV;
  g_boundsReady.store(true);
}

bool GetAsiDirLocal(char* out, DWORD outLen) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&GetAsiDirLocal), &self))
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

// Optional gtaiv_dxvk_vr.fov next to the ASI: horizontal game FOV in degrees (30..150).
// Lets the user calibrate world size in Mode 14 without a rebuild. Read once.
float ReadFovOverrideDegOnce() {
  static std::atomic<bool> s_read{false};
  static std::atomic<float> s_deg{0.f};
  if (s_read.exchange(true))
    return s_deg.load();

  char path[MAX_PATH]{};
  if (!GetAsiDirLocal(path, MAX_PATH))
    return 0.f;
  strcat_s(path, "gtaiv_dxvk_vr.fov");

  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return 0.f;
  char buf[16]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  int deg = 0;
  if (n > 0 && sscanf_s(buf, "%d", &deg) == 1 && deg >= 30 && deg <= 150) {
    s_deg.store(static_cast<float>(deg));
    Log("VrDisplay: game FOV override %d deg horizontal (gtaiv_dxvk_vr.fov)", deg);
  }
  return s_deg.load();
}

// Canvas zoom DEAD (2026-07-24 headset): claiming wider FOV via gtaiv_dxvk_vr.zoom
// warped the environment on every head move (same class as FusionFix FOV look-up warp).
// Always true FOV (scale=1). File ignored; keep zoom=100 or delete for clarity.
float ReadCanvasZoomScaleOnce() {
  static std::atomic<bool> s_logged{false};
  if (!s_logged.exchange(true))
    Log("CanvasZoom: DISABLED (warp rejected) — true FOV only; file ignored "
        "(was gtaiv_dxvk_vr.zoom; kill was 100)");
  return 1.f;
}

// Soft inset: map game FOV into HMD cover FOV without square-crop UV (that broke fusion).
bool BuildSoftInset(float tanGameH, float tanGameV, vr::VRTextureBounds_t* out) {
  float tanHmdH = 0.f, tanHmdV = 0.f;
  if (!GetCoverFovTangents(&tanHmdH, &tanHmdV))
    return false;
  if (!(tanGameH > 0.05f) || !(tanGameV > 0.05f))
    return false;

  // Clamp so we never invent extreme UV (black bars / blur).
  float rh = tanGameH / tanHmdH;
  float rv = tanGameV / tanHmdV;
  if (rh < 0.55f)
    rh = 0.55f;
  if (rh > 1.0f)
    rh = 1.0f;
  if (rv < 0.55f)
    rv = 0.55f;
  if (rv > 1.0f)
    rv = 1.0f;

  out->uMin = 0.5f - 0.5f / rh;
  out->uMax = 0.5f + 0.5f / rh;
  out->vMin = 0.5f - 0.5f / rv;
  out->vMax = 0.5f + 0.5f / rv;
  return true;
}

}  // namespace

float GetVrHorizontalFovDegrees() {
  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys)
    return 0.f;
  float lL = 0.f, lR = 0.f, lT = 0.f, lB = 0.f;
  float rL = 0.f, rR = 0.f, rT = 0.f, rB = 0.f;
  sys->GetProjectionRaw(vr::Eye_Left, &lL, &lR, &lT, &lB);
  sys->GetProjectionRaw(vr::Eye_Right, &rL, &rR, &rT, &rB);
  const float fovL = FovFromProjectionRaw(lL, lR);
  const float fovR = FovFromProjectionRaw(rL, rR);
  if (!(fovL > 1.f) || !(fovR > 1.f))
    return 0.f;
  return 0.5f * (fovL + fovR);
}

bool GetEyeSubmitBounds(vr::EVREye eye, vr::VRTextureBounds_t* out) {
  if (!out)
    return false;
  EnsureBoundsCache();
  if (!g_boundsReady.load())
    return false;
  *out = (eye == vr::Eye_Right) ? g_boundsR : g_boundsL;
  return true;
}

bool GetCoverFovTangents(float* tanHalfHoriz, float* tanHalfVert) {
  if (!tanHalfHoriz || !tanHalfVert)
    return false;
  EnsureBoundsCache();
  if (!g_boundsReady.load())
    return false;
  *tanHalfHoriz = g_tanHalfH;
  *tanHalfVert = g_tanHalfV;
  return g_tanHalfH > 0.05f && g_tanHalfV > 0.05f;
}

void SetBackbufferAspect(float aspectWH) {
  if (aspectWH > 0.5f && aspectWH < 3.f)
    g_bbAspect.store(aspectWH);
}

float GetBackbufferAspect() {
  return g_bbAspect.load();
}

bool IsGameFovFromCCamActive() {
  return g_fovFromCCam.load();
}

uint32_t GetGameFovPublishGeneration() {
  return g_fovPublishGen.load();
}

void PublishGameFovFromCCamDegrees(float ccamDeg, float aspectWH) {
  if (!(ccamDeg >= 5.f) || !(ccamDeg <= 160.f) || !std::isfinite(ccamDeg))
    return;
  // Mode 16 probe: CCam 45.000 → rendered vertical ≈ 58.7° (tanV≈0.562 at 16:9).
  constexpr float kEng = 58.7f / 45.f;
  float vDeg = ccamDeg * kEng;
  if (vDeg > 170.f)
    vDeg = 170.f;
  const float tanV = std::tan(0.5f * vDeg * 3.14159265f / 180.f);
  float aspect = aspectWH;
  if (!(aspect > 0.5f) || !(aspect < 3.f))
    aspect = g_bbAspect.load();
  if (!(aspect > 0.5f) || !(aspect < 3.f))
    aspect = 16.f / 9.f;
  const float tanH = tanV * aspect;
  if (!(tanH > 0.05f) || !(tanV > 0.05f) || !(tanH < 5.f) || !(tanV < 5.f))
    return;

  const float curH = g_gameTanH.load();
  // Mode 44 owns fixed-size eye RTs, so retain the first stable true-FOV tangent
  // through ordinary CCam noise. A real zoom/camera change still exceeds 5%.
  const StereoMode mode = GetStereoMode();
  const float gate = (mode == StereoMode::FovCanvasMotionGuardRtLock ||
                      IsHeadOwnedCamFamily(mode))
                         ? 0.05f
                         : 0.025f;
  const bool changed = !(curH > 0.05f) || std::fabs(tanH - curH) > gate * curH;
  if (!changed && g_fovFromCCam.load())
    return;
  g_gameTanH.store(tanH);
  g_gameTanV.store(tanV);
  g_fovFromCCam.store(true);
  if (changed) {
    const uint32_t gen = ++g_fovPublishGen;
    if (gen <= 6 || (gen % 120) == 0)
      Log("VrDisplay: gameTan from TRUE CCam FOV=%.1f -> tan=(%.3f,%.3f) gen=%u "
          "(canvas uses engine FOV; not D3DTS_PROJECTION)",
          ccamDeg, tanH, tanV, gen);
  }
}

void UpdateGameFovFromDevice(IDirect3DDevice9* device) {
  if (!device)
    return;

  uint32_t bw = 0, bh = 0;
  IDirect3DSurface9* bb = nullptr;
  if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
    D3DSURFACE_DESC d{};
    if (SUCCEEDED(bb->GetDesc(&d))) {
      bw = d.Width;
      bh = d.Height;
    }
    bb->Release();
  }
  if (bw >= 16 && bh >= 16) {
    const float aspect = static_cast<float>(bw) / static_cast<float>(bh);
    if (aspect > 0.5f && aspect < 3.f)
      g_bbAspect.store(aspect);
  }

  // Mode 36: canvas FOV comes from live CCam — do NOT revert to stale projection.
  if (g_fovFromCCam.load())
    return;

  D3DMATRIX proj{};
  if (FAILED(device->GetTransform(D3DTS_PROJECTION, &proj)))
    return;
  const float a = proj.m[0][0];
  const float b = proj.m[1][1];
  if (!(a > 0.2f) || !(a < 8.f) || !(b > 0.2f) || !(b < 8.f))
    return;

  // ALWAYS derive tanV from the backbuffer aspect. GetTransform at EndScene can
  // return a square side-pass projection (mirror/envmap) — trusting m[1][1] made
  // gameTan flap between 0.562 and 1.0 (2026-07-24: lost bars, wrong proportions,
  // RT-recreate stutter). Aspect-derivation also stays correct with the Mode 17
  // FOV patch, because the game derives horizontal FOV from vertical × aspect.
  float tanH = 1.f / a;
  float tanV = tanH * 9.f / 16.f;
  if (bw >= 16 && bh >= 16) {
    const float aspect = static_cast<float>(bw) / static_cast<float>(bh);
    if (aspect > 0.5f && aspect < 3.f)
      tanV = tanH / aspect;
  }
  if (!(tanH > 0.05f) || !(tanV > 0.05f) || !(tanH < 5.f) || !(tanV < 5.f))
    return;

  // Stability gate: only accept a NEW tangent after it has been steady for 30
  // frames. Prevents per-frame canvas-rect flapping (= objects jumping L/R in
  // temporal stereo) if the engine FOV oscillates or side passes interfere.
  const float cur = g_gameTanH.load();
  if (std::fabs(tanH - cur) <= 0.01f * cur) {
    return;  // unchanged — keep current
  }
  static float s_candidate = 0.f;
  static int s_stable = 0;
  if (s_candidate > 0.f && std::fabs(tanH - s_candidate) <= 0.01f * s_candidate) {
    if (++s_stable >= 30) {
      g_gameTanH.store(tanH);
      g_gameTanV.store(tanV);
      s_candidate = 0.f;
      s_stable = 0;
      Log("VrDisplay: gameTan committed (%.3f,%.3f) after stability gate", tanH, tanV);
    }
  } else {
    s_candidate = tanH;
    s_stable = 1;
  }
}

bool GetEyeRawProjection(vr::EVREye eye, float* left, float* right, float* top, float* bottom) {
  if (!left || !right || !top || !bottom)
    return false;
  EnsureBoundsCache();
  if (!g_boundsReady.load())
    return false;
  const float* raw = (eye == vr::Eye_Right) ? g_rawR : g_rawL;
  *left = raw[0];
  *right = raw[1];
  *top = raw[2];
  *bottom = raw[3];
  return true;
}

bool BuildHmdEyeProjection(vr::EVREye eye, float* out16, const float* template16, float zn,
                           float zf) {
  if (!out16)
    return false;
  float l = 0.f, r = 0.f, t = 0.f, b = 0.f;
  if (!GetEyeRawProjection(eye, &l, &r, &t, &b) || !(r > l) || !(b > t))
    return false;
  if (!(zn > 1e-4f) || !(zf > zn + 1e-3f)) {
    zn = 0.15f;
    zf = 2500.f;
  }

  const float invRL = 1.f / (r - l);
  const float invTB = 1.f / (b - t);
  const float sx = 2.f * invRL;
  const float sy = 2.f * invTB;
  const float ox = (r + l) * invRL;
  const float oy = (t + b) * invTB;

  // Prefer adapting a game projection template (keeps Rage near/far + layout).
  if (template16 && template16[0] > 0.1f && template16[0] < 8.f && template16[5] > 0.1f &&
      template16[5] < 8.f && std::fabs(template16[15]) < 0.08f &&
      std::fabs(template16[11]) > 0.85f && std::fabs(template16[11]) < 1.15f) {
    for (int i = 0; i < 16; ++i)
      out16[i] = template16[i];
    out16[0] = sx;
    out16[5] = sy;
    // Off-axis terms — this is what matrix-IPD alone cannot provide.
    out16[8] = ox;
    out16[9] = oy;
    return std::isfinite(out16[0]) && std::isfinite(out16[5]) && std::isfinite(out16[8]) &&
           std::isfinite(out16[9]);
  }

  // Fresh D3D-style row-major perspective (camera looks −Z, +Y up).
  std::memset(out16, 0, 16 * sizeof(float));
  out16[0] = sx;
  out16[5] = sy;
  out16[8] = ox;
  out16[9] = oy;
  out16[10] = zf / (zn - zf);
  out16[11] = -1.f;
  out16[14] = (zf * zn) / (zn - zf);
  out16[15] = 0.f;
  return std::isfinite(out16[0]) && std::isfinite(out16[10]) && std::isfinite(out16[14]);
}

void LatchGameFovForPair() {
  g_pairLatchH.store(g_gameTanH.load());
  g_pairLatchV.store(g_gameTanV.load());
  g_pairLatchOn.store(true);
}

void ClearGameFovPairLatch() {
  g_pairLatchOn.store(false);
}

void GetGameFovTangents(float* tanHalfH, float* tanHalfV) {
  float tanH = g_gameTanH.load();
  float tanV = g_gameTanV.load();
  const float ovrDeg = ReadFovOverrideDegOnce();
  if (ovrDeg > 0.f) {
    // Keep the measured aspect ratio, override only the horizontal FOV.
    const float ratio = (tanH > 0.05f) ? (tanV / tanH) : (9.f / 16.f);
    tanH = std::tan(0.5f * ovrDeg * 3.14159265f / 180.f);
    tanV = tanH * ratio;
  }
  // Zoom path forced to 1.0 (DISABLED) — see ReadCanvasZoomScaleOnce.
  (void)ReadCanvasZoomScaleOnce();
  if (tanHalfH)
    *tanHalfH = tanH;
  if (tanHalfV)
    *tanHalfV = tanV;
}

// Mode 37 pair-hold: same tangents for L+R StretchRect of one promote pair.
bool GetLatchedGameFovTangents(float* tanHalfH, float* tanHalfV) {
  if (!g_pairLatchOn.load())
    return false;
  const float tanH = g_pairLatchH.load();
  const float tanV = g_pairLatchV.load();
  if (!(tanH > 0.05f) || !(tanV > 0.05f))
    return false;
  if (tanHalfH)
    *tanHalfH = tanH;
  if (tanHalfV)
    *tanHalfV = tanV;
  return true;
}

bool GetNativeFovInsetBounds(vr::EVREye eye, vr::VRTextureBounds_t* out) {
  (void)eye;
  if (!out)
    return false;
  if (!BuildSoftInset(g_gameTanH.load(), g_gameTanV.load(), out))
    return false;
  if (!g_loggedInset.exchange(true)) {
    Log("VrDisplay: soft-inset (no square crop) tanH=%.3f tanV=%.3f u=%.3f..%.3f v=%.3f..%.3f",
        g_gameTanH.load(), g_gameTanV.load(), out->uMin, out->uMax, out->vMin, out->vMax);
  }
  return true;
}

void LogVrDisplayInfo() {
  if (g_loggedInfo.exchange(true))
    return;
  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys) {
    Log("VrDisplay: VRSystem null");
    return;
  }
  uint32_t rw = 0, rh = 0;
  sys->GetRecommendedRenderTargetSize(&rw, &rh);
  Log("VrDisplay: SteamVR recommended %ux%u (compositor only). Submit = game BB, nullptr bounds.",
      rw, rh);
  EnsureBoundsCache();
}

bool InstallVrDisplayHooks() {
  Log("VrDisplay: soft-inset Submit; square is engine commandline only (optional, off by default)");
  LogVrDisplayInfo();
  return false;
}

}  // namespace asi
