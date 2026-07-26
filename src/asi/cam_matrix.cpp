#include "cam_matrix.h"
#include "aob.h"
#include "hmd_pose.h"
#include "log.h"
#include "ped_hide.h"
#include "stereo_config.h"
#include "stereo_eye.h"
#include "stereo_render.h"
#include "vr_display.h"
#include "vr_move.h"

#include <d3d9.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <openvr.h>

namespace asi {
namespace {

struct Vec4 {
  float x, y, z, w;
};

struct Matrix44 {
  Vec4 right;  // X
  Vec4 up;     // Y = forward (RAGE)
  Vec4 at;     // Z = up
  Vec4 pos;
};

using CopyMat_t = void(__fastcall*)(Matrix44* mat, void* edx, void* arg2);
using FindPlayerPed_t = void*(__cdecl*)(int32_t id);

CopyMat_t g_origCopyMat = nullptr;
FindPlayerPed_t g_FindPlayerPed = nullptr;

std::atomic<bool> g_hooksOk{false};
std::atomic<bool> g_gameplayActive{false};  // false until past title (avoids blackscreen)
std::atomic<uint32_t> g_applyCount{0};

// Eye placement. Inspiration FP uses ScriptHook SET_DRAW + FPX/FPY/FPZ (often 0).
// We: PedHide via CE SetDraw helper (no natives) + eyefwd/camoff knobs.
constexpr float kEyeHeight = 0.70f;  // along ped "at" (world up) — skull center
constexpr float kPosScale = 1.0f;

constexpr uint32_t kPedVehicleOff = 0xB30;  // CPed::m_pVehicle (CE)
constexpr uint32_t kEntityMatrixOff = 0x20; // CEntity::m_pMatrix

bool g_havePosBaseline = false;
float g_basePx = 0.f, g_basePy = 0.f, g_basePz = 0.f;

bool g_haveLastEye = false;
Vec4 g_lastEye{};
Matrix44* g_liveCamMat = nullptr;

bool g_haveLastApplied = false;
Vec4 g_lastAppliedPos{};

constexpr int kMaxTrackedMats = 8;
Matrix44* g_trackedMats[kMaxTrackedMats]{};
int g_trackedCount = 0;
bool g_loggedD3dPush = false;

void TrackCamMat(Matrix44* mat) {
  if (!mat)
    return;
  for (int i = 0; i < g_trackedCount; ++i) {
    if (g_trackedMats[i] == mat)
      return;
  }
  if (g_trackedCount >= kMaxTrackedMats)
    return;
  g_trackedMats[g_trackedCount++] = mat;
}

void OvrToGta(float ox, float oy, float oz, float* gx, float* gy, float* gz) {
  *gx = ox;
  *gy = -oz;
  *gz = oy;
}

void Normalize(float* x, float* y, float* z) {
  const float len = std::sqrt((*x) * (*x) + (*y) * (*y) + (*z) * (*z));
  if (len < 1e-6f) {
    *x = 0.f;
    *y = 1.f;
    *z = 0.f;
    return;
  }
  *x /= len;
  *y /= len;
  *z /= len;
}

bool TryGetPedEyePos(Vec4* outEye) {
  if (!g_FindPlayerPed || !outEye)
    return false;

  void* ped = g_FindPlayerPed(0);
  if (!ped)
    return false;

  // CEntity::m_pMatrix @ +0x20 (plugin-sdk CE)
  auto* pMat = *reinterpret_cast<Matrix44**>(reinterpret_cast<uint8_t*>(ped) + 0x20);
  if (!pMat) {
    // Fallback: CSimpleTransform pos @ +0x10
    const float* t = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(ped) + 0x10);
    outEye->x = t[0];
    outEye->y = t[1];
    outEye->z = t[2] + kEyeHeight;
    outEye->w = 1.f;
    return true;
  }

  outEye->x = pMat->pos.x + pMat->at.x * kEyeHeight;
  outEye->y = pMat->pos.y + pMat->at.y * kEyeHeight;
  outEye->z = pMat->pos.z + pMat->at.z * kEyeHeight;
  outEye->w = 1.f;
  return true;
}

// Vehicle heading-follow (gtaiv_dxvk_vr.vehfollow): camera yaw tracks the vehicle's
// yaw delta since entry/recenter, HMD free look stays on top. Baseline resets on
// exit and on F9.
float g_vehYawBase = 0.f;
bool g_haveVehYawBase = false;

float WrapPi(float a) {
  while (a > 3.14159265f)
    a -= 6.2831853f;
  while (a < -3.14159265f)
    a += 6.2831853f;
  return a;
}

float ComputeVehicleYawOffset() {
  const int mode = GetVehicleFollowMode();
  if (mode == 0 || !g_FindPlayerPed)
    return 0.f;
  void* ped = g_FindPlayerPed(0);
  if (!ped) {
    g_haveVehYawBase = false;
    return 0.f;
  }
  void* veh = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ped) + kPedVehicleOff);
  if (!veh) {
    g_haveVehYawBase = false;
    return 0.f;
  }
  auto* m = *reinterpret_cast<Matrix44**>(reinterpret_cast<uint8_t*>(veh) + kEntityMatrixOff);
  if (!m)
    return 0.f;
  const float fx = m->up.x;  // RAGE: "up" row = forward
  const float fy = m->up.y;
  if (fx * fx + fy * fy < 1e-6f)
    return 0.f;
  const float yaw = std::atan2(-fx, fy);
  if (!g_haveVehYawBase) {
    g_vehYawBase = yaw;
    g_haveVehYawBase = true;
    Log("VehFollow: baseline yaw=%.1f deg", yaw * 57.2957795f);
  }
  float d = WrapPi(yaw - g_vehYawBase);
  if (mode == 2)
    d = -d;
  return d;
}

