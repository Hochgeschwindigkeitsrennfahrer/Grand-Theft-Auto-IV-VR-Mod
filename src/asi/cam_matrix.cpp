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
#include <cstring>

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
using FindPlayerVehicle_t = void*(__cdecl*)(int32_t id);

CopyMat_t g_origCopyMat = nullptr;
FindPlayerPed_t g_FindPlayerPed = nullptr;
FindPlayerVehicle_t g_FindPlayerVehicle = nullptr;

std::atomic<bool> g_hooksOk{false};
std::atomic<bool> g_gameplayActive{false};  // false until past title (avoids blackscreen)
std::atomic<uint32_t> g_applyCount{0};

// Mode 171: freeze ped eye ORIGIN for one DrawScene L→R pair (driving flicker).
std::atomic<bool> g_dualCamFreeze{false};
Vec4 g_frozenPedEye{};
bool g_haveFrozenPedEye = false;
bool g_freezeFullBasis = false;  // Mode 178+: also freeze pre-IPD basis+center
bool g_haveFrozenCenter = false;
Vec4 g_frozenCenterEye{};
float g_frozenFx = 0.f, g_frozenFy = 0.f, g_frozenFz = 0.f;
float g_frozenRx = 0.f, g_frozenRy = 0.f, g_frozenRz = 0.f;
float g_frozenUx = 0.f, g_frozenUy = 0.f, g_frozenUz = 0.f;

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
SRWLOCK g_lastAppliedLock = SRWLOCK_INIT;

struct CamBuildReceiptScope {
  uint32_t applyGeneration = 0u;
  uint32_t applyCount = 0u;
  DWORD writerThreadId = 0u;
  bool expectedRightEye = false;
  bool lastRightEye = false;
  bool eyeMismatch = false;
  bool eyeOffsetApplied = false;
  bool eyeOffsetMismatch = false;
  bool firstPersonAnchor = false;
  Vec4 lastPosition{};
  Vec4 lastPreEyePosition{};
  Vec4 appliedEyeOffset{};
  Vec4 selectedLocalEyeOffset{};
  float lastBasis[9]{};
  bool active = false;
};
thread_local CamBuildReceiptScope g_camBuildReceiptScope{};

// Mode 140: snapshot FirstPerson cam from CopyMat; IPD offset applied from this base
// so L/R dual passes do not stack offsets.
Matrix44 g_fpHostBase{};
bool g_haveFpHostBase = false;
std::atomic<uint32_t> g_fpHostIpdLog{0};

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

// Mode 140: keep FirstPerson orientation/pos; add ±half IPD along cam right (+ EyeZ).
void ApplyFpHostStereoIpd(Matrix44* mat) {
  if (!mat || !g_haveFpHostBase)
    return;
  *mat = g_fpHostBase;

  float rx = mat->right.x, ry = mat->right.y, rz = mat->right.z;
  Normalize(&rx, &ry, &rz);
  float fx = mat->up.x, fy = mat->up.y, fz = mat->up.z;  // RAGE forward
  Normalize(&fx, &fy, &fz);

  const bool rightEye = (GetStereoEye() == StereoEye::Right);
  float eyeZ = 0.f;
  if (vr::VRSystem()) {
    const vr::HmdMatrix34_t e2h =
        vr::VRSystem()->GetEyeToHeadTransform(rightEye ? vr::Eye_Right : vr::Eye_Left);
    eyeZ = e2h.m[2][3];
  }
  const float half = 0.5f * GetStereoSepMeters() * GetStereoScale();
  const float s = rightEye ? half : -half;
  const float ipdX = rx * s + fx * (-eyeZ);
  const float ipdY = ry * s + fy * (-eyeZ);
  const float ipdZ = rz * s + fz * (-eyeZ);
  if (std::isfinite(ipdX) && std::isfinite(ipdY) && std::isfinite(ipdZ) &&
      std::fabs(ipdX) < 20.f && std::fabs(ipdY) < 20.f && std::fabs(ipdZ) < 20.f) {
    mat->pos.x += ipdX;
    mat->pos.y += ipdY;
    mat->pos.z += ipdZ;
  }
  AcquireSRWLockExclusive(&g_lastAppliedLock);
  g_lastAppliedPos = mat->pos;
  g_haveLastApplied = true;
  ReleaseSRWLockExclusive(&g_lastAppliedLock);

  const uint32_t n = ++g_fpHostIpdLog;
  if (n <= 5 || (n % 300) == 0)
    Log("Mode140: IPD-only #%u %s pos=(%.3f,%.3f,%.3f) sep=%.0fcm (FP base kept)", n,
        rightEye ? "R" : "L", mat->pos.x, mat->pos.y, mat->pos.z,
        GetStereoSepMeters() * 100.f);
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

bool TryGetPedHorizontalHeading(float* outHeading) {
  if (!g_FindPlayerPed || !outHeading)
    return false;
  void* ped = g_FindPlayerPed(0);
  if (!ped)
    return false;
  auto* pMat = *reinterpret_cast<Matrix44**>(reinterpret_cast<uint8_t*>(ped) + 0x20);
  if (!pMat)
    return false;
  const float fx = pMat->up.x;
  const float fy = pMat->up.y;
  if (fx * fx + fy * fy < 1e-6f)
    return false;
  *outHeading = std::atan2(fx, fy);
  return true;
}

// Vehicle heading-follow (gtaiv_dxvk_vr.vehfollow): camera yaw tracks the vehicle's
// yaw delta since entry/recenter, HMD free look stays on top. Baseline resets on
// exit and on F9 (not F10).
float g_vehYawBase = 0.f;
bool g_haveVehYawBase = false;

// Mode 49: ped-yaw coupling — body turns carry the view; HMD yaw is delta on top.
float g_pedHeadingBase = 0.f;
bool g_havePedHeadingBase = false;

float WrapPi(float a) {
  while (a > 3.14159265f)
    a -= 6.2831853f;
  while (a < -3.14159265f)
    a += 6.2831853f;
  return a;
}

// True while occupying a vehicle for VR cam offsets.
// Prefer FindPlayerVehicle (null on foot) — CPed::m_pVehicle often LINGERS after
// exit, which left vehcamoff stuck on. Fall back to m_pVehicle only if the pattern
// miss (rare). No proximity heuristic (near-car without occupying).
bool IsPlayerSeatedForVrCam() {
  static int s_okFrames = 0;
  static bool s_loggedExit = false;
  static bool s_loggedEnter = false;
  static bool s_loggedFallback = false;

  bool inside = false;
  const char* how = "none";
  if (g_FindPlayerVehicle) {
    inside = g_FindPlayerVehicle(0) != nullptr;
    how = "FindPlayerVehicle";
  } else if (g_FindPlayerPed) {
    if (!s_loggedFallback) {
      s_loggedFallback = true;
      Log("VehCam: FindPlayerVehicle MISS — fallback m_pVehicle (exit may stick)");
    }
    void* ped = g_FindPlayerPed(0);
    if (ped) {
      inside =
          *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ped) + kPedVehicleOff) !=
          nullptr;
      how = "m_pVehicle";
    }
  }

  if (!inside) {
    if (s_okFrames > 0 && !s_loggedExit) {
      s_loggedExit = true;
      s_loggedEnter = false;
      Log("VehCam: on foot — vehcamoff OFF (%s)", how);
    }
    s_okFrames = 0;
    return false;
  }
  if (s_okFrames < 100)
    ++s_okFrames;
  if (s_okFrames >= 2) {
    if (!s_loggedEnter) {
      s_loggedEnter = true;
      s_loggedExit = false;
      Log("VehCam: INSIDE vehicle — vehcamoff/kPedFwd car ON (%s)", how);
    }
    return true;
  }
  return false;
}

