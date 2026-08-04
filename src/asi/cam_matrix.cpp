#include "cam_matrix.h"
#include "aob.h"
#include "hmd_pose.h"
#include "log.h"
#include "ped_hide.h"
#include "stereo_calib.h"
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

// Mode 271 AER: park the live camera at the true head CENTRE (no eye offset)
// when handing it back to the game after a frame — see RefreshLiveCamCentered.
std::atomic<bool> g_camCenterOverride{false};

// Mode 271 AER: the view basis ApplyHmdToCam last WROTE, in world space
// (RAGE convention: right, forward = mat.up, up = mat.at). The AER
// reprojection differences two of these to get the exact rotation between the
// frame an eye was rendered in and the frame it is submitted in. HMD poses
// would not do: the bake also folds in stick yaw, vehicle yaw and the
// world-up relevel, and the pixels describe the baked view, not the raw head
// pose.
bool g_haveLastBasis = false;
float g_lastBasisR[3]{};
float g_lastBasisF[3]{};
float g_lastBasisU[3]{};

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
  g_lastAppliedPos = mat->pos;
  g_haveLastApplied = true;

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
// Mode216: SampleDeferredHeadBoneEye calls this once per EndScene; cam bake only
// reads g_deferredBoneEye (never GetBonePos mid-DrawScene).
Vec4 g_stickyBoneEye{};
Vec4 g_stickyBoneRoot{};
bool g_haveStickyBoneEye = false;

Vec4 g_deferredBoneEye{};
Vec4 g_deferredBoneRoot{};
bool g_haveDeferredBoneEye = false;
DWORD g_deferredBoneTick = 0;
uint32_t g_deferredSampleOk = 0;
uint32_t g_deferredSampleFail = 0;
// Mode216: adaptive track — stick on stairs/crouch, damp walk bob.
Vec4 g_deferredWorldEye{};
bool g_haveDeferredWorldEye = false;
Vec4 g_deferredSmoothOff{};  // smoothed (bone - root)
bool g_haveDeferredSmoothOff = false;
float g_deferredSmoothRootZ = 0.f;
float g_deferredSmoothRootX = 0.f;
float g_deferredSmoothRootY = 0.f;
bool g_haveDeferredSmoothRootZ = false;

// Mode 234 SAME-STATE: one yaw + one ped eye-center for the L/R dual pair.
std::atomic<bool> g_sameStateLatch{false};
float g_sameStateYawOff = 0.f;
Vec4 g_sameStatePedEye{};
bool g_haveSameStatePedEye = false;

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
    // Mode216: one AV → kill bone for the session (height fallback). SEH does not
    // save a stack-smash, but catches plain AVs from early/bad ped state.
    if (IsOursFpDeferredHeadBone(GetStereoMode())) {
      g_boneThiscallDead = true;
      Log("CamMatrix: deferred HEAD SEH — bone DISABLED for session (use height)");
    }
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

bool TryReadPedRoot(Vec4* outRoot) {
  if (!outRoot || !g_FindPlayerPed)
    return false;
  void* ped = g_FindPlayerPed(0);
  if (!ped)
    return false;
  auto* pMat = *reinterpret_cast<Matrix44**>(reinterpret_cast<uint8_t*>(ped) + 0x20);
  if (pMat) {
    *outRoot = pMat->pos;
  } else {
    const float* t = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(ped) + 0x10);
    outRoot->x = t[0];
    outRoot->y = t[1];
    outRoot->z = t[2];
    outRoot->w = 1.f;
  }
  return SaneWorldPos(*outRoot);
}