bool SaneWorldPos(const Vec4& p) {
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
    return false;
  // Rough Liberty City bounds — reject menu/junk origins
  if (p.x < -6000.f || p.x > 6000.f || p.y < -6000.f || p.y > 6000.f)
    return false;
  if (p.z < -500.f || p.z > 2500.f)
    return false;
  return true;
}

void ApplyHmdToCam(Matrix44* mat) {
  if (!g_gameplayActive.load())
    return;

  vr::HmdMatrix34_t h{};
  // Prefer once-per-frame snapshot when available (matches Mode74 dual L/R).
  if (!GetFrameHmdPoseMatrix(&h))
    return;

  Vec4 eye{};
  if (!TryGetPedEyePos(&eye) || !SaneWorldPos(eye)) {
    // No real ped yet — never reuse stale eye (menu → freeze risk)
    return;
  }
  g_lastEye = eye;
  g_haveLastEye = true;

  // Seated 6DoF on top of ped eye (F9 resets baseline).
  // Mode74: DRAW-PATH Mode74ApplyHmdEyeLocal owns lean (CamMatrix bake often never
  // reaches ReplayDispatch) — skip here to avoid double translation.
  const bool mode74SkipLean = IsMode74Family(GetStereoMode());
  float px, py, pz;
  OvrToGta(h.m[0][3], h.m[1][3], h.m[2][3], &px, &py, &pz);
  if (!g_havePosBaseline) {
    g_basePx = px;
    g_basePy = py;
    g_basePz = pz;
    g_havePosBaseline = true;
  }
  if (!mode74SkipLean) {
    const float leanGain = GetLeanGain();
    eye.x += (px - g_basePx) * kPosScale / leanGain;
    eye.y += (py - g_basePy) * kPosScale / leanGain;
    eye.z += (pz - g_basePz) * kPosScale / leanGain;
  }

  float fx, fy, fz;
  OvrToGta(-h.m[0][2], -h.m[1][2], -h.m[2][2], &fx, &fy, &fz);
  Normalize(&fx, &fy, &fz);

  // Right-stick yaw offset (controller camera L/R) + optional vehicle follow, on top of HMD
  {
    const float yawOff = GetControllerYawOffset() + ComputeVehicleYawOffset();
    const float c = std::cos(yawOff);
    const float s = std::sin(yawOff);
    const float nfx = fx * c - fy * s;
    const float nfy = fx * s + fy * c;
    fx = nfx;
    fy = nfy;
    Normalize(&fx, &fy, &fz);
  }

  // right = forward × worldUp(0,0,1)
  float rx = fy;
  float ry = -fx;
  float rz = 0.f;
  float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
  if (rlen < 1e-4f) {
    rx = 1.f;
    ry = 0.f;
    rz = 0.f;
  } else {
    rx /= rlen;
    ry /= rlen;
    rz /= rlen;
  }

  float ux = ry * fz - rz * fy;
  float uy = rz * fx - rx * fz;
  float uz = rx * fy - ry * fx;
  Normalize(&ux, &uy, &uz);

  const float eyeFwd = GetEyeForwardMeters();
  eye.x += fx * eyeFwd;
  eye.y += fy * eyeFwd;
  eye.z += fz * eyeFwd;

  // Inspiration FirstPerson.ini FPX/FPY/FPZ (our units: cm via gtaiv_dxvk_vr.camoff).
  float offR = 0.f, offF = 0.f, offU = 0.f;
  GetCamOffsetMeters(&offR, &offF, &offU);
  if (offR != 0.f || offF != 0.f || offU != 0.f) {
    eye.x += rx * offR + fx * offF + ux * offU;
    eye.y += ry * offR + fy * offF + uy * offU;
    eye.z += rz * offR + fz * offF + uz * offU;
  }

  // Inspiration-style head/hair hide (CE SetDraw helper — not EndScene natives).
  UpdatePedHeadHide();

  // Stereo eye origin — L4D2VR GetViewOriginLeft/Right:
  //   origin + forward*(-eyeZ*scale) + right*(±IPD*ipdScale*scale/2)
  // Cover FOV + TextureBounds (Submit) handle fusion; IPD alone is not enough.
  // Mode74 family (outside dual): DRAW-PATH OffsetViewLocal owns ±IPD — skip here
  // to avoid CamMatrix+inject double-IPD (felt like stereo jump). During same-frame
  // dual CamMatrix still supplies IPD (VS often silent between walks).
  float ipdX = 0.f, ipdY = 0.f, ipdZ = 0.f;
  const bool rightEye = (GetStereoEye() == StereoEye::Right);
  const bool mode74 = IsMode74Family(GetStereoMode());
  const bool mode74OwnsIpd = mode74 && !StereoInDualPass();
  if (GetStereoMode() >= StereoMode::DualIpd && !mode74OwnsIpd) {
    float hrx, hry, hrz;
    OvrToGta(h.m[0][0], h.m[1][0], h.m[2][0], &hrx, &hry, &hrz);
    float hrlen = std::sqrt(hrx * hrx + hry * hry + hrz * hrz);
    if (hrlen > 1e-4f) {
      hrx /= hrlen;
      hry /= hrlen;
      hrz /= hrlen;
    } else {
      hrx = rx;
      hry = ry;
      hrz = rz;
    }

    // Cached EyeToHead forward (OpenVR col2 translation) like L4D2 m_EyeZ.
    // Never call VRSystem from CopyMat hot path (perf).
    float eyeZ = 0.f;
    vr::HmdMatrix34_t e2h{};
    if (GetCachedEyeToHead(rightEye, &e2h))
      eyeZ = e2h.m[2][3];

    // LeanGain (F7) = 6DoF only. StereoScale (F6, default 1.15, cap 1.30) =
    // soft disparity for size-without-fusion-break. LeanGain does not touch IPD.
    const float half = 0.5f * GetStereoSepMeters() * GetStereoScale();
    const float s = rightEye ? half : -half;
    const float fzOff = -eyeZ;
    ipdX = hrx * s + fx * fzOff;
    ipdY = hry * s + fy * fzOff;
    ipdZ = hrz * s + fz * fzOff;
    if (std::isfinite(ipdX) && std::isfinite(ipdY) && std::isfinite(ipdZ) &&
        std::fabs(ipdX) < 20.f && std::fabs(ipdY) < 20.f && std::fabs(ipdZ) < 20.f) {
      eye.x += ipdX;
      eye.y += ipdY;
      eye.z += ipdZ;
    } else {
      ipdX = ipdY = ipdZ = 0.f;
    }
  }

  if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
    return;

  mat->right = {rx, ry, rz, 0.f};
  mat->up = {fx, fy, fz, 0.f};
  mat->at = {ux, uy, uz, 0.f};
  mat->pos = eye;

  g_lastAppliedPos = eye;
  g_haveLastApplied = true;

  const uint32_t n = ++g_applyCount;
  if (n <= 5 || (n % 300) == 0) {
    const float yawDeg = std::atan2(fx, fy) * (180.f / 3.14159265f);
    const float pitchDeg =
        std::asin((std::max)(-1.f, (std::min)(1.f, fz))) * (180.f / 3.14159265f);
    Log("CamMatrix: FP lock #%u %s pos=(%.3f,%.3f,%.3f) fwd=(%.2f,%.2f,%.2f) "
        "right=(%.2f,%.2f,%.2f) yaw=%.1f pitch=%.1f ipd=(%.4f,%.4f,%.4f) sep=%.0fcm "
        "eyeFwd=%.0fcm dual=%d",
        n, rightEye ? "R" : "L", eye.x, eye.y, eye.z, fx, fy, fz, rx, ry, rz, yawDeg,
        pitchDeg, ipdX, ipdY, ipdZ, GetStereoSepMeters() * 100.f, eyeFwd * 100.f,
        StereoInDualPass() ? 1 : 0);
  }
}