// Legacy name used by ApplyHmdToCam.
bool IsPlayerInVehicleSticky() {
  return IsPlayerSeatedForVrCam();
}

// Raw m_pVehicle (no debounce) — enter/jack edge detection.
bool IsPlayerVehiclePtrSet() {
  if (!g_FindPlayerPed)
    return false;
  void* ped = g_FindPlayerPed(0);
  if (!ped)
    return false;
  return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ped) + kPedVehicleOff) !=
         nullptr;
}

// Mode 165: enter/jack window (force FP onto live CCam — script cams skip CopyMat).
DWORD g_enterFpUntil = 0;

bool IsEnterCarFpWindowActive() {
  return g_enterFpUntil != 0 && GetTickCount() < g_enterFpUntil;
}

void UpdateEnterCarFpWindow() {
  if (!IsOursFpEnterCarFp(GetStereoMode()) || !g_FindPlayerPed)
    return;
  static bool s_hadVeh = false;
  const bool vehPtr = IsPlayerVehiclePtrSet();
  const DWORD now = GetTickCount();
  if (vehPtr && !s_hadVeh) {
    g_enterFpUntil = now + 5000;  // jack + pull-out + sit
    Log("EnterFp: vehicle ptr set — force CCam FP for 5000ms (enter/jack)");
  }
  if (!vehPtr) {
    if (s_hadVeh)
      Log("EnterFp: on foot — foot cam offsets restored (vehcamoff off)");
    g_enterFpUntil = 0;
  }
  s_hadVeh = vehPtr;
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

// Rage bone tag HEAD.
constexpr int kBoneHead = 0x4B5;

// CE ~rva 0x5E7320: void __thiscall (Vec3* out, unsigned boneId) — writes ~16 bytes.
// Mode193 FAILED first try: AOB matched 0x5E70B0 (bone *matrix* copy past float[3] →
// stack smash / crash after load). Validation looked like "fallback" but stack was toast.
using PedGetBonePos_t = void(__thiscall*)(void* ped, float* outXyz, uint32_t boneId);

PedGetBonePos_t g_pedGetBonePos = nullptr;
bool g_boneThiscallResolved = false;
bool g_boneThiscallDead = false;
bool g_boneThiscallLoggedOk = false;
bool g_boneThiscallLoggedFail = false;
uint32_t g_boneThiscallOk = 0;

bool ResolvePedGetBonePos() {
  if (g_boneThiscallDead)
    return false;
  if (g_boneThiscallResolved)
    return g_pedGetBonePos != nullptr;
  g_boneThiscallResolved = true;

  // CE ~rva 0x5E7320: writes Vec3 into caller buffer (ret 8).
  // First jz is 74 16 on this CE; 74 4A is later in-body (unique vs sibling 0x5E72C0).
  uintptr_t p = FindPattern(
      nullptr,
      "56 8B F1 8B 06 FF 90 A0 00 00 00 85 C0 74 16 8B 06 8B CE FF 90 A0 00 00 00 "
      "8B 10 8B C8 FF 92 E0 00 00 00 EB 06 8B 86 00 01 00 00 85 C0 74 4A FF 74 24 0C");
  const char* how = "AOB";
  if (!p) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    if (base) {
      auto* cand = base + 0x5E7320;
      if (cand[0] == 0x56 && cand[1] == 0x8B && cand[2] == 0xF1 && cand[0x0E] == 0x74) {
        p = reinterpret_cast<uintptr_t>(cand);
        how = "RVA+0x5E7320";
      }
    }
  }
  if (!p) {
    g_boneThiscallDead = true;
    Log("CamMatrix: PedGetBonePos(Vec3) AOB+RVA MISS — Mode193 falls back to root+height");
    return false;
  }
  g_pedGetBonePos = reinterpret_cast<PedGetBonePos_t>(p);
  Log("CamMatrix: PedGetBonePos Vec3 thiscall @ %p via %s (not matrix; Mode193)",
      reinterpret_cast<void*>(p), how);
  return true;
}

// Eye pivot from ped HEAD bone via CE Vec3 thiscall.
// Dual L/R must share one sample — Mode193 enables dual cam freeze (pairFreeze).
Vec4 g_stickyBoneEye{};
Vec4 g_stickyBoneRoot{};
bool g_haveStickyBoneEye = false;