bool TryReadPedBasis(Vec4* outRoot, float* fwdX, float* fwdY, float* fwdZ, float* upX,
                     float* upY, float* upZ) {
  if (!outRoot || !g_FindPlayerPed)
    return false;
  void* ped = g_FindPlayerPed(0);
  if (!ped)
    return false;
  auto* pMat = *reinterpret_cast<Matrix44**>(reinterpret_cast<uint8_t*>(ped) + 0x20);
  if (!pMat) {
    if (!TryReadPedRoot(outRoot))
      return false;
    if (fwdX) {
      *fwdX = 0.f;
      *fwdY = 1.f;
      *fwdZ = 0.f;
    }
    if (upX) {
      *upX = 0.f;
      *upY = 0.f;
      *upZ = 1.f;
    }
    return true;
  }
  *outRoot = pMat->pos;
  if (!SaneWorldPos(*outRoot))
    return false;
  // RAGE: up = forward, at = world-up.
  if (fwdX) {
    float fx = pMat->up.x, fy = pMat->up.y, fz = pMat->up.z;
    const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (len > 1e-4f) {
      *fwdX = fx / len;
      *fwdY = fy / len;
      *fwdZ = fz / len;
    } else {
      *fwdX = 0.f;
      *fwdY = 1.f;
      *fwdZ = 0.f;
    }
  }
  if (upX) {
    float ux = pMat->at.x, uy = pMat->at.y, uz = pMat->at.z;
    const float len = std::sqrt(ux * ux + uy * uy + uz * uz);
    if (len > 1e-4f) {
      *upX = ux / len;
      *upY = uy / len;
      *upZ = uz / len;
    } else {
      *upX = 0.f;
      *upY = 0.f;
      *upZ = 1.f;
    }
  }
  return true;
}

bool TryGetDeferredHeadBoneEyeCached(Vec4* outEye) {
  if (!outEye || !g_haveDeferredWorldEye)
    return false;
  Vec4 root{};
  if (TryReadPedRoot(&root)) {
    const float dx = root.x - g_deferredBoneRoot.x;
    const float dy = root.y - g_deferredBoneRoot.y;
    const float dz = root.z - g_deferredBoneRoot.z;
    if (dx * dx + dy * dy + dz * dz >= 4.f) {
      g_haveDeferredWorldEye = false;
      g_haveDeferredBoneEye = false;
      g_haveDeferredSmoothOff = false;
      g_haveDeferredSmoothRootZ = false;
      return false;
    }
  }
  // Dual L/R share the eye frozen at sample time (before HookDrawWalk dual).
  *outEye = g_deferredWorldEye;
  outEye->w = 1.f;
  return SaneWorldPos(*outEye);
}

// Fast catch-up on big errors (stairs / crouch), heavy damp on small bob.
float AdaptiveFollowAlpha(float errAbs, float aBob, float aMove, float bobThresh,
                          float moveThresh) {
  if (!(errAbs >= 0.f))
    return aBob;
  if (errAbs <= bobThresh)
    return aBob;
  if (errAbs >= moveThresh)
    return aMove;
  const float t = (errAbs - bobThresh) / (moveThresh - bobThresh);
  return aBob + t * (aMove - aBob);
}

// Ped-fixed eye-center (Mode 155+) BEFORE 6DoF / camoff / IPD.
bool TryGetPedEyeCenterBase(Vec4* outEye) {
  if (!g_FindPlayerPed || !outEye)
    return false;
  // Mode 234: first computed eye of the pair wins (plan §4.5 pair latch).
  if (g_sameStateLatch.load() && g_haveSameStatePedEye) {
    *outEye = g_sameStatePedEye;
    outEye->w = 1.f;
    return SaneWorldPos(*outEye);
  }
  void* ped = g_FindPlayerPed(0);
  if (!ped)
    return false;
  const StereoMode sm = GetStereoMode();
  const bool inVeh = IsPlayerInVehicleSticky();

  auto storeSameState = [&]() {
    if (g_sameStateLatch.load() && !g_haveSameStatePedEye && SaneWorldPos(*outEye)) {
      g_sameStatePedEye = *outEye;
      g_haveSameStatePedEye = true;
    }
  };

  // Mode 216: always model-centered HEAD (foot + car + enter/exit soft scenes).
  // Never skip for vehicle — keep bone pivot through jack/enter/exit when ped exists.
  if (IsOursFpDeferredHeadBone(sm)) {
    if (TryGetDeferredHeadBoneEyeCached(outEye)) {
      storeSameState();
      return true;
    }
    // Height fallback: root + eyeH only (no foot/car forward nudge).
    const float kEyeH = 0.65f;
    auto* pMat = *reinterpret_cast<Matrix44**>(reinterpret_cast<uint8_t*>(ped) + 0x20);
    if (!pMat) {
      const float* t = reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(ped) + 0x10);
      outEye->x = t[0];
      outEye->y = t[1];
      outEye->z = t[2] + kEyeH;
      outEye->w = 1.f;
    } else {
      outEye->x = pMat->pos.x + pMat->at.x * kEyeH;
      outEye->y = pMat->pos.y + pMat->at.y * kEyeH;
      outEye->z = pMat->pos.z + pMat->at.z * kEyeH;
      outEye->w = 1.f;
    }
    const bool ok = SaneWorldPos(*outEye);
    if (ok)
      storeSameState();
    return ok;
  }

  // Mode 193: stick to HEAD bone via CE thiscall (crouch/sprint) — mid-draw path.
  if (IsOursFpHeadBoneCam(sm) && !inVeh) {
    if (TryGetPedHeadBoneEye(outEye)) {
      storeSameState();
      return true;
    }
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
  const bool ok = SaneWorldPos(*outEye);
  if (ok)
    storeSameState();
  return ok;
}