// ---- Mode 16: read-only FOV probe ------------------------------------------
// Scan floats around the camera matrix over many frames; report offsets whose
// value is stable and plausible as a FOV (degrees ~15..120 or radians ~0.3..2.1).
// The engine FOV should sit near ~58.7 deg vertical (or ~1.02 rad) at 16:9.
constexpr int kProbeStartOff = -0x80;  // bytes relative to mat
constexpr int kProbeSlots = 96;        // covers -0x80 .. +0xFC step 4

struct FovProbeState {
  Matrix44* mat;
  uint32_t samples;
  uint32_t reports;
  bool readable;
  float mn[kProbeSlots];
  float mx[kProbeSlots];
};
FovProbeState g_probe[kMaxTrackedMats]{};

void ProbeCamFov(Matrix44* mat) {
  FovProbeState* st = nullptr;
  for (auto& p : g_probe) {
    if (p.mat == mat) {
      st = &p;
      break;
    }
    if (!p.mat) {
      p.mat = mat;
      p.samples = 0;
      p.reports = 0;
      p.readable = !IsBadReadPtr(reinterpret_cast<uint8_t*>(mat) + kProbeStartOff,
                                 kProbeSlots * 4);
      for (int i = 0; i < kProbeSlots; ++i) {
        p.mn[i] = 1e9f;
        p.mx[i] = -1e9f;
      }
      st = &p;
      break;
    }
  }
  if (!st || !st->readable || st->reports >= 2)
    return;

  const float* base = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(mat) +
                                                     kProbeStartOff);
  for (int i = 0; i < kProbeSlots; ++i) {
    const float v = base[i];
    if (!std::isfinite(v))
      continue;
    if (v < st->mn[i])
      st->mn[i] = v;
    if (v > st->mx[i])
      st->mx[i] = v;
  }

  ++st->samples;
  // First report after 240 samples, second after 2400 (catches aim/vehicle changes).
  if (st->samples != 240 && st->samples != 2400)
    return;
  ++st->reports;
  Log("FovProbe: mat=%p report#%u (samples=%u) — candidates:", static_cast<void*>(mat),
      st->reports, st->samples);
  for (int i = 0; i < kProbeSlots; ++i) {
    const float mn = st->mn[i], mx = st->mx[i];
    if (mn > mx)
      continue;
    const int off = kProbeStartOff + i * 4;
    const bool deg = (mn >= 15.f && mx <= 120.f && (mx - mn) <= 8.f);
    const bool rad = (mn >= 0.30f && mx <= 2.10f && (mx - mn) <= 0.15f &&
                      std::fabs(mn - 1.f) > 0.002f);
    if (deg)
      Log("FovProbe:   off=%+d (0x%X) DEG %.3f..%.3f", off, off & 0xFFF, mn, mx);
    else if (rad)
      Log("FovProbe:   off=%+d (0x%X) RAD %.4f..%.4f", off, off & 0xFFF, mn, mx);
  }
}