bool TryGetPedHeadBoneEye(Vec4* outEye) {
  if (!outEye || !g_FindPlayerPed || g_boneThiscallDead)
    return false;
  if (!ResolvePedGetBonePos() || !g_pedGetBonePos)
    return false;

  void* ped = g_FindPlayerPed(0);
  if (!ped)
    return false;

  Vec4 root{};
  auto* pMat = *reinterpret_cast<Matrix44**>(reinterpret_cast<uint8_t*>(ped) + 0x20);
  if (pMat) {
    root = pMat->pos;
  } else {
    const float* t = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(ped) + 0x10);
    root.x = t[0];
    root.y = t[1];
    root.z = t[2];
  }
  if (!SaneWorldPos(root))
    return false;

  // After a door AV we skip DrawScene — also avoid GetBonePos (can finish the crash).
  if (StereoMode193SkipDrawActive()) {
    if (g_haveStickyBoneEye) {
      const float dx = root.x - g_stickyBoneRoot.x;
      const float dy = root.y - g_stickyBoneRoot.y;
      const float dz = root.z - g_stickyBoneRoot.z;
      if (dx * dx + dy * dy + dz * dz < 4.f) {
        *outEye = g_stickyBoneEye;
        return true;
      }
    }
    return false;
  }

  alignas(16) float xyz[8] = {};
  __try {
    g_pedGetBonePos(ped, xyz, static_cast<uint32_t>(kBoneHead));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (g_haveStickyBoneEye) {
      *outEye = g_stickyBoneEye;
      return true;
    }
    return false;
  }

  if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) {
    if (g_haveStickyBoneEye) {
      *outEye = g_stickyBoneEye;
      return true;
    }
    return false;
  }
  if (std::fabs(xyz[0]) < 1.f && std::fabs(xyz[1]) < 1.f && std::fabs(xyz[2]) < 1.f) {
    if (g_haveStickyBoneEye) {
      *outEye = g_stickyBoneEye;
      return true;
    }
    return false;
  }

  const float dx = xyz[0] - root.x;
  const float dy = xyz[1] - root.y;
  const float dz = xyz[2] - root.z;
  const float d2 = dx * dx + dy * dy + dz * dz;
  if (d2 < 0.01f || d2 > (2.2f * 2.2f)) {
    if (g_haveStickyBoneEye) {
      *outEye = g_stickyBoneEye;
      return true;
    }
    return false;
  }
  if (xyz[2] < root.z - 0.5f) {
    if (g_haveStickyBoneEye) {
      *outEye = g_stickyBoneEye;
      return true;
    }
    return false;
  }

  outEye->x = xyz[0];
  outEye->y = xyz[1];
  outEye->z = xyz[2];
  outEye->w = 1.f;
  if (!SaneWorldPos(*outEye)) {
    if (g_haveStickyBoneEye) {
      *outEye = g_stickyBoneEye;
      return true;
    }
    return false;
  }

  g_stickyBoneEye = *outEye;
  g_stickyBoneRoot = root;
  g_haveStickyBoneEye = true;

  const uint32_t n = ++g_boneThiscallOk;
  if (!g_boneThiscallLoggedOk) {
    g_boneThiscallLoggedOk = true;
    Log("CamMatrix: HEAD Vec3 eye OK pos=(%.3f,%.3f,%.3f) d=%.2f", outEye->x, outEye->y,
        outEye->z, std::sqrt(d2));
  } else if (n <= 5 || (n % 300) == 0) {
    Log("CamMatrix: HEAD Vec3 eye #%u pos=(%.3f,%.3f,%.3f) d=%.2f", n, outEye->x, outEye->y,
        outEye->z, std::sqrt(d2));
  }
  return true;
}

// Ped-fixed eye-center (Mode 155+) BEFORE 6DoF / camoff / IPD.
bool TryGetPedEyeCenterBase(Vec4* outEye) {
  if (!g_FindPlayerPed || !outEye)
    return false;
  void* ped = g_FindPlayerPed(0);
  if (!ped)
    return false;
  const StereoMode sm = GetStereoMode();
  const bool inVeh = IsPlayerInVehicleSticky();

  // Mode 193: stick to HEAD bone via CE thiscall (crouch/sprint).
  if (IsOursFpHeadBoneCam(sm) && !inVeh) {
    if (TryGetPedHeadBoneEye(outEye))
      return true;
    if (!g_boneThiscallLoggedFail) {
      g_boneThiscallLoggedFail = true;
      Log("CamMatrix: HEAD thiscall eye FAILED — fallback root+height");
    }
  }

  const float kEyeH = IsOursFpEyeCenterLow(sm) ? 0.65f : 0.78f;
  const bool carHead = IsOursFpInCarHead(sm) && inVeh;
  const float kPedFwd = carHead ? 0.02f : 0.08f;
  auto* pMat = *reinterpret_cast<Matrix44**>(reinterpret_cast<uint8_t*>(ped) + 0x20);
  if (!pMat) {
    const float* t = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(ped) + 0x10);
    outEye->x = t[0];
    outEye->y = t[1];
    outEye->z = t[2] + kEyeH;
    outEye->w = 1.f;
  } else {
    outEye->x = pMat->pos.x + pMat->at.x * kEyeH + pMat->up.x * kPedFwd;
    outEye->y = pMat->pos.y + pMat->at.y * kEyeH + pMat->up.y * kPedFwd;
    outEye->z = pMat->pos.z + pMat->at.z * kEyeH + pMat->up.z * kPedFwd;
    outEye->w = 1.f;
  }
  return SaneWorldPos(*outEye);
}

