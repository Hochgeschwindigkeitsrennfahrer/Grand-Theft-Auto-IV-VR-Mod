#include "cam_matrix.h"
#include "aob.h"
#include "hmd_pose.h"
#include "log.h"
#include "ped_hide.h"
#include "stereo_config.h"
#include "stereo_eye.h"
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
  if (!GetHmdPoseMatrix(&h))
    return;

  Vec4 eye{};
  if (!TryGetPedEyePos(&eye) || !SaneWorldPos(eye)) {
    // No real ped yet — never reuse stale eye (menu → freeze risk)
    return;
  }
  g_lastEye = eye;
  g_haveLastEye = true;

  // Seated 6DoF on top of ped eye (F9 resets baseline)
  float px, py, pz;
  OvrToGta(h.m[0][3], h.m[1][3], h.m[2][3], &px, &py, &pz);
  if (!g_havePosBaseline) {
    g_basePx = px;
    g_basePy = py;
    g_basePz = pz;
    g_havePosBaseline = true;
  }
  const float worldScale = GetWorldScale();
  eye.x += (px - g_basePx) * kPosScale * worldScale;
  eye.y += (py - g_basePy) * kPosScale * worldScale;
  eye.z += (pz - g_basePz) * kPosScale * worldScale;

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
  float ipdX = 0.f, ipdY = 0.f, ipdZ = 0.f;
  const bool rightEye = (GetStereoEye() == StereoEye::Right);
  if (GetStereoMode() >= StereoMode::DualIpd) {
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

    // Optional EyeToHead forward (OpenVR col2 translation) like L4D2 m_EyeZ.
    float eyeZ = 0.f;
    if (vr::VRSystem()) {
      const vr::HmdMatrix34_t e2h =
          vr::VRSystem()->GetEyeToHeadTransform(rightEye ? vr::Eye_Right : vr::Eye_Left);
      eyeZ = e2h.m[2][3];
    }

    // WorldScale (F7) = 6DoF only. StereoScale (F6, default 1.15, cap 1.30) =
    // soft disparity for size-without-fusion-break. Raw WorldScale×IPD at 1.5
    // made ~9 cm sep → fusion gone + violent jump (headset 2026-07-24).
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
    Log("CamMatrix: FP lock #%u %s pos=(%.3f,%.3f,%.3f) ipd=(%.4f,%.4f,%.4f) sep=%.0fcm "
        "eyeFwd=%.0fcm",
        n, rightEye ? "R" : "L", eye.x, eye.y, eye.z, ipdX, ipdY, ipdZ,
        GetStereoSepMeters() * 100.f, eyeFwd * 100.f);
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

}  // namespace asi