// ---- Mode 17: targeted FOV write (offset verified via Mode 16) ---------------
// IDEMPOTENT: capture the game's baseline FOV once per cam object, then always
// write the absolute target = baseline × scale. Multiplying the live value
// compounded across call sites (45→58.5→76→…112) and made the image pulse/jitter
// (2026-07-24). Absolute writes pin a constant FOV every frame.
std::atomic<uint32_t> g_fovWriteCount{0};

struct FovBaseline {
  Matrix44* mat;
  float base;
};
FovBaseline g_fovBase[kMaxTrackedMats]{};

void ApplyFovPatch(Matrix44* mat) {
  int off = 0;
  float scale = 0.f;
  if (!GetFovPatchConfig(&off, &scale))
    return;
  float* f = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(mat) + off);
  const float v = *f;

  FovBaseline* bl = nullptr;
  for (auto& b : g_fovBase) {
    if (b.mat == mat) {
      bl = &b;
      break;
    }
    if (!b.mat) {
      // First sighting: only accept a plausible, UNPATCHED value as baseline.
      if (v >= 15.f && v <= 120.f) {
        b.mat = mat;
        b.base = v;
        Log("FovPatch: baseline mat=%p base=%.3f", static_cast<void*>(mat), v);
        bl = &b;
      }
      break;
    }
  }
  if (!bl)
    return;

  float target = 0.f;
  if (bl->base >= 15.f && bl->base <= 120.f)
    target = (std::min)(bl->base * scale, 130.f);  // degrees
  else if (bl->base >= 0.30f && bl->base <= 2.10f)
    target = (std::min)(bl->base * scale, 2.27f);  // radians
  else
    return;

  if (std::fabs(v - target) < 0.01f)
    return;  // already pinned this frame — no write, no log spam
  *f = target;
  const uint32_t n = ++g_fovWriteCount;
  if (n <= 3 || (n % 600) == 0)
    Log("FovPatch: #%u off=%d %.3f -> %.3f (base=%.3f)", n, off, v, target, bl->base);
}

void __fastcall HookCopyMat(Matrix44* mat, void* edx, void* arg2) {
  g_origCopyMat(mat, edx, arg2);
  if (mat && g_gameplayActive.load()) {
    const StereoMode sm = GetStereoMode();
    if (sm == StereoMode::CamFovProbe)
      ProbeCamFov(mat);
    else if (sm == StereoMode::CamFovWrite)
      ApplyFovPatch(mat);
    g_liveCamMat = mat;
    TrackCamMat(mat);
    ApplyHmdToCam(mat);
  }
}

bool HookOneCall(const char* name, const char* pattern) {
  const uintptr_t site = FindPattern(nullptr, pattern);
  if (!site) {
    Log("CamMatrix: pattern MISS %s", name);
    return false;
  }
  const uintptr_t orig = HookCallSite(site, reinterpret_cast<void*>(&HookCopyMat));
  if (!orig) {
    Log("CamMatrix: hook FAIL %s site=%p", name, reinterpret_cast<void*>(site));
    return false;
  }
  if (!g_origCopyMat)
    g_origCopyMat = reinterpret_cast<CopyMat_t>(orig);
  Log("CamMatrix: hooked %s call=%p orig=%p", name, reinterpret_cast<void*>(site),
      reinterpret_cast<void*>(orig));
  return true;
}

}  // namespace

bool InstallCamMatrixHooks() {
  if (g_hooksOk.load())
    return true;

  const uintptr_t findPed = FindPattern(nullptr, "8B 44 24 04 85 C0 75 18 A1");
  if (!findPed) {
    Log("CamMatrix: FindPlayerPed pattern MISS — cannot hard-lock FP");
    return false;
  }
  g_FindPlayerPed = reinterpret_cast<FindPlayerPed_t>(findPed);
  Log("CamMatrix: FindPlayerPed @ %p", reinterpret_cast<void*>(findPed));

  int ok = 0;
  ok += HookOneCall("onfoot_front", "E8 ? ? ? ? 8A 86 ? ? ? ? 80 A6 ? ? ? ? ? 80 A6") ? 1 : 0;
  ok += HookOneCall("onfoot_behind", "E8 ? ? ? ? 8B 8E ? ? ? ? 56 8D 44 24 74") ? 1 : 0;
  ok += HookOneCall("vehicle_front", "E8 ? ? ? ? 80 A7 ? ? ? ? ? 80 A7 ? ? ? ? ? 80 7C 24") ? 1 : 0;
  ok += HookOneCall("vehicle_behind", "E8 ? ? ? ? 5F B0 01 5E 8B E5 5D C2 14 00") ? 1 : 0;

  if (ok == 0) {
    Log("CamMatrix: NO CopyMat hooks — abort");
    g_FindPlayerPed = nullptr;
    return false;
  }

  g_hooksOk = true;
  Log("CamMatrix: %d/4 hooked — HARD FP on ped (only F9 recenter)", ok);
  return true;
}

void SetCamMatrixGameplayActive(bool active) {
  const bool prev = g_gameplayActive.exchange(active);
  if (active && !prev) {
    g_havePosBaseline = false;
    g_haveLastEye = false;
    Log("CamMatrix: gameplay ACTIVE — FP lock armed");
  } else if (!active && prev) {
    Log("CamMatrix: gameplay inactive");
  }
}

bool AreCamMatrixHooksInstalled() {
  return g_hooksOk.load();
}

bool IsCamMatrixOverrideEnabled() {
  return g_hooksOk.load() && g_gameplayActive.load();
}

void CamMatrixOnRecenter() {
  g_havePosBaseline = false;
  g_haveVehYawBase = false;
  HmdPoseOnRecenter();
  Log("CamMatrix: 6DoF baseline reset (F9)");
}