void ApplyHmdToCam(Matrix44* mat) {
  if (!g_gameplayActive.load())
    return;
  const StereoMode sm = GetStereoMode();
  // Mode 57 camera writes are valid only inside the pinned BuildRootA
  // receipt scope. A later camera callback cannot mutate the queued eye.
  if (sm == StereoMode::OpenXrTemporalStereo &&
      !g_camBuildReceiptScope.active) {
    return;
  }

  // Exit vehicle → drop car offsets and re-baseline 6DoF so foot height returns.
  {
    static bool s_wasSeated = false;
    const bool seated = IsPlayerSeatedForVrCam();
    if (s_wasSeated && !seated) {
      g_havePosBaseline = false;
      Log("VehCam: exit edge — 6DoF baseline cleared (foot height/offsets)");
    }
    s_wasSeated = seated;
  }

  vr::HmdMatrix34_t h{};
  if (!GetHmdPoseMatrix(&h))
    return;

  const bool eyeCenter = IsOursFpEyeCenterCam(sm);

  Vec4 eye{};
  // Mode 171: while freezing a dual pair, reuse the ped origin snapped at Begin.
  if (g_dualCamFreeze.load() && g_haveFrozenPedEye) {
    eye = g_frozenPedEye;
  } else if (eyeCenter) {
    // Mode 155/156: ped-fixed eye-center pivot (between eyes), not skull+6DoF.
    if (!TryGetPedEyeCenterBase(&eye))
      return;
  } else if (!TryGetPedEyePos(&eye) || !SaneWorldPos(eye)) {
    // No real ped yet — never reuse stale eye (menu → freeze risk)
    return;
  }
  g_lastEye = eye;
  g_haveLastEye = true;

  // Seated 6DoF on top of ped eye (F10 resets translation baseline).
  // Mode 155: skip — keep cam fixed on ped (no lean/shoulder orbit from HMD pos).
  float px, py, pz;
  OvrToGta(h.m[0][3], h.m[1][3], h.m[2][3], &px, &py, &pz);
  if (!g_havePosBaseline) {
    g_basePx = px;
    g_basePy = py;
    g_basePz = pz;
    g_havePosBaseline = true;
  }
  if (!eyeCenter) {
    const float worldScale = GetWorldScale();
    eye.x += (px - g_basePx) * kPosScale * worldScale;
    eye.y += (py - g_basePy) * kPosScale * worldScale;
    eye.z += (pz - g_basePz) * kPosScale * worldScale;
  }

  float fx, fy, fz;
  OvrToGta(-h.m[0][2], -h.m[1][2], -h.m[2][2], &fx, &fy, &fz);
  Normalize(&fx, &fy, &fz);

  // Modes 47/48 only: leveled pitch flip (Mode 49+ OFF — headset said flip=1 reversed).
  const bool leveledPitchFlip =
      sm == StereoMode::HeadOwnedCamLeveledPitchFlip ||
      sm == StereoMode::HeadOwnedCamPitchStable;
  if (leveledPitchFlip)
    fz = -fz;

  // Mode 46 keeps the complete HMD basis. The forward + world-up rebuild
  // deliberately discarded physical roll, which makes a head tilt look like
  // the game camera is resisting/inverting it. OpenVR matrix columns are the
  // HMD local axes in tracking space; apply OvrToGta to each column once.
  const bool fullHeadPose = sm == StereoMode::HeadOwnedCamFullPose;
  float hRightX = 0.f, hRightY = 0.f, hRightZ = 0.f;
  float hUpX = 0.f, hUpY = 0.f, hUpZ = 0.f;
  if (fullHeadPose) {
    OvrToGta(h.m[0][0], h.m[1][0], h.m[2][0], &hRightX, &hRightY, &hRightZ);
    OvrToGta(h.m[0][1], h.m[1][1], h.m[2][1], &hUpX, &hUpY, &hUpZ);
    Normalize(&hRightX, &hRightY, &hRightZ);
    Normalize(&hUpX, &hUpY, &hUpZ);
  }

  // Right-stick yaw offset (controller camera L/R) + optional vehicle follow, on top of HMD.
  // Rotate every HMD axis together so Mode 46 retains physical pitch and roll.
  {
    const float yawOff = GetControllerYawOffset() + ComputeVehicleYawOffset();
    const float c = std::cos(yawOff);
    const float s = std::sin(yawOff);
    const float nfx = fx * c - fy * s;
    const float nfy = fx * s + fy * c;
    fx = nfx;
    fy = nfy;
    if (fullHeadPose) {
      const float nrx = hRightX * c - hRightY * s;
      const float nry = hRightX * s + hRightY * c;
      const float nux = hUpX * c - hUpY * s;
      const float nuy = hUpX * s + hUpY * c;
      hRightX = nrx;
      hRightY = nry;
      hUpX = nux;
      hUpY = nuy;
    }
    Normalize(&fx, &fy, &fz);
  }

  // Mode 49 and 50+: yaw follows live ped heading + HMD yaw delta (F9 resets ped baseline).
  bool pedCoupledYaw = false;
  if (UsesPedCoupledYaw(sm)) {
    float pedHeading = 0.f;
    if (TryGetPedHorizontalHeading(&pedHeading)) {
      if (!g_havePedHeadingBase) {
        g_pedHeadingBase = pedHeading;
        g_havePedHeadingBase = true;
      }
      const float horizLen = std::sqrt(fx * fx + fy * fy);
      if (horizLen > 1e-6f) {
        const float hmdHeading = std::atan2(fx, fy);
        const float coupledHeading =
            hmdHeading + WrapPi(pedHeading - g_pedHeadingBase);
        const float s = std::sin(coupledHeading);
        const float c = std::cos(coupledHeading);
        fx = s * horizLen;
        fy = c * horizLen;
        Normalize(&fx, &fy, &fz);
        pedCoupledYaw = true;
      }
    }
  }

  // Modes through 45 and Mode 47+ level the horizon by rebuilding right/up from
  // world-up. Mode 46 uses the HMD's rigid basis: right=X, forward=Y, up=Z in RAGE.
  const bool pitchStableMode =
      static_cast<int>(sm) >= static_cast<int>(StereoMode::HeadOwnedCamPitchStable);
  constexpr float kPitchStableHoriz = 0.26f;  // ~75 deg from horizontal
  static float s_stableRx = 1.f, s_stableRy = 0.f, s_stableRz = 0.f;
  static bool s_haveStableBasis = false;
  const float fwdHoriz = std::sqrt(fx * fx + fy * fy);

  float rx = fy;
  float ry = -fx;
  float rz = 0.f;
  float ux = ry * fz - rz * fy;
  float uy = rz * fx - rx * fz;
  float uz = rx * fy - ry * fx;
  bool usedPitchStable = false;
  if (fullHeadPose) {
    rx = hRightX;
    ry = hRightY;
    rz = hRightZ;
    ux = hUpX;
    uy = hUpY;
    uz = hUpZ;
  } else if (pitchStableMode && fwdHoriz < kPitchStableHoriz && s_haveStableBasis) {
    // Near-vertical look: world-up cross degenerates → snap/warp. Keep last stable
    // horizontal right and rebuild up from current forward (no Mode-46 HMD up).
    rx = s_stableRx;
    ry = s_stableRy;
    rz = s_stableRz;
    ux = ry * fz - rz * fy;
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;
    Normalize(&ux, &uy, &uz);
    usedPitchStable = true;
  } else {
    float rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rlen < 1e-4f) {
      if (pitchStableMode && s_haveStableBasis) {
        rx = s_stableRx;
        ry = s_stableRy;
        rz = s_stableRz;
        usedPitchStable = true;
      } else {
        rx = 1.f;
        ry = 0.f;
        rz = 0.f;
      }
    } else {
      rx /= rlen;
      ry /= rlen;
      rz /= rlen;
    }
    ux = ry * fz - rz * fy;
    uy = rz * fx - rx * fz;
    uz = rx * fy - ry * fx;
    Normalize(&ux, &uy, &uz);
    if (fwdHoriz >= kPitchStableHoriz) {
      s_stableRx = rx;
      s_stableRy = ry;
      s_stableRz = rz;
      s_haveStableBasis = true;
    }
  }

  const float eyeFwd = eyeCenter ? 0.f : GetEyeForwardMeters();
  eye.x += fx * eyeFwd;
  eye.y += fy * eyeFwd;
  eye.z += fz * eyeFwd;

  // Inspiration FirstPerson.ini FPX/FPY/FPZ (our units: cm via gtaiv_dxvk_vr.camoff).
  float offR = 0.f, offF = 0.f, offU = 0.f;
  GetCamOffsetMeters(&offR, &offF, &offU);
  // Mode 164/165 in vehicle only: extra rearward offset (vehcamoff). Cleared on foot.
  if (IsOursFpInCarHead(sm) && IsPlayerInVehicleSticky()) {
    float vr = 0.f, vf = 0.f, vu = 0.f;
    GetVehicleCamOffsetMeters(&vr, &vf, &vu);
    offR += vr;
    offF += vf;
    offU += vu;
  }
  if (offR != 0.f || offF != 0.f || offU != 0.f) {
    eye.x += rx * offR + fx * offF + ux * offU;
    eye.y += ry * offR + fy * offF + uy * offU;
    eye.z += rz * offR + fz * offF + uz * offU;
  }

  // Inspiration-style head/hair hide (CE SetDraw helper — not EndScene natives).
  UpdatePedHeadHide();

  // Mode 178+: freeze FULL pre-IPD cam (basis+center) across L→R; only ±IPD differs.
  if (g_dualCamFreeze.load() && g_freezeFullBasis) {
    if (!g_haveFrozenCenter) {
      g_frozenCenterEye = eye;
      g_frozenFx = fx;
      g_frozenFy = fy;
      g_frozenFz = fz;
      g_frozenRx = rx;
      g_frozenRy = ry;
      g_frozenRz = rz;
      g_frozenUx = ux;
      g_frozenUy = uy;
      g_frozenUz = uz;
      g_haveFrozenCenter = true;
      static uint32_t s_capN = 0;
      const uint32_t cn = ++s_capN;
      if (cn <= 6 || (cn % 600) == 0)
        Log("Mode178: full cam freeze capture #%u center=(%.3f,%.3f,%.3f)", cn, eye.x, eye.y,
            eye.z);
    } else {
      const float dpx = eye.x - g_frozenCenterEye.x;
      const float dpy = eye.y - g_frozenCenterEye.y;
      const float dpz = eye.z - g_frozenCenterEye.z;
      const float dpos =
          std::sqrt(dpx * dpx + dpy * dpy + dpz * dpz);
      const float dbasis = std::fabs(fx - g_frozenFx) + std::fabs(fy - g_frozenFy) +
                           std::fabs(fz - g_frozenFz) + std::fabs(rx - g_frozenRx) +
                           std::fabs(ry - g_frozenRy) + std::fabs(rz - g_frozenRz);
      static uint32_t s_deltaN = 0;
      const uint32_t dn = ++s_deltaN;
      if (dn <= 8 || (dn % 600) == 0)
        Log("Mode178: inter-eye cam delta #%u beforeFreeze dpos=%.4fm dbasis=%.5f "
            "(expect ~0 after)",
            dn, dpos, dbasis);
      eye = g_frozenCenterEye;
      fx = g_frozenFx;
      fy = g_frozenFy;
      fz = g_frozenFz;
      rx = g_frozenRx;
      ry = g_frozenRy;
      rz = g_frozenRz;
      ux = g_frozenUx;
      uy = g_frozenUy;
      uz = g_frozenUz;
    }
  }

  // Stereo eye origin — L4D2VR GetViewOriginLeft/Right:
  //   origin + forward*(-eyeZ*scale) + right*(±IPD*ipdScale*scale/2)
  // Cover FOV + TextureBounds (Submit) handle fusion; IPD alone is not enough.
  float ipdX = 0.f, ipdY = 0.f, ipdZ = 0.f;
  bool directOpenXrEyeOffsetApplied = false;
  EyeOffset directOpenXrLocalEyeOffset{};
  const bool rightEye = (GetStereoEye() == StereoEye::Right);
  // Mode 172 mono-pair and direct OpenXR immersive mono use a center camera.
  if (sm >= StereoMode::DualIpd &&
      sm != StereoMode::OpenXrImmersiveMono &&
      !IsOursFpMonoPair(sm)) {
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
    // Same-frame full freeze: IPD uses frozen right axis (not a later HMD sample).
    if (g_dualCamFreeze.load() && g_freezeFullBasis && g_haveFrozenCenter) {
      hrx = rx;
      hry = ry;
      hrz = rz;
    }

    // Optional EyeToHead forward (OpenVR col2 translation) like L4D2 m_EyeZ.
    float eyeZ = 0.f;
    EyeOffset eyeOffset{};
    const bool haveEyeOffset = GetCachedEyeOffset(rightEye, &eyeOffset);
    if (haveEyeOffset)
      eyeZ = eyeOffset.z;

    if ((sm == StereoMode::OpenXrDrawSceneStereo ||
         sm == StereoMode::OpenXrTemporalStereo) &&
        haveEyeOffset) {
      // The x64 OpenXR host supplies exact LOCAL-space eye poses. Transform
      // the complete eye-to-head vector through the cached HMD basis rather
      // than inventing an IPD from the legacy user configuration.
      float hux, huy, huz;
      float hbackX, hbackY, hbackZ;
      OvrToGta(h.m[0][1], h.m[1][1], h.m[2][1], &hux, &huy, &huz);
      OvrToGta(h.m[0][2], h.m[1][2], h.m[2][2],
               &hbackX, &hbackY, &hbackZ);
      Normalize(&hux, &huy, &huz);
      Normalize(&hbackX, &hbackY, &hbackZ);
      ipdX = hrx * eyeOffset.x + hux * eyeOffset.y + hbackX * eyeOffset.z;
      ipdY = hry * eyeOffset.x + huy * eyeOffset.y + hbackY * eyeOffset.z;
      ipdZ = hrz * eyeOffset.x + huz * eyeOffset.y + hbackZ * eyeOffset.z;
    } else {
      // Upstream Mode187+ deliberately scales the legacy stereo baseline with
      // WorldScale. Modes 56/57 above keep the host's exact local eye poses.
      // Mode 58 intentionally stays on Mode 204's proven configured-IPD law;
      // the pinned OpenXR eye transform still supplies eye-Z and proves that
      // both walks used one coherent runtime view sample.
      const float ws =
          IsOursFpTrueWorldScale(sm) ? GetWorldScale() : 1.f;
      const float half =
          0.5f * GetStereoSepMeters() * GetStereoScale() * ws;
      const float s = rightEye ? half : -half;
      const float fzOff = -eyeZ * ws;
      ipdX = hrx * s + fx * fzOff;
      ipdY = hry * s + fy * fzOff;
      ipdZ = hrz * s + fz * fzOff;
    }
    if (std::isfinite(ipdX) && std::isfinite(ipdY) && std::isfinite(ipdZ) &&
        std::fabs(ipdX) < 20.f && std::fabs(ipdY) < 20.f && std::fabs(ipdZ) < 20.f) {
      eye.x += ipdX;
      eye.y += ipdY;
      eye.z += ipdZ;
      directOpenXrEyeOffsetApplied =
          (sm == StereoMode::OpenXrDrawSceneStereo ||
           sm == StereoMode::OpenXrTemporalStereo ||
           sm == StereoMode::OpenXrFusedFirstPerson) &&
          haveEyeOffset;
      if (directOpenXrEyeOffsetApplied)
        directOpenXrLocalEyeOffset = eyeOffset;
    } else {
      ipdX = ipdY = ipdZ = 0.f;
    }
  }
  if (sm == StereoMode::OpenXrTemporalStereo &&
      !directOpenXrEyeOffsetApplied) {
    return;
  }

  if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z))
    return;

  mat->right = {rx, ry, rz, 0.f};
  mat->up = {fx, fy, fz, 0.f};
  mat->at = {ux, uy, uz, 0.f};
  mat->pos = eye;

  const uint32_t n =
      g_applyCount.fetch_add(1u, std::memory_order_acq_rel) + 1u;
  AcquireSRWLockExclusive(&g_lastAppliedLock);
  g_lastAppliedPos = eye;
  g_haveLastApplied = true;
  ReleaseSRWLockExclusive(&g_lastAppliedLock);
  if (g_camBuildReceiptScope.active) {
    const DWORD writerThreadId = GetCurrentThreadId();
    if (writerThreadId !=
            g_camBuildReceiptScope.writerThreadId ||
        rightEye !=
            g_camBuildReceiptScope.expectedRightEye) {
      g_camBuildReceiptScope.eyeMismatch = true;
    }
    g_camBuildReceiptScope.applyGeneration = n;
    ++g_camBuildReceiptScope.applyCount;
    g_camBuildReceiptScope.lastRightEye = rightEye;
    g_camBuildReceiptScope.lastPosition = eye;
    g_camBuildReceiptScope.lastPreEyePosition = {
        eye.x - ipdX, eye.y - ipdY, eye.z - ipdZ, 0.f};
    g_camBuildReceiptScope.firstPersonAnchor =
        eyeCenter && IsOpenXrFusedFirstPerson(sm);
    const float basis[9] = {
        rx, ry, rz,
        fx, fy, fz,
        ux, uy, uz};
    std::memcpy(
        g_camBuildReceiptScope.lastBasis,
        basis,
        sizeof(basis));
    if (!directOpenXrEyeOffsetApplied) {
      g_camBuildReceiptScope.eyeOffsetMismatch = true;
    } else if (!g_camBuildReceiptScope.eyeOffsetApplied) {
      g_camBuildReceiptScope.eyeOffsetApplied = true;
      g_camBuildReceiptScope.appliedEyeOffset =
          {ipdX, ipdY, ipdZ, 0.f};
      g_camBuildReceiptScope.selectedLocalEyeOffset = {
          directOpenXrLocalEyeOffset.x,
          directOpenXrLocalEyeOffset.y,
          directOpenXrLocalEyeOffset.z,
          0.f};
    } else if (
        std::fabs(
            g_camBuildReceiptScope.appliedEyeOffset.x -
            ipdX) > 1e-4f ||
        std::fabs(
            g_camBuildReceiptScope.appliedEyeOffset.y -
            ipdY) > 1e-4f ||
        std::fabs(
            g_camBuildReceiptScope.appliedEyeOffset.z -
            ipdZ) > 1e-4f ||
        std::fabs(
            g_camBuildReceiptScope.selectedLocalEyeOffset.x -
            directOpenXrLocalEyeOffset.x) > 1e-4f ||
        std::fabs(
            g_camBuildReceiptScope.selectedLocalEyeOffset.y -
            directOpenXrLocalEyeOffset.y) > 1e-4f ||
        std::fabs(
            g_camBuildReceiptScope.selectedLocalEyeOffset.z -
            directOpenXrLocalEyeOffset.z) > 1e-4f) {
      g_camBuildReceiptScope.eyeOffsetMismatch = true;
    }
  }
  if (n <= 5 || (n % 300) == 0) {
    Log("CamMatrix: FP lock #%u %s pos=(%.3f,%.3f,%.3f) ipd=(%.4f,%.4f,%.4f) sep=%.0fcm "
        "eyeFwd=%.0fcm eyeCenter=%d fullHmdBasis=%d leveledPitchFlip=%d pitchStable=%d "
        "pedCoupled=%d",
        n, rightEye ? "R" : "L", eye.x, eye.y, eye.z, ipdX, ipdY, ipdZ,
        GetStereoSepMeters() * 100.f, eyeFwd * 100.f, eyeCenter ? 1 : 0, fullHeadPose ? 1 : 0,
        leveledPitchFlip ? 1 : 0, usedPitchStable ? 1 : 0, pedCoupledYaw ? 1 : 0);
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
    if (IsExternalFpHost(sm)) {
      // Snapshot FirstPerson cam, then IPD for current stereo eye.
      g_fpHostBase = *mat;
      g_haveFpHostBase = true;
      ApplyFpHostStereoIpd(mat);
    } else {
      ApplyHmdToCam(mat);
    }
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

void NotifyHeadBoneSoftSkip(unsigned ms, const char* why) {
  // Kept for dual-AV path; bone no longer uses timed soft-skip (jumped cam).
  (void)ms;
  if (why)
    Log("CamMatrix: Mode193 dual-AV note (%s) — HOLD stereo, skip DrawScene", why);
}

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

  // FusionFix / SpeedoIV CE pattern — returns null on foot (unlike lingering m_pVehicle).
  const uintptr_t findVeh =
      FindPattern(nullptr, "8B 44 24 04 85 C0 75 15 A1 ? ? ? ? 83 F8 FF 75 04 33 C0 EB 07");
  if (findVeh) {
    g_FindPlayerVehicle = reinterpret_cast<FindPlayerVehicle_t>(findVeh);
    Log("CamMatrix: FindPlayerVehicle @ %p (sit detect)", reinterpret_cast<void*>(findVeh));
  } else {
    g_FindPlayerVehicle = nullptr;
    Log("CamMatrix: FindPlayerVehicle pattern MISS — sit detect falls back to m_pVehicle");
  }

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
  Log("CamMatrix: %d/4 hooked — HARD FP on ped (F9 recenter, F10 6DoF reset)", ok);
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
  // Mode 140: leave cam to FirstPerson; HMD→mouse (hmd_look) stays active.
  if (IsExternalFpHost(GetStereoMode()))
    return false;
  return g_hooksOk.load() && g_gameplayActive.load();
}