void ApplyHmdToCam(Matrix44* mat) {
  if (!g_gameplayActive.load())
    return;

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

  const StereoMode sm = GetStereoMode();
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
  // Mode 234: latched yaw for both eyes of the dual pair (§4.5).
  {
    const float yawOff = g_sameStateLatch.load()
                             ? g_sameStateYawOff
                             : (GetControllerYawOffset() + ComputeVehicleYawOffset());
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

  const float eyeFwd =
      (eyeCenter || IsOursFpDeferredHeadBone(sm)) ? 0.f : GetEyeForwardMeters();
  eye.x += fx * eyeFwd;
  eye.y += fy * eyeFwd;
  eye.z += fz * eyeFwd;

  // Inspiration FirstPerson.ini FPX/FPY/FPZ (our units: cm via gtaiv_dxvk_vr.camoff).
  // Mode216: model-centered only — ignore camoff + vehcamoff (foot/car nudges OFF).
  float offR = 0.f, offF = 0.f, offU = 0.f;
  if (!IsOursFpDeferredHeadBone(sm)) {
    GetCamOffsetMeters(&offR, &offF, &offU);
    if (IsOursFpInCarHead(sm) && IsPlayerInVehicleSticky()) {
      float vr = 0.f, vf = 0.f, vu = 0.f;
      GetVehicleCamOffsetMeters(&vr, &vf, &vu);
      offR += vr;
      offF += vf;
      offU += vu;
    }
  }
  if (offR != 0.f || offF != 0.f || offU != 0.f) {
    eye.x += rx * offR + fx * offF + ux * offU;
    eye.y += ry * offR + fy * offF + uy * offU;
    eye.z += rz * offR + fz * offF + uz * offU;
  }

  // Inspiration-style head/hair hide. Mode216: once per deferred sample (not every
  // CopyMat) — hide thrash on the 204 dual path looked like model spazz.
  if (!IsOursFpDeferredHeadBone(sm))
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

  // Stereo eye origin.
  // Default/L4D2: HMD-right × ±half + optional EyeZ (stick-yaw ≠ HMD-right → dual pivot).
  // Mode 238 UEVR/praydog: EyeToHead in CAMERA local frame AFTER stick yaw.
  float ipdX = 0.f, ipdY = 0.f, ipdZ = 0.f;
  const bool rightEye = (GetStereoEye() == StereoEye::Right);
  // Mode 172 mono-pair: center cam only (no ±IPD) — one DrawScene feeds both eyes.
  // Mode 271 AER: g_camCenterOverride leaves the camera at the true head
  // CENTRE, no eye offset. Used to park the live camera between the eyes
  // after an AER frame — the game reads that same matrix for its aim
  // raycast, so leaving it on one eye put the shot half an IPD off to the
  // side ("the gun shoots slightly right of where it points").
  if (GetStereoMode() >= StereoMode::DualIpd && !IsOursFpMonoPair(GetStereoMode()) &&
      !g_camCenterOverride.load(std::memory_order_relaxed)) {
    const float ws =
        IsOursFpTrueWorldScale(GetStereoMode()) ? GetWorldScale() : 1.f;
    float half = 0.5f * GetStereoSepMeters() * GetStereoScale() * ws;

    if (UsesCamRightIpd(GetStereoMode())) {
      // Parallel stereo: ±half IPD on post-yaw CAMERA right only (follows stick).
      // Flip: Mode 240 TrueStereo / Mode 241 stock-203 (inverted sign fixed fuse).
      float s = rightEye ? half : -half;
      if (UsesCamRightIpdFlip(GetStereoMode()))
        s = -s;
      ipdX = rx * s;
      ipdY = ry * s;
      ipdZ = rz * s;
      static bool s_camRLogged = false;
      if (!s_camRLogged) {
        s_camRLogged = true;
        Log("Mode%d: CAM-RIGHT-IPD %shalf=%.2fcm on post-yaw camera right eye=%s "
            "(no HMD-right, no EyeToHead)",
            static_cast<int>(GetStereoMode()),
            UsesCamRightIpdFlip(GetStereoMode()) ? "FLIP " : "", half * 100.f,
            rightEye ? "R" : "L");
      }
    } else if (IsTrueStereoUevrIpd(GetStereoMode()) && vr::VRSystem()) {
      // OpenVR EyeToHead: +X right, +Y up, +Z back. Apply on post-yaw cam basis
      // (RAGE: right / forward=mat.up / up=mat.at) — matches UEVR view_rotation * eye_offset.
      const vr::HmdMatrix34_t e2h =
          vr::VRSystem()->GetEyeToHeadTransform(rightEye ? vr::Eye_Right : vr::Eye_Left);
      const float localR = e2h.m[0][3];
      const float localU = e2h.m[1][3];
      const float localB = e2h.m[2][3];
      const float scale = ws * GetStereoScale();
      ipdX = (rx * localR + ux * localU + fx * (-localB)) * scale;
      ipdY = (ry * localR + uy * localU + fy * (-localB)) * scale;
      ipdZ = (rz * localR + uz * localU + fz * (-localB)) * scale;
      half = 0.5f * std::sqrt(localR * localR + localU * localU + localB * localB) * scale;
      static bool s_uevrLogged = false;
      if (!s_uevrLogged) {
        s_uevrLogged = true;
        Log("Mode238: UEVR-IPD EyeToHead in CAMERA frame local=(%.4f,%.4f,%.4f) eye=%s "
            "scale=%.2f (no HMD-right, no L4D2 EyeZ)",
            localR, localU, localB, rightEye ? "R" : "L", scale);
      }
    } else {
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

      // Optional EyeToHead forward (OpenVR col2) like L4D2 m_EyeZ. Mode 237: force 0.
      float eyeZ = 0.f;
      if (!IsTrueStereoLateralIpd(GetStereoMode()) && vr::VRSystem()) {
        const vr::HmdMatrix34_t e2h =
            vr::VRSystem()->GetEyeToHeadTransform(rightEye ? vr::Eye_Right : vr::Eye_Left);
        eyeZ = e2h.m[2][3];
      }

      const float s = rightEye ? half : -half;
      const float fzOff = -eyeZ * ws;
      ipdX = hrx * s + fx * fzOff;
      ipdY = hry * s + fy * fzOff;
      ipdZ = hrz * s + fz * fzOff;
      static bool s_latLogged = false;
      if (IsTrueStereoLateralIpd(GetStereoMode()) && !s_latLogged) {
        s_latLogged = true;
        Log("Mode237: EyeZ forward forced 0 (lateral IPD only) half=%.2fcm eye=%s", half * 100.f,
            rightEye ? "R" : "L");
      }
    }
    if (std::isfinite(ipdX) && std::isfinite(ipdY) && std::isfinite(ipdZ) &&
        std::fabs(ipdX) < 20.f && std::fabs(ipdY) < 20.f && std::fabs(ipdZ) < 20.f) {
      eye.x += ipdX;
      eye.y += ipdY;
      eye.z += ipdZ;
    } else {
      ipdX = ipdY = ipdZ = 0.f;
    }

    // Mode 210/211/215: toe-in so optical axes meet ahead (near fusion try).
    // 210/215 ≈ 1.5m; 211 stronger ≈ 0.85m (dash/hands).
    if (IsOursFpSameTickParentDual203ToeIn(GetStereoMode()) && half > 1e-5f) {
      const float convM =
          IsOursFpSameTickParentDual203ToeInStrong(GetStereoMode()) ? 0.85f : 1.5f;
      float ang = std::atan2(half, convM);
      if (!rightEye)
        ang = -ang;
      const float ca = std::cos(ang);
      const float sa = std::sin(ang);
      // Rotate around up (ux,uy,uz): v' = v*c + (u×v)*s + u*(u·v)*(1-c)
      auto rotU = [&](float vx, float vy, float vz, float* ox, float* oy, float* oz) {
        const float dot = ux * vx + uy * vy + uz * vz;
        const float cxv_x = uy * vz - uz * vy;
        const float cxv_y = uz * vx - ux * vz;
        const float cxv_z = ux * vy - uy * vx;
        *ox = vx * ca + cxv_x * sa + ux * dot * (1.f - ca);
        *oy = vy * ca + cxv_y * sa + uy * dot * (1.f - ca);
        *oz = vz * ca + cxv_z * sa + uz * dot * (1.f - ca);
      };
      float nrx, nry, nrz, nfx, nfy, nfz;
      rotU(rx, ry, rz, &nrx, &nry, &nrz);
      rotU(fx, fy, fz, &nfx, &nfy, &nfz);
      rx = nrx;
      ry = nry;
      rz = nrz;
      fx = nfx;
      fy = nfy;
      fz = nfz;
      static uint32_t s_toeN = 0;
      const uint32_t tn = ++s_toeN;
      if (tn <= 4 || (tn % 600) == 0)
        Log("Mode%d: toe-in #%u %s ang=%.3fdeg half=%.3fcm conv=%.2fm",
            static_cast<int>(GetStereoMode()), tn, rightEye ? "R" : "L",
            ang * (180.f / 3.14159265f), half * 100.f, convM);
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
  // Mode 271 AER: remember the basis this eye was actually RENDERED with, not
  // the HMD pose it came from — see g_lastBasisR/F/U above.
  g_lastBasisR[0] = rx; g_lastBasisR[1] = ry; g_lastBasisR[2] = rz;
  g_lastBasisF[0] = fx; g_lastBasisF[1] = fy; g_lastBasisF[2] = fz;
  g_lastBasisU[0] = ux; g_lastBasisU[1] = uy; g_lastBasisU[2] = uz;
  g_haveLastBasis = true;

  const uint32_t n = ++g_applyCount;
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

void SampleDeferredHeadBoneEye() {
  if (!IsOursFpDeferredHeadBone(GetStereoMode()))
    return;
  if (!g_gameplayActive.load())
    return;
  // Keep sampling in vehicles / enter-exit — model-centered through soft scenes.
  if (g_boneThiscallDead)
    return;
  // Never sample mid-DrawScene / mid parent dual (Mode193 crash surface).
  if (StereoInDualPass() || StereoInParentDualWalk())
    return;
  if (StereoMode193SkipDrawActive())
    return;
  // Warm gate: first duals after load use height only (bone too early = crash).
  if (!StereoParentDualReadyForBone())
    return;

  static bool s_loggedFirst = false;
  if (!s_loggedFirst) {
    s_loggedFirst = true;
    Log("CamMatrix: deferred HEAD first sample (warm dual ready) — attempt GetBonePos");
  }

  Vec4 eye{};
  if (TryGetPedHeadBoneEye(&eye)) {
    Vec4 root{};
    float fwdX = 0.f, fwdY = 1.f, fwdZ = 0.f;
    float upX = 0.f, upY = 0.f, upZ = 1.f;
    if (!TryReadPedBasis(&root, &fwdX, &fwdY, &fwdZ, &upX, &upY, &upZ)) {
      if (!TryReadPedRoot(&root))
        root = g_stickyBoneRoot;
    }

    // Bone offset from root (posture / crouch lives here; walk bob too).
    float ox = eye.x - root.x;
    float oy = eye.y - root.y;
    float oz = eye.z - root.z;

    // Adaptive: walk/run bob (~2–8cm) heavily damped; stairs/crouch catch up fast.
    // Prior bobA~0.08–0.12 still tracked walk and shook the world.
    constexpr float kOffBobA = 0.025f;
    constexpr float kOffMoveA = 0.80f;
    constexpr float kOffBobT = 0.08f;   // treat ≤8cm as bob
    constexpr float kOffMoveT = 0.22f;  // ≥22cm = posture/stair stick
    if (!g_haveDeferredSmoothOff) {
      g_deferredSmoothOff.x = ox;
      g_deferredSmoothOff.y = oy;
      g_deferredSmoothOff.z = oz;
      g_haveDeferredSmoothOff = true;
    } else {
      const float axy = AdaptiveFollowAlpha(
          std::sqrt((ox - g_deferredSmoothOff.x) * (ox - g_deferredSmoothOff.x) +
                    (oy - g_deferredSmoothOff.y) * (oy - g_deferredSmoothOff.y)),
          kOffBobA, kOffMoveA, kOffBobT, kOffMoveT);
      const float az = AdaptiveFollowAlpha(std::fabs(oz - g_deferredSmoothOff.z), kOffBobA,
                                           kOffMoveA, kOffBobT, kOffMoveT);
      g_deferredSmoothOff.x += axy * (ox - g_deferredSmoothOff.x);
      g_deferredSmoothOff.y += axy * (oy - g_deferredSmoothOff.y);
      g_deferredSmoothOff.z += az * (oz - g_deferredSmoothOff.z);
    }

    // Smooth root XYZ — live root.xy every sample was injecting walk sway into the view.
    constexpr float kRootBobA = 0.03f;
    constexpr float kRootMoveA = 0.92f;
    constexpr float kRootBobT = 0.08f;
    constexpr float kRootMoveT = 0.20f;
    if (!g_haveDeferredSmoothRootZ) {
      g_deferredSmoothRootX = root.x;
      g_deferredSmoothRootY = root.y;
      g_deferredSmoothRootZ = root.z;
      g_haveDeferredSmoothRootZ = true;
    } else {
      const float dxy = std::sqrt((root.x - g_deferredSmoothRootX) * (root.x - g_deferredSmoothRootX) +
                                 (root.y - g_deferredSmoothRootY) * (root.y - g_deferredSmoothRootY));
      const float axy =
          AdaptiveFollowAlpha(dxy, kRootBobA, kRootMoveA, kRootBobT, kRootMoveT);
      const float az = AdaptiveFollowAlpha(std::fabs(root.z - g_deferredSmoothRootZ),
                                           kRootBobA, kRootMoveA, kRootBobT, kRootMoveT);
      g_deferredSmoothRootX += axy * (root.x - g_deferredSmoothRootX);
      g_deferredSmoothRootY += axy * (root.y - g_deferredSmoothRootY);
      g_deferredSmoothRootZ += az * (root.z - g_deferredSmoothRootZ);
    }

    // Between the eyes: +6cm up, +3cm forward (no back — vehicles looked wrong).
    constexpr float kUpM = 0.06f;
    constexpr float kFwdM = 0.03f;
    g_deferredWorldEye.x =
        g_deferredSmoothRootX + g_deferredSmoothOff.x + upX * kUpM + fwdX * kFwdM;
    g_deferredWorldEye.y =
        g_deferredSmoothRootY + g_deferredSmoothOff.y + upY * kUpM + fwdY * kFwdM;
    g_deferredWorldEye.z =
        g_deferredSmoothRootZ + g_deferredSmoothOff.z + upZ * kUpM + fwdZ * kFwdM;
    g_deferredWorldEye.w = 1.f;
    g_haveDeferredWorldEye = true;

    g_deferredBoneEye = g_deferredWorldEye;
    g_deferredBoneRoot = root;
    g_haveDeferredBoneEye = true;
    g_deferredBoneTick = GetTickCount();

    // One hide pulse per sample (Mode216) — avoids CopyMat×hide thrash on 204 dual.
    UpdatePedHeadHide();

    const uint32_t n = ++g_deferredSampleOk;
    if (n <= 6 || (n % 600) == 0) {
      Log("CamMatrix: deferred HEAD #%u rootZ=%.3f smRootZ=%.3f offZ=%.3f eyeZ=%.3f "
          "(bob≤8cm damp; stairs/crouch stick; +3cm fwd)",
          n, root.z, g_deferredSmoothRootZ, g_deferredSmoothOff.z, g_deferredWorldEye.z);
    }
    return;
  }

  // Keep last world eye if root still nearby; else clear for height fallback.
  Vec4 root{};
  if (g_haveDeferredWorldEye && TryReadPedRoot(&root)) {
    const float dx = root.x - g_deferredBoneRoot.x;
    const float dy = root.y - g_deferredBoneRoot.y;
    const float dz = root.z - g_deferredBoneRoot.z;
    if (dx * dx + dy * dy + dz * dz < 4.f) {
      g_deferredBoneRoot = root;
      g_deferredBoneTick = GetTickCount();
      return;
    }
  }
  g_haveDeferredBoneEye = false;
  g_haveDeferredWorldEye = false;
  g_haveDeferredSmoothOff = false;
  g_haveDeferredSmoothRootZ = false;
  const uint32_t n = ++g_deferredSampleFail;
  if (n <= 4 || (n % 300) == 0)
    Log("CamMatrix: deferred HEAD miss #%u — height fallback next bake", n);
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

// Mode 271 AER: same as RefreshLiveCamForStereoEye(), but with NO eye offset —
// parks the live camera at the true head centre. Call this instead when
// handing the camera back to the game after an AER frame: the game aims,
// raycasts and places the HUD from that same matrix, so leaving it on one eye
// biases every shot half an IPD to one side.
void RefreshLiveCamCentered() {
  g_camCenterOverride.store(true, std::memory_order_relaxed);
  RefreshLiveCamForStereoEye();
  g_camCenterOverride.store(false, std::memory_order_relaxed);
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

void BeginTrueStereoSameStateLatch() {
  if (!IsTrueStereoSameState(GetStereoMode()))
    return;
  g_sameStateYawOff = GetControllerYawOffset() + ComputeVehicleYawOffset();
  g_haveSameStatePedEye = false;
  g_sameStateLatch.store(true);
  static uint32_t s_n = 0;
  const uint32_t n = ++s_n;
  if (n <= 6 || (n % 600) == 0)
    Log("Mode234: same-state latch begin #%u yawOff=%.4f", n, g_sameStateYawOff);
}

void EndTrueStereoSameStateLatch() {
  g_sameStateLatch.store(false);
  g_haveSameStatePedEye = false;
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

// Mode 271 AER: any out pointer may be null. False until the first bake.
bool GetLastStereoCamBasis(float outR[3], float outF[3], float outU[3], float outPos[3]) {
  if (!g_haveLastBasis || !g_haveLastApplied)
    return false;
  if (outR) {
    outR[0] = g_lastBasisR[0];
    outR[1] = g_lastBasisR[1];
    outR[2] = g_lastBasisR[2];
  }
  if (outF) {
    outF[0] = g_lastBasisF[0];
    outF[1] = g_lastBasisF[1];
    outF[2] = g_lastBasisF[2];
  }
  if (outU) {
    outU[0] = g_lastBasisU[0];
    outU[1] = g_lastBasisU[1];
    outU[2] = g_lastBasisU[2];
  }
  if (outPos) {
    outPos[0] = g_lastAppliedPos.x;
    outPos[1] = g_lastAppliedPos.y;
    outPos[2] = g_lastAppliedPos.z;
  }
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
      // Mode 231: do NOT under-publish / cover-lock. Canvas tangents come from the
      // live StereoCalib measurement; until the first sample, fall back to kEng.
      if (IsTrueStereoExact(sm)) {
        if (!StereoCalibHasMeasurement()) {
          PublishGameFovFromCCamDegrees(after, GetBackbufferAspect());
          static bool s_kengFb = false;
          if (!s_kengFb) {
            s_kengFb = true;
            Log("Mode231: gameTan source=kEng FALLBACK (waiting for StereoCalib measure)");
          }
        }
      } else if (IsOursFpWideAspectFit(sm))
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
        IsCleanDualLookMove(sm) || IsHeadHideNativeOurs(sm)) {
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