void PollCamHotkeys() {
  // Intentionally empty — only F9 recenter (via hmd_look) remains.
}

void RefreshLiveCamForStereoEye() {
  if (!g_gameplayActive.load())
    return;
  if (g_trackedCount > 0) {
    for (int i = 0; i < g_trackedCount; ++i) {
      if (g_trackedMats[i])
        ApplyHmdToCam(g_trackedMats[i]);
    }
    return;
  }
  if (g_liveCamMat)
    ApplyHmdToCam(g_liveCamMat);
}

// Mode 22: world-space delta from LEFT eye to RIGHT eye = hmdRight * sep * scale
// (the -eyeZ forward term in ApplyHmdToCam is identical for both eyes).
bool GetStereoEyeRightDeltaWorld(float* dx, float* dy, float* dz) {
  if (!dx || !dy || !dz)
    return false;
  vr::HmdMatrix34_t h{};
  if (!GetHmdPoseMatrix(&h))
    return false;
  float hrx, hry, hrz;
  OvrToGta(h.m[0][0], h.m[1][0], h.m[2][0], &hrx, &hry, &hrz);
  const float len = std::sqrt(hrx * hrx + hry * hry + hrz * hrz);
  if (len < 1e-4f)
    return false;
  // Stereo IPD × soft StereoScale (not WorldScale) — see ApplyHmdToCam.
  const float d = GetStereoSepMeters() * GetStereoScale() / len;
  *dx = hrx * d;
  *dy = hry * d;
  *dz = hrz * d;
  return true;
}

bool GetLastStereoCamPos(float* x, float* y, float* z) {
  if (!x || !y || !z || !g_haveLastApplied)
    return false;
  *x = g_lastAppliedPos.x;
  *y = g_lastAppliedPos.y;
  *z = g_lastAppliedPos.z;
  return true;
}

bool BuildLiveViewMatrix16(float* out16) {
  if (!out16 || !g_haveLastApplied || !g_liveCamMat)
    return false;

  const Matrix44* m = g_liveCamMat;
  const float rx = m->right.x, ry = m->right.y, rz = m->right.z;
  const float fx = m->up.x, fy = m->up.y, fz = m->up.z;
  const float ux = m->at.x, uy = m->at.y, uz = m->at.z;
  const float px = m->pos.x, py = m->pos.y, pz = m->pos.z;

  out16[0] = rx;
  out16[1] = ux;
  out16[2] = fx;
  out16[3] = 0.f;
  out16[4] = ry;
  out16[5] = uy;
  out16[6] = fy;
  out16[7] = 0.f;
  out16[8] = rz;
  out16[9] = uz;
  out16[10] = fz;
  out16[11] = 0.f;
  out16[12] = -(rx * px + ry * py + rz * pz);
  out16[13] = -(ux * px + uy * py + uz * pz);
  out16[14] = -(fx * px + fy * py + fz * pz);
  out16[15] = 1.f;
  return true;
}

void PushLiveCamToD3D(IDirect3DDevice9* device) {
  if (!device)
    return;
  float v16[16];
  if (!BuildLiveViewMatrix16(v16))
    return;
  device->SetTransform(D3DTS_VIEW, reinterpret_cast<D3DMATRIX*>(v16));
  if (!g_loggedD3dPush) {
    g_loggedD3dPush = true;
    Log("CamMatrix: D3DTS_VIEW push enabled (trackedMats=%d)", g_trackedCount);
  }
}

// ---- Mode 35/36: FusionFix FOV recompute site (CCam+0x60) ----------------------
// FusionFix hooks the same CALL and does: orig(); *(this+0x60) += n*5.
// Mode 17 wrote mat+0x50 after CopyMat and lost to this recompute. We chain
// AFTER the current target (often FusionFix stub with FOV=0) so our ADD sticks.
// Mode 36: also publish post-ADD FOV to canvas (Rage ignores D3DTS_PROJECTION).
void RefreshLiveCamForStereoEye();

using CamFovSite_t = void(__fastcall*)(void* self, void* edx);
CamFovSite_t g_origCamFovSite = nullptr;
std::atomic<bool> g_fovSiteOk{false};
std::atomic<uint32_t> g_fovSiteCalls{0};