bool IsStereoRenderArmed() {
  return g_hooksOk.load() && g_gameplayActive.load();
}

void CamMatrixOnRecenter() {
  g_haveVehYawBase = false;
  g_havePedHeadingBase = false;
  Log("CamMatrix: heading baseline reset (F9) — 6DoF translation unchanged");
}

void CamMatrixOnSixDofReset() {
  g_havePosBaseline = false;
  Log("SixDofReset: F10 — head translation origin zeroed");
}

void PollCamHotkeys() {
  static bool wasF10 = false;
  const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
  const bool edge = down && !wasF10;
  wasF10 = down;
  if (edge)
    CamMatrixOnSixDofReset();
}

void RefreshLiveCamForStereoEye() {
  if (!g_gameplayActive.load())
    return;

  UpdateEnterCarFpWindow();

  const bool fpHost = IsExternalFpHost(GetStereoMode());
  if (g_trackedCount > 0) {
    for (int i = 0; i < g_trackedCount; ++i) {
      if (!g_trackedMats[i])
        continue;
      if (fpHost)
        ApplyFpHostStereoIpd(g_trackedMats[i]);
      else
        ApplyHmdToCam(g_trackedMats[i]);
    }
    return;
  }
  if (g_liveCamMat) {
    if (fpHost)
      ApplyFpHostStereoIpd(g_liveCamMat);
    else
      ApplyHmdToCam(g_liveCamMat);
  }
}