// Mode74: CCam degrees so engine FOV covers HMD (RealVR UniversalFOVFix class).
// Mode16 probe: rendered vertical ≈ ccam * (58.7/45). Match BOTH cover V and the
// V implied by cover H at backbuffer aspect (giant-screen killer). Returns 0 if unknown.
float Mode74HmdTargetCcamDegrees() {
  float coverH = 0.f, coverV = 0.f;
  if (!GetCoverFovTangents(&coverH, &coverV) || !(coverV > 0.05f))
    return 0.f;
  constexpr float kEng = 58.7f / 45.f;
  constexpr float kRad2Deg = 180.f / 3.14159265f;
  float aspect = GetBackbufferAspect();
  if (!(aspect > 0.5f) || !(aspect < 3.f))
    aspect = 16.f / 9.f;
  // From vertical cover tan.
  const float targetFromV =
      (2.f * std::atan(coverV) * kRad2Deg) / kEng;
  // From horizontal cover: need tanV >= coverH/aspect so game H fills HMD H.
  float needTanV = coverH / aspect;
  if (needTanV < 0.05f)
    needTanV = 0.05f;
  const float targetFromH =
      (2.f * std::atan(needTanV) * kRad2Deg) / kEng;

  const StereoMode sm = GetStereoMode();
  const CanvasAspectPolicy policy = GetCanvasAspectPolicy(sm);
  float target = 0.f;
  if (IsAspectFillSweet(sm)) {
    // Mode83: aim ~90% of cover-V (less bars than letterbox 62%v; may crop sides).
    // Still aspect-preserving at publish (tanH = tanV × bbAspect).
    // Headset: fill≈149%h → monitor — prefer Mode84 letterbox.
    constexpr float kFillV = 0.90f;
    float needTanV = coverV * kFillV;
    if (needTanV < 0.05f)
      needTanV = 0.05f;
    target = (2.f * std::atan(needTanV) * kRad2Deg) / kEng;
  } else if (IsCullSyncPresence(sm)) {
    // Mode89: mild soft letterbox ~68%v — fewer bars than Mode80; not Mode84 groß.
    constexpr float kFillV = 0.68f;
    float needTanV = coverV * kFillV;
    if (needTanV < 0.05f)
      needTanV = 0.05f;
    target = (2.f * std::atan(needTanV) * kRad2Deg) / kEng;
    if (targetFromH > target)
      target = targetFromH;
  } else if (IsLetterboxWideFov(sm)) {
    // Mode84: aim ~78% cover-V (soft letterbox). Aspect locked at publish;
    // fillH hard-capped ~118% — fewer bars than Mode80 62%v, not Mode83 149%h.
    // Headset: no bars but groß — prefer Mode85 hard letterbox.
    constexpr float kFillV = 0.78f;
    float needTanV = coverV * kFillV;
    if (needTanV < 0.05f)
      needTanV = 0.05f;
    target = (2.f * std::atan(needTanV) * kRad2Deg) / kEng;
    // Also ensure cover-H is reachable under the fillH cap (real FOV climb).
    if (targetFromH > target)
      target = targetFromH;
  } else if (policy == CanvasAspectPolicy::FillPrefer) {
    // Mode82: aim cover-V (taller) so HMD fills vertically; H may exceed cover → crop.
    target = (targetFromV > targetFromH) ? targetFromV : targetFromH;
  } else if (IsSteamVrRtLetterbox(sm) || IsSteamVrRtSafe(sm) || IsAspectLetterboxSafe(sm) ||
             IsHitchCutEyeProj(sm)) {
    // Mode88/86/85/81: cover-H only — Mode80-honest vertical letterbox; never chase cover-V.
    target = targetFromH;
  } else {
    // Mode80 / Mode74 family / Mode89: prefer cover-H (letterbox OK) but allow
    // mild max with V so presence climbs. Still aspect-safe at publish.
    target = (targetFromV > targetFromH) ? targetFromV : targetFromH;
  }

  // Aggressive presence (77/79/80/82/87/89): push toward Reverb G2 ~90-100° feel.
  // Mode84 letterbox-wide: cap ~94 (cover-H climb). Mode83 fill: cap ~92.
  // Mode85/81 letterbox: softer cap 88. Mode88/86 + baseline Mode74: cap 85.
  float cap = 85.f;
  if (IsLetterboxWideFov(sm))
    cap = 94.f;
  else if (IsCullSyncPresence(sm))
    cap = 90.f;  // Mode89 mild soft — under Mode84 94 / Mode83 92
  else if (IsAspectFillSweet(sm))
    cap = 92.f;
  else if (WantsAggressiveFovPresence(sm))
    cap = 98.f;
  else if (IsSteamVrRtSafe(sm) || IsHitchCutEyeProj(sm))
    cap = 85.f;  // Mode80-honest; Mode85 cap 88 felt still groß
  else if (IsSteamVrRtLetterbox(sm) || IsAspectLetterboxSafe(sm))
    cap = 88.f;
  if (target < 50.f)
    target = 50.f;
  if (target > cap)
    target = cap;
  // Presence modes: if cover asks for more than formula, still climb toward cap.
  // Mode83/84: no +8 hammer (perf + honest letterbox/fill).
  if (WantsAggressiveFovPresence(sm) && !IsAspectFillSweet(sm) && target < cap - 0.5f) {
    // Bias +8° toward HMD presence (cinema killer) without exceeding cap.
    target += 8.f;
    if (target > cap)
      target = cap;
  }
  return target;
}

// Cache HMD CCam target — GetCoverFovTangents is not free; FovSite is hot.
// Street streaming: 2s cache (was 500ms) — FOV target barely changes mid-walk.
float Mode74HmdTargetCcamDegreesCached() {
  static float s_cached = 0.f;
  static DWORD s_tick = 0;
  const DWORD now = GetTickCount();
  if (s_cached > 5.f && (now - s_tick) < 2000u)
    return s_cached;
  s_cached = Mode74HmdTargetCcamDegrees();
  s_tick = now;
  return s_cached;
}

void __fastcall HookCamFovSite(void* self, void* edx) {
  if (g_origCamFovSite)
    g_origCamFovSite(self, edx);
  if (!self)
    return;
  __try {
    float* fov = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x60);
    const float before = *fov;
    if (!std::isfinite(before) || before < 5.f || before > 160.f)
      return;
    const StereoMode sm = GetStereoMode();
    const float requestedAdd = GetFovAddDegrees();
    // Mode 39 is intentionally a comfort A/B, not another reprojection attempt:
    // Mode 38's pose-aware Submit was accepted by SteamVR but did not reduce the
    // jump. Keep the same pair-held fusion path while reducing the true engine
    // FOV expansion that correlated with worse FPS/jump in Modes 36/37.
    const bool lowMotion = sm == StereoMode::FovCanvasLowMotion;
    const bool mode74 = IsMode74Family(sm);
    float add = lowMotion ? (std::min)(requestedAdd, 12.f) : requestedAdd;
    if (lowMotion && requestedAdd > add) {
      static bool s_loggedLowMotionCap = false;
      if (!s_loggedLowMotionCap) {
        s_loggedLowMotionCap = true;
        Log("Mode39: fovadd requested=%.0f capped=%.0f deg (lower temporal/FOV stress)",
            requestedAdd, add);
      }
    }
    float after = before;
    // Idempotent ADD: the cam CALL usually resets CCam+0x60 to the base FOV, then
    // we ADD. Sometimes the CALL leaves our previous write in place — adding again
    // compounds (67→89) and blows canvas fill past 100% (headset 2026-07-24).
    // Skip when before ≈ lastWritten (already has our ADD).
    // Also: only ADD in the normal gameplay FOV band. Cutscene/menu bases (~70+)
    // must not get +fovadd (load spike 76→98 → 193% fill / heavy crop).
    static float s_lastWritten = -1.f;
    constexpr float kGameplayFovHi = 55.f;

    // Mode74: drive CCam FOV toward HMD cover every frame (absolute, idempotent).
    // Prior fovadd-only left fill~83%v (cinema). Kill/45 drops this branch.
    if (mode74) {
      const float hmdTarget = Mode74HmdTargetCcamDegreesCached();
      float target = before;
      if (hmdTarget > 5.f) {
        // Prefer HMD target; still honor fovadd if file asks for wider.
        target = hmdTarget;
        if (requestedAdd > 0.05f && before <= kGameplayFovHi) {
          const float addTarget = before + requestedAdd;
          if (addTarget > target)
            target = addTarget;
        }
      } else if (requestedAdd > 0.05f && before <= kGameplayFovHi) {
        target = before + requestedAdd;
      }
      if (target > 85.f)
        target = 85.f;
      // Aggressive presence (77/79/80/82): allow up to 98° CCam cover.
      if (IsCullSyncPresence(sm) && hmdTarget > 5.f) {
        // Mode89: mild soft ~68%v, cap 90 — fillH≤108% at publish.
        target = hmdTarget;
        if (target > 90.f)
          target = 90.f;
      } else if (WantsAggressiveFovPresence(sm) && hmdTarget > 5.f) {
        target = hmdTarget;
        if (target > 98.f)
          target = 98.f;
      } else if (IsLetterboxWideFov(sm) && hmdTarget > 5.f) {
        // Mode84: soft letterbox ~78%v target, cap 94 — fillH capped at publish.
        target = hmdTarget;
        if (target > 94.f)
          target = 94.f;
      } else if (IsAspectFillSweet(sm) && hmdTarget > 5.f) {
        // Mode83: ~90%v target from Mode74HmdTargetCcamDegrees, cap 92.
        target = hmdTarget;
        if (target > 92.f)
          target = 92.f;
      } else if ((IsSteamVrRtSafe(sm) || IsSteamVrRtLetterbox(sm) ||
                  IsAspectLetterboxSafe(sm) || IsHitchCutEyeProj(sm)) &&
                 hmdTarget > 5.f) {
        // Mode88/86: cover-H, cap 85. Mode85/81: cap 88.
        target = hmdTarget;
        const float softCap =
            (IsSteamVrRtSafe(sm) || IsHitchCutEyeProj(sm)) ? 85.f : 88.f;
        if (target > softCap)
          target = softCap;
      }
      // Gameplay / our prior write band only — skip cutscene spikes (~90+).
      float bandHi = 85.f;
      if (WantsAggressiveFovPresence(sm))
        bandHi = 98.f;
      else if (IsLetterboxWideFov(sm))
        bandHi = 94.f;
      else if (IsCullSyncPresence(sm))
        bandHi = 90.f;
      else if (IsAspectFillSweet(sm))
        bandHi = 92.f;
      else if (IsSteamVrRtSafe(sm) || IsHitchCutEyeProj(sm))
        bandHi = 85.f;
      else if (IsSteamVrRtLetterbox(sm) || IsAspectLetterboxSafe(sm))
        bandHi = 88.f;
      const bool inBand = before <= bandHi;
      if (inBand && target > before + 0.4f) {
        // Engine reset to base — write target.
        if (s_lastWritten > 0.f && std::fabs(before - s_lastWritten) < 0.4f) {
          after = before;  // already holding our write
        } else {
          *fov = target;
          after = target;
          s_lastWritten = target;
          add = target - before;
          // Also stamp CopyMat mat+0x50 (Mode16: same FOV field as CCam+0x60).
          if (g_liveCamMat) {
            __try {
              float* matFov =
                  reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_liveCamMat) + 0x50);
              if (*matFov > 5.f && *matFov < 160.f)
                *matFov = target;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
          }
        }
      } else if (inBand && s_lastWritten > 0.f &&
                 std::fabs(before - s_lastWritten) < 0.4f) {
        after = before;
        add = 0.f;
      } else if (!inBand) {
        s_lastWritten = -1.f;
        after = before;
        add = 0.f;
      } else {
        s_lastWritten = -1.f;
        after = before;
        add = 0.f;
      }
      const uint32_t n = ++g_fovSiteCalls;
      if (n <= 4 || (n % 2400) == 0)
        Log("FovSite: Mode74 HMD #%u CCam+0x60 %.3f -> %.3f (hmdTarget=%.1f add=%.1f) "
            "self=%p proven=%d (full cover FOV; canvas gated until drawn proof; kill=45)",
            n, before, after, hmdTarget, add, self, IsDrawnFovProven() ? 1 : 0);
      // Publish canvas FOV only when we actually changed CCam — still gated inside
      // PublishGameFovFromCCamDegrees until drawn FOV is proven.
      if (after > before + 0.2f)
        PublishGameFovFromCCamDegrees(after, GetBackbufferAspect());
      // Mode74 DRAW-PATH owns HMD look — skip late CamMatrix refresh (street hitch).
      return;
    }

    if (add > 0.05f && before <= kGameplayFovHi) {
      if (s_lastWritten > 0.f && std::fabs(before - s_lastWritten) < 0.4f) {
        after = before;
      } else {
        after = before + add;
        if (after > 130.f)
          after = 130.f;
        *fov = after;
        s_lastWritten = after;
      }
    } else if (before > kGameplayFovHi) {
      // Special cam — leave engine FOV alone; clear sticky lastWritten.
      s_lastWritten = -1.f;
      after = before;
    } else {
      s_lastWritten = -1.f;
    }
    // Mode 36/37: canvas must track TRUE engine FOV (not stale GetTransform).
    // Mode 35 baseline left unchanged (protect headset-good warp).
    if (sm == StereoMode::FovRecomputeTrueCanvas || sm == StereoMode::FovCanvasComfort ||
        sm == StereoMode::AerPoseSubmit || sm == StereoMode::FovCanvasLowMotion ||
        sm == StereoMode::FovCanvasMotionGuard || sm == StereoMode::ReplayCallChainProbe ||
        sm == StereoMode::ReplayOwnerCountProbe || sm == StereoMode::FovCanvasMotionGuardFast ||
        sm == StereoMode::FovCanvasMotionGuardRtLock || IsHeadOwnedCamFamily(sm))
      PublishGameFovFromCCamDegrees(after, GetBackbufferAspect());

    // Mode 45: Rage can revise a camera after CopyMat but before this already-safe
    // post-process FOV site. Re-apply the exact same ped-eye + relative HMD pose
    // here, late in the game-thread camera path. This does not move gameplay's
    // collision camera, add a VS offset, or make another render/replay pass.
    // Mode74 family: hitchcut cut late CamMatrix refresh (street hitch). DRAW-PATH owns look.
    if (IsHeadOwnedCamFamily(sm) && !IsMode74Family(sm)) {
      RefreshLiveCamForStereoEye();
      static uint32_t s_headOwnedRefreshes = 0;
      const uint32_t refreshes = ++s_headOwnedRefreshes;
      if (refreshes <= 4 || (refreshes % 600) == 0) {
        Log("Mode45: late head-owned CopyMat refresh #%u (CCam site; collision unchanged)",
            refreshes);
      }
    }
    const uint32_t n = ++g_fovSiteCalls;
    if (n <= 4 || (n % 600) == 0)
      Log("FovSite: #%u CCam+0x60 %.3f -> %.3f (add=%.0f) self=%p trueCanvas=%d", n, before,
          after, add, self,
          (sm == StereoMode::FovRecomputeTrueCanvas || sm == StereoMode::FovCanvasComfort ||
           sm == StereoMode::AerPoseSubmit || sm == StereoMode::FovCanvasLowMotion ||
           sm == StereoMode::FovCanvasMotionGuard || sm == StereoMode::ReplayCallChainProbe ||
           sm == StereoMode::ReplayOwnerCountProbe || sm == StereoMode::FovCanvasMotionGuardFast ||
           sm == StereoMode::FovCanvasMotionGuardRtLock || IsHeadOwnedCamFamily(sm))
              ? 1
              : 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    static bool once = false;
    if (!once) {
      once = true;
      Log("FovSite: EXCEPTION reading/writing CCam+0x60 — disabled for process");
    }
    g_fovSiteOk.store(false);
  }
}