void BeginStereoDualCamFreeze() {
  const StereoMode sm = GetStereoMode();
  if (!IsOursFpDualCamFreeze(sm))
    return;
  Vec4 eye{};
  const bool eyeCenter = IsOursFpEyeCenterCam(sm);
  const bool ok = eyeCenter ? TryGetPedEyeCenterBase(&eye)
                            : (TryGetPedEyePos(&eye) && SaneWorldPos(eye));
  if (!ok) {
    g_dualCamFreeze.store(false);
    g_haveFrozenPedEye = false;
    g_freezeFullBasis = false;
    g_haveFrozenCenter = false;
    return;
  }
  g_frozenPedEye = eye;
  g_haveFrozenPedEye = true;
  g_freezeFullBasis = IsOursFpSameFrameFreeze(sm) || IsOursFpSameTickDual(sm) ||
                      IsOursFpSameTickParentDualFreeze(sm);
  g_haveFrozenCenter = false;  // first ApplyHmdToCam captures full pre-IPD cam
  g_dualCamFreeze.store(true);
  static uint32_t s_n = 0;
  const uint32_t n = ++s_n;
  if (n <= 6 || (n % 600) == 0) {
    if (IsOursFpSameTickParentDualFreeze(sm))
      Log("Mode201: dual cam freeze begin #%u pedEye=(%.3f,%.3f,%.3f) fullBasis=1", n, eye.x,
          eye.y, eye.z);
    else if (IsOursFpSameTickDual(sm))
      Log("Mode189: dual cam freeze begin #%u pedEye=(%.3f,%.3f,%.3f) fullBasis=1", n, eye.x,
          eye.y, eye.z);
    else if (g_freezeFullBasis)
      Log("Mode178: dual cam freeze begin #%u pedEye=(%.3f,%.3f,%.3f) fullBasis=1", n, eye.x,
          eye.y, eye.z);
    else
      Log("Mode171: dual cam freeze #%u pedEye=(%.3f,%.3f,%.3f)", n, eye.x, eye.y, eye.z);
  }
}