bool InstallFovRecomputeSiteHook() {
  if (g_fovSiteOk.load())
    return true;
  // Same AOBs as FusionFix fixes.ixx "Custom FOV" (CE + classic patterns).
  static const char* kPatterns[] = {
      "E8 ? ? ? ? F6 87 ? ? ? ? ? 5B",
      "E8 ? ? ? ? 8B CE E8 ? ? ? ? F6 86 ? ? ? ? ? 5F",
  };
  uintptr_t site = 0;
  const char* hitPat = nullptr;
  for (const char* pat : kPatterns) {
    site = FindPattern(nullptr, pat);
    if (site) {
      hitPat = pat;
      break;
    }
  }
  if (!site) {
    Log("FovSite: AOB MISS (FusionFix Custom FOV CALL) — Mode 35 FOV write unavailable");
    return false;
  }
  const uintptr_t orig = HookCallSite(site, reinterpret_cast<void*>(&HookCamFovSite));
  if (!orig) {
    Log("FovSite: hook FAIL site=%p", reinterpret_cast<void*>(site));
    return false;
  }
  g_origCamFovSite = reinterpret_cast<CamFovSite_t>(orig);
  g_fovSiteOk.store(true);
  HMODULE exe = GetModuleHandleA(nullptr);
  const uintptr_t rva = site - reinterpret_cast<uintptr_t>(exe);
  Log("FovSite: hooked CALL @ %p exeRva=0x%X (FusionFix FOV recompute; chain→%p; "
      "add via gtaiv_dxvk_vr.fovadd)",
      reinterpret_cast<void*>(site), static_cast<unsigned>(rva),
      reinterpret_cast<void*>(orig));
  (void)hitPat;
  // Force config log once.
  (void)GetFovAddDegrees();
  return true;
}

}  // namespace asi