void EndStereoDualCamFreeze() {
  g_dualCamFreeze.store(false);
  g_haveFrozenPedEye = false;
  g_freezeFullBasis = false;
  g_haveFrozenCenter = false;
}

uint32_t GetStereoCamApplyGeneration() {
  return g_applyCount.load(std::memory_order_acquire);
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
  if (!x || !y || !z)
    return false;
  AcquireSRWLockShared(&g_lastAppliedLock);
  if (!g_haveLastApplied) {
    ReleaseSRWLockShared(&g_lastAppliedLock);
    return false;
  }
  *x = g_lastAppliedPos.x;
  *y = g_lastAppliedPos.y;
  *z = g_lastAppliedPos.z;
  ReleaseSRWLockShared(&g_lastAppliedLock);
  return true;
}

void BeginStereoCamBuildReceipt(bool rightEye) {
  g_camBuildReceiptScope = {};
  g_camBuildReceiptScope.writerThreadId =
      GetCurrentThreadId();
  g_camBuildReceiptScope.expectedRightEye = rightEye;
  g_camBuildReceiptScope.active = true;
}

bool EndStereoCamBuildReceipt(
    StereoCamBuildReceipt* receipt) {
  if (!receipt)
    return false;
  *receipt = {};
  const CamBuildReceiptScope scope =
      g_camBuildReceiptScope;
  g_camBuildReceiptScope = {};
  if (!scope.active ||
      scope.applyCount == 0u ||
      scope.applyGeneration == 0u ||
      scope.writerThreadId != GetCurrentThreadId() ||
      scope.eyeMismatch ||
      !scope.eyeOffsetApplied ||
      scope.eyeOffsetMismatch ||
      scope.lastRightEye != scope.expectedRightEye) {
    return false;
  }
  receipt->applyGeneration = scope.applyGeneration;
  receipt->applyCount = scope.applyCount;
  receipt->writerThreadId = scope.writerThreadId;
  receipt->rightEye = scope.lastRightEye;
  receipt->eyeOffsetApplied = scope.eyeOffsetApplied;
  receipt->firstPersonAnchor = scope.firstPersonAnchor;
  receipt->position[0] = scope.lastPosition.x;
  receipt->position[1] = scope.lastPosition.y;
  receipt->position[2] = scope.lastPosition.z;
  receipt->preEyePosition[0] = scope.lastPreEyePosition.x;
  receipt->preEyePosition[1] = scope.lastPreEyePosition.y;
  receipt->preEyePosition[2] = scope.lastPreEyePosition.z;
  receipt->eyeOffset[0] = scope.appliedEyeOffset.x;
  receipt->eyeOffset[1] = scope.appliedEyeOffset.y;
  receipt->eyeOffset[2] = scope.appliedEyeOffset.z;
  receipt->localEyeOffset[0] =
      scope.selectedLocalEyeOffset.x;
  receipt->localEyeOffset[1] =
      scope.selectedLocalEyeOffset.y;
  receipt->localEyeOffset[2] =
      scope.selectedLocalEyeOffset.z;
  std::memcpy(
      receipt->basis,
      scope.lastBasis,
      sizeof(receipt->basis));
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
    const float add = lowMotion ? (std::min)(requestedAdd, 12.f) : requestedAdd;
    if (lowMotion && requestedAdd > add) {
      static bool s_loggedLowMotionCap = false;
      if (!s_loggedLowMotionCap) {
        s_loggedLowMotionCap = true;
        Log("Mode39: fovadd requested=%.0f capped=%.0f deg (lower temporal/FOV stress)",
            requestedAdd, add);
      }
    }
    float after = before;
    static float s_lastWritten = -1.f;
    constexpr float kGameplayFovHi = 55.f;

    // Modes 153–155: write ForwardFOV (fpfov) into CCam+0x60 to widen the engine BB.
    // 153: lock gameTan to cover (aspect lie — can stretch V).
    // 154/155: aspect-fit under-publish (preserve BB aspect, scale into cover).
    // Absolute 90 + true CCam publish (Mode 152) caused StretchRect SRC crop zoom-IN.
    if (WantsFpAbsoluteFov(sm)) {
      const float target = GetFpForwardFovDegrees();
      if (target >= 40.f && target <= 120.f && std::isfinite(target)) {
        *fov = target;
        after = target;
        s_lastWritten = target;
      } else {
        s_lastWritten = -1.f;
        after = before;
      }
      if (IsOursFpWideAspectFit(sm))
        PublishGameFovUnderPublishFit(after, GetBackbufferAspect(), GetUnderPublishOverscan());
      else
        PublishGameFovFromCover();
    } else if (add > 0.05f && before <= kGameplayFovHi) {
      // Idempotent ADD: the cam CALL usually resets CCam+0x60 to the base FOV, then
      // we ADD. Sometimes the CALL leaves our previous write in place — adding again
      // compounds (67→89) and blows canvas fill past 100% (headset 2026-07-24).
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
      s_lastWritten = -1.f;
      after = before;
    } else {
      s_lastWritten = -1.f;
    }

    // Mode 36/37: canvas must track TRUE engine FOV (not stale GetTransform).
    // Mode 35 baseline left unchanged (protect headset-good warp).
    // Modes 153–155: skip — under-publish already applied above.
    if (sm == StereoMode::FovRecomputeTrueCanvas || sm == StereoMode::FovCanvasComfort ||
        sm == StereoMode::AerPoseSubmit || sm == StereoMode::FovCanvasLowMotion ||
        sm == StereoMode::FovCanvasMotionGuard || sm == StereoMode::ReplayCallChainProbe ||
        sm == StereoMode::ReplayOwnerCountProbe || sm == StereoMode::FovCanvasMotionGuardFast ||
        sm == StereoMode::FovCanvasMotionGuardRtLock || sm == StereoMode::HeadOwnedCamSpike ||
        sm == StereoMode::HeadOwnedCamFullPose ||
        sm == StereoMode::HeadOwnedCamLeveledPitchFlip ||
        sm == StereoMode::HeadOwnedCamPitchStable ||
        sm == StereoMode::HeadOwnedCamPedCoupled ||
        sm == StereoMode::HeadOwnedCamStereoAlways ||
        sm == StereoMode::HeadOwnedCamStereoAer ||
        sm == StereoMode::HeadOwnedCamStereoSwap ||
        sm == StereoMode::HeadOwnedCamStereoSoftGuard ||
        sm == StereoMode::OpenXrImmersiveMono ||
        sm == StereoMode::OpenXrDrawSceneStereo ||
        sm == StereoMode::OpenXrTemporalStereo ||
        IsCleanDualLookMove(sm) ||
        (IsHeadHideNativeOurs(sm) && !WantsFpAbsoluteFov(sm)))
      PublishGameFovFromCCamDegrees(after, GetBackbufferAspect());

    // Modes 45/46/47/48/49: Rage can revise a camera after CopyMat but before this already-safe
    // post-process FOV site. Re-apply the HMD pose here, late in the game-thread
    // camera path. Mode 46 additionally preserves HMD roll. This does not move
    // gameplay's collision camera, add a VS offset, or make another render/replay pass.
    if (sm == StereoMode::HeadOwnedCamSpike || sm == StereoMode::HeadOwnedCamFullPose ||
        sm == StereoMode::HeadOwnedCamLeveledPitchFlip ||
        sm == StereoMode::HeadOwnedCamPitchStable ||
        sm == StereoMode::HeadOwnedCamPedCoupled ||
        sm == StereoMode::HeadOwnedCamStereoAlways ||
        sm == StereoMode::HeadOwnedCamStereoAer ||
        sm == StereoMode::HeadOwnedCamStereoSwap ||
        sm == StereoMode::HeadOwnedCamStereoSoftGuard ||
        sm == StereoMode::OpenXrImmersiveMono ||
        sm == StereoMode::OpenXrDrawSceneStereo ||
        IsCleanDualLookMove(sm) ||
        IsHeadHideNativeOurs(sm)) {
      // Mode 165: enter/jack often uses a script cam that never hits our 4 CopyMat
      // hooks. mat = CCam+0x10 (FOV at CCam+0x60 = mat+0x50). Always force HMD FP
      // onto the live CCam so jacking stays first-person.
      if (IsOursFpEnterCarFp(sm)) {
        UpdateEnterCarFpWindow();
        auto* ccamMat =
            reinterpret_cast<Matrix44*>(reinterpret_cast<uint8_t*>(self) + 0x10);
        // Start enter window if game cam is already far from ped (jack anim before
        // m_pVehicle is set).
        if (!IsPlayerVehiclePtrSet()) {
          Vec4 pedEye{};
          if (TryGetPedEyePos(&pedEye)) {
            const float dx = ccamMat->pos.x - pedEye.x;
            const float dy = ccamMat->pos.y - pedEye.y;
            const float dz = ccamMat->pos.z - pedEye.z;
            const float dist2 = dx * dx + dy * dy + dz * dz;
            if (dist2 > 2.5f * 2.5f) {
              g_enterFpUntil = GetTickCount() + 5000;
              static uint32_t s_far = 0;
              if (++s_far <= 4 || (s_far % 200) == 0)
                Log("EnterFp: CCam far from ped (%.1fm) — jack/enter force FP",
                    std::sqrt(dist2));
            }
          }
        }
        ApplyHmdToCam(ccamMat);
        g_liveCamMat = ccamMat;
        TrackCamMat(ccamMat);
        static uint32_t s_enterForce = 0;
        const uint32_t ef = ++s_enterForce;
        if (ef <= 6 || (ef % 300) == 0)
          Log("EnterFp: forced HMD onto CCam+0x10 #%u (window=%d vehPtr=%d)", ef,
              IsEnterCarFpWindowActive() ? 1 : 0, IsPlayerVehiclePtrSet() ? 1 : 0);
      } else {
        RefreshLiveCamForStereoEye();
      }
      static uint32_t s_headOwnedRefreshes = 0;
      const uint32_t refreshes = ++s_headOwnedRefreshes;
      if (refreshes <= 4 || (refreshes % 600) == 0)
        Log("Mode%d: late head-owned CopyMat refresh #%u (CCam site; fullBasis=%d; "
            "leveledPitchFlip=%d; pitchStable=%d; pedCoupled=%d; collision unchanged)",
            static_cast<int>(sm), refreshes, sm == StereoMode::HeadOwnedCamFullPose ? 1 : 0,
            (sm == StereoMode::HeadOwnedCamLeveledPitchFlip ||
             sm == StereoMode::HeadOwnedCamPitchStable)
                ? 1
                : 0,
            static_cast<int>(sm) >= static_cast<int>(StereoMode::HeadOwnedCamPitchStable)
                ? 1
                : 0,
            sm == StereoMode::HeadOwnedCamPedCoupled || UsesPedCoupledYaw(sm) ? 1 : 0);
    }
    const uint32_t n = ++g_fovSiteCalls;
    if (n <= 4 || (n % 600) == 0)
      Log("FovSite: #%u CCam+0x60 %.3f -> %.3f (add=%.0f) self=%p trueCanvas=%d underPub=%d", n,
          before, after, add, self,
          (sm == StereoMode::FovRecomputeTrueCanvas || sm == StereoMode::FovCanvasComfort ||
           sm == StereoMode::AerPoseSubmit || sm == StereoMode::FovCanvasLowMotion ||
           sm == StereoMode::FovCanvasMotionGuard || sm == StereoMode::ReplayCallChainProbe ||
           sm == StereoMode::ReplayOwnerCountProbe || sm == StereoMode::FovCanvasMotionGuardFast ||
           sm == StereoMode::FovCanvasMotionGuardRtLock || sm == StereoMode::HeadOwnedCamSpike ||
           sm == StereoMode::HeadOwnedCamFullPose ||
           sm == StereoMode::HeadOwnedCamLeveledPitchFlip ||
           sm == StereoMode::HeadOwnedCamPitchStable ||
           sm == StereoMode::HeadOwnedCamPedCoupled ||
           IsCleanDualLookMove(sm) ||
           (IsHeadHideNativeOurs(sm) && !WantsFpAbsoluteFov(sm)))
              ? 1
              : 0,
          WantsFpAbsoluteFov(sm) ? 1 : 0);
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

