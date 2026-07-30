#include "aim_decouple.h"
#include "cam_matrix.h"
#include "hmd_pose.h"
#include "ik_aim.h"
#include "ik_copy.h"
#include "ik_ray.h"
#include "ik_pin.h"
#include "ik_read.h"
#include "log.h"
#include "native_invoke.h"
#include "vr_pad.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace asi {
namespace {

// Same OpenVR→GTA axis mapping as cam_matrix.cpp:100 (x, -z, y).
void OvrToGta(float ox, float oy, float oz, float* gx, float* gy, float* gz) {
  *gx = ox;
  *gy = -oz;
  *gz = oy;
}

bool Normalize3(float* x, float* y, float* z) {
  const float len = std::sqrt((*x) * (*x) + (*y) * (*y) + (*z) * (*z));
  if (len < 1e-6f)
    return false;
  *x /= len;
  *y /= len;
  *z /= len;
  return true;
}

int ReadIntFileOnce(const char* fileName, int defValue, int minV, int maxV) {
  char path[MAX_PATH]{};
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&ReadIntFileOnce), &self))
    return defValue;
  if (!GetModuleFileNameA(self, path, MAX_PATH))
    return defValue;
  char* slash = strrchr(path, '\\');
  if (!slash)
    slash = strrchr(path, '/');
  if (!slash)
    return defValue;
  slash[1] = 0;
  strcat_s(path, fileName);
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return defValue;
  char buf[16]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return defValue;
  int v = 0;
  if (sscanf_s(buf, "%d", &v) != 1)
    return defValue;
  if (v < minV || v > maxV)
    return defValue;
  return v;
}

std::mutex g_mu;
vr::HmdMatrix34_t g_ctrlMat{};
vr::TrackedDeviceIndex_t g_ctrlIdx = vr::k_unTrackedDeviceIndexInvalid;
std::atomic<bool> g_ctrlValid{false};
std::atomic<bool> g_gripHeld{false};
std::atomic<uint32_t> g_probeN{0};
DWORD g_lastProbeTick = 0;
DWORD g_lastAimTick = 0;
DWORD g_lastShootTick = 0;
bool g_shootWasHeld = false;
std::atomic<bool> g_nativesLogged{false};
std::atomic<bool> g_mouseShootDown{false};
std::atomic<bool> g_fireNativesDead{false};

// G2 / WMR: OpenVR device pose is grip, not tip/aim. Tip sits ~55° pitched
// from grip −Z toward local +Y (OpenXR /input/aim/pose vs /input/grip/pose).
constexpr float kG2TipPitchDeg = 55.f;

void* g_fnAimAtCoord = nullptr;
void* g_fnShootAtCoord = nullptr;
void* g_fnFireBullet = nullptr;
void* g_fnFirePedWeapon = nullptr;
void* g_fnClearTasks = nullptr;
void* g_fnGetPlayerChar = nullptr;
void* g_fnIsCharInAnyCar = nullptr;
void* g_fnGiveWeapon = nullptr;
void* g_fnGiveDelayedWeapon = nullptr;
void* g_fnSetCurrentWeapon = nullptr;
void* g_fnGetBonePos = nullptr;
void* g_fnGetHeldObject = nullptr;  // diagnostic only — IV guns are NOT held objects
void* g_fnDoesObjectExist = nullptr;
void* g_fnDetachObject = nullptr;
void* g_fnSetObjectCoords = nullptr;
void* g_fnSetObjectRotation = nullptr;
void* g_fnGetObjectCoords = nullptr;
void* g_fnFreezeObject = nullptr;
void* g_fnSetObjectCollision = nullptr;
void* g_fnGetCurrentWeapon = nullptr;
void* g_fnGetWeapontypeModel = nullptr;
void* g_fnRequestModel = nullptr;
void* g_fnHasModelLoaded = nullptr;
void* g_fnMarkModel = nullptr;
void* g_fnCreateObject = nullptr;
void* g_fnDeleteObject = nullptr;
void* g_fnSetWeaponVisible = nullptr;
std::atomic<bool> g_handlersReady{false};
std::atomic<bool> g_gunsDone{false};
std::atomic<bool> g_taskAimDead{false};
DWORD g_lastHandProbeTick = 0;
uint32_t g_weapGhostObj = 0;
uint32_t g_weapGhostWeapon = 0;
uint32_t g_weapGhostModel = 0;
bool g_weapStockHidden = false;
bool g_weapHeldDiagDone = false;
std::atomic<bool> g_weapFollowDead{false};

// CPed::m_pVehicle — same as look_move/vr_move (CE).
constexpr uint32_t kPedVehicleOff = 0xB30;
constexpr float kAimRayMeters = 25.f;
constexpr DWORD kAimIntervalMs = 200;
constexpr int kAimDurationMs = 250;
constexpr DWORD kFireIntervalMs = 100;
constexpr DWORD kHandProbeIntervalMs = 2000;  // rare; weapon path is the visual
constexpr bool kUseTaskAim = false;
constexpr int kHandFollowDefault = 0;   // ped bone WRITE hard-off forever for now
constexpr int kWeapFollowDefault = 0;   // HARD-OFF: CREATE_OBJECT ghost froze/crash (003848)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// GTAMods Ped_Bones (IV) — HEAD matches cam_matrix kBoneHead.
constexpr uint32_t kBoneHead = 0x4B5;
constexpr uint32_t kBoneRHand = 0x4D0;
constexpr uint32_t kBoneRForearm = 0x4C9;
constexpr uint32_t kBoneRUpperArm = 0x4C8;
constexpr uint32_t kBoneLHand = 0x4C3;

void LogAimNativeHandlersOnce() {
  if (g_nativesLogged.exchange(true))
    return;
  static const char* kNames[] = {
      "TASK_AIM_GUN_AT_COORD",
      "TASK_SHOOT_AT_COORD",
      "FIRE_SINGLE_BULLET",
      "GET_PED_BONE_POSITION",
      "FIRE_PED_WEAPON",
      "CLEAR_CHAR_TASKS",
      "GET_PLAYER_CHAR",
      "IS_CHAR_IN_ANY_CAR",
      "GIVE_WEAPON_TO_CHAR",
      "GIVE_DELAYED_WEAPON_TO_CHAR",
      "SET_CURRENT_CHAR_WEAPON",
      "GET_OBJECT_PED_IS_HOLDING",
      "DOES_OBJECT_EXIST",
      "DETACH_OBJECT",
      "SET_OBJECT_COORDINATES",
      "SET_OBJECT_ROTATION",
      "GET_OBJECT_COORDINATES",
      "FREEZE_OBJECT_POSITION",
      "SET_OBJECT_COLLISION",
      "GET_CURRENT_CHAR_WEAPON",
      "GET_WEAPONTYPE_MODEL",
      "REQUEST_MODEL",
      "HAS_MODEL_LOADED",
      "MARK_MODEL_AS_NO_LONGER_NEEDED",
      "CREATE_OBJECT",
      "DELETE_OBJECT",
      "SET_CHAR_CURRENT_WEAPON_VISIBLE",
  };
  int ok = 0;
  for (const char* name : kNames) {
    void* h = GetNativeHandlerByName(name);
    if (h) {
      ++ok;
      Log("AimNative: %s OK @ %p", name, h);
    } else {
      Log("AimNative: %s MISS", name);
    }
  }
  g_fnAimAtCoord = GetNativeHandlerByName("TASK_AIM_GUN_AT_COORD");
  g_fnShootAtCoord = GetNativeHandlerByName("TASK_SHOOT_AT_COORD");
  g_fnFireBullet = GetNativeHandlerByName("FIRE_SINGLE_BULLET");
  g_fnFirePedWeapon = GetNativeHandlerByName("FIRE_PED_WEAPON");
  g_fnClearTasks = GetNativeHandlerByName("CLEAR_CHAR_TASKS");
  g_fnGetPlayerChar = GetNativeHandlerByName("GET_PLAYER_CHAR");
  g_fnIsCharInAnyCar = GetNativeHandlerByName("IS_CHAR_IN_ANY_CAR");
  g_fnGiveWeapon = GetNativeHandlerByName("GIVE_WEAPON_TO_CHAR");
  g_fnGiveDelayedWeapon = GetNativeHandlerByName("GIVE_DELAYED_WEAPON_TO_CHAR");
  g_fnSetCurrentWeapon = GetNativeHandlerByName("SET_CURRENT_CHAR_WEAPON");
  g_fnGetBonePos = GetNativeHandlerByName("GET_PED_BONE_POSITION");
  g_fnGetHeldObject = GetNativeHandlerByName("GET_OBJECT_PED_IS_HOLDING");
  g_fnDoesObjectExist = GetNativeHandlerByName("DOES_OBJECT_EXIST");
  g_fnDetachObject = GetNativeHandlerByName("DETACH_OBJECT");
  g_fnSetObjectCoords = GetNativeHandlerByName("SET_OBJECT_COORDINATES");
  g_fnSetObjectRotation = GetNativeHandlerByName("SET_OBJECT_ROTATION");
  g_fnGetObjectCoords = GetNativeHandlerByName("GET_OBJECT_COORDINATES");
  g_fnFreezeObject = GetNativeHandlerByName("FREEZE_OBJECT_POSITION");
  g_fnSetObjectCollision = GetNativeHandlerByName("SET_OBJECT_COLLISION");
  g_fnGetCurrentWeapon = GetNativeHandlerByName("GET_CURRENT_CHAR_WEAPON");
  g_fnGetWeapontypeModel = GetNativeHandlerByName("GET_WEAPONTYPE_MODEL");
  g_fnRequestModel = GetNativeHandlerByName("REQUEST_MODEL");
  g_fnHasModelLoaded = GetNativeHandlerByName("HAS_MODEL_LOADED");
  g_fnMarkModel = GetNativeHandlerByName("MARK_MODEL_AS_NO_LONGER_NEEDED");
  g_fnCreateObject = GetNativeHandlerByName("CREATE_OBJECT");
  g_fnDeleteObject = GetNativeHandlerByName("DELETE_OBJECT");
  g_fnSetWeaponVisible = GetNativeHandlerByName("SET_CHAR_CURRENT_WEAPON_VISIBLE");
  g_handlersReady.store(g_fnGetPlayerChar && g_fnFirePedWeapon);
  const bool weapGhostReady = g_fnGetCurrentWeapon && g_fnGetWeapontypeModel && g_fnCreateObject &&
                              g_fnSetObjectCoords && g_fnRequestModel && g_fnHasModelLoaded;
  Log("AimNative: resolve done — %d/%d OK. shootVia=%s (no LMB/FIRE_SINGLE/RT). weapGhost=%s. "
      "kill: aimmode=0",
      ok, static_cast<int>(sizeof(kNames) / sizeof(kNames[0])),
      g_fnFirePedWeapon ? "FIRE_PED_WEAPON" : "NONE",
      weapGhostReady ? "ready" : "MISSING");
}

int ReadHandFollowFile() {
  return ReadIntFileOnce("gtaiv_dxvk_vr.handfollow", kHandFollowDefault, 0, 2);
}

int ReadWeapFollowFile() {
  return ReadIntFileOnce("gtaiv_dxvk_vr.weapfollow", kWeapFollowDefault, 0, 2);
}

int GetWeapFollowMode() {
  static int s_v = -1;
  if (s_v < 0) {
    (void)ReadWeapFollowFile();  // file ignored while HARD-OFF
    s_v = 0;
    Log("AimDecouple: weapfollow HARD-OFF (CREATE_OBJECT ghost disabled after "
        "003848 freeze/crash). Ped bone write OFF. file ignored until safer path");
  }
  return s_v;
}

int GetHandFollowMode() {
  static int s_v = -1;
  if (s_v < 0) {
    s_v = ReadHandFollowFile();
    Log("AimDecouple: handfollow=%d (ped bone WRITE hard-off; future hands). use weapfollow",
        s_v);
  }
  return s_v;
}

bool EstimateControllerWorld(float* outXyz) {
  if (!outXyz)
    return false;
  float camX = 0, camY = 0, camZ = 0;
  if (!GetLastStereoCamPos(&camX, &camY, &camZ))
    return false;
  float cPos[3]{}, hPos[3]{};
  vr::HmdMatrix34_t hm{};
  if (!GetControllerAimPose(cPos, nullptr, nullptr) || !GetHmdPoseMatrix(&hm))
    return false;
  OvrToGta(hm.m[0][3], hm.m[1][3], hm.m[2][3], &hPos[0], &hPos[1], &hPos[2]);
  outXyz[0] = camX + (cPos[0] - hPos[0]);
  outXyz[1] = camY + (cPos[1] - hPos[1]);
  outXyz[2] = camZ + (cPos[2] - hPos[2]);
  return true;
}

bool ObjectExists(uint32_t obj) {
  if (!g_fnDoesObjectExist || obj == 0)
    return false;
  uint32_t result = 0;
  const uint32_t args[] = {obj};
  if (!InvokeNativeFn(g_fnDoesObjectExist, args, 1, &result))
    return false;
  return result != 0;
}

uint32_t GetHeldObject(uint32_t ped) {
  if (!g_fnGetHeldObject)
    return 0;
  uint32_t result = 0;
  const uint32_t args[] = {ped};
  __try {
    if (!InvokeNativeFn(g_fnGetHeldObject, args, 1, &result))
      return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
  return result;
}

bool GetCurrentWeaponType(uint32_t ped, uint32_t* outWeapon) {
  if (!g_fnGetCurrentWeapon || !outWeapon)
    return false;
  uint32_t weapon = 0;
  const uint32_t args[] = {ped, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&weapon))};
  uint32_t ok = 0;
  if (!InvokeNativeFn(g_fnGetCurrentWeapon, args, 2, &ok))
    return false;
  *outWeapon = weapon;
  return true;
}

bool GetWeaponModel(uint32_t weapon, uint32_t* outModel) {
  if (!g_fnGetWeapontypeModel || !outModel || weapon == 0)
    return false;
  uint32_t model = 0;
  // IV dirty ABI: GET_WEAPONTYPE_MODEL(weapon, &model) — not a return hash.
  const uint32_t args[] = {weapon, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&model))};
  if (!InvokeNativeFn(g_fnGetWeapontypeModel, args, 2, nullptr))
    return false;
  if (model == 0)
    return false;
  *outModel = model;
  return true;
}

bool ModelLoaded(uint32_t model) {
  if (!g_fnHasModelLoaded)
    return false;
  uint32_t result = 0;
  const uint32_t args[] = {model};
  if (!InvokeNativeFn(g_fnHasModelLoaded, args, 1, &result))
    return false;
  return result != 0;
}

void DeleteGhostWeapon() {
  if (g_weapGhostObj == 0)
    return;
  if (g_fnDeleteObject) {
    uint32_t handle = g_weapGhostObj;
    const uint32_t args[] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&handle))};
    InvokeNativeFn(g_fnDeleteObject, args, 1, nullptr);
  }
  Log("WeapFollow: DELETE ghost obj=%u weap=%u", g_weapGhostObj, g_weapGhostWeapon);
  g_weapGhostObj = 0;
  g_weapGhostWeapon = 0;
  g_weapGhostModel = 0;
}

void HideStockWeapon(uint32_t ped, bool hide) {
  if (!g_fnSetWeaponVisible)
    return;
  if (hide && g_weapStockHidden)
    return;
  if (!hide && !g_weapStockHidden)
    return;
  const uint32_t args[] = {ped, hide ? 0u : 1u};
  InvokeNativeFn(g_fnSetWeaponVisible, args, 2, nullptr);
  g_weapStockHidden = hide;
  Log("WeapFollow: stock weapon visible=%d", hide ? 0 : 1);
}

void PoseGhostAtController(uint32_t obj, const float* ctrlW, const float* fwd) {
  {
    const uint32_t args[] = {obj, FloatBits(ctrlW[0]), FloatBits(ctrlW[1]), FloatBits(ctrlW[2])};
    InvokeNativeFn(g_fnSetObjectCoords, args, 4, nullptr);
  }
  if (g_fnSetObjectRotation && fwd) {
    const float pitch = std::asin((std::max)(-1.f, (std::min)(1.f, fwd[2]))) * 180.f /
                        static_cast<float>(M_PI);
    const float heading = std::atan2(-fwd[0], fwd[1]) * 180.f / static_cast<float>(M_PI);
    const uint32_t args[] = {obj, FloatBits(pitch), FloatBits(0.f), FloatBits(heading)};
    InvokeNativeFn(g_fnSetObjectRotation, args, 4, nullptr);
  }
}

// HARD-OFF after build 20260730-003848: CREATE_OBJECT weapon ghost + per-tick
// SET_OBJECT_COORDINATES/ROTATION followed OK (~1700 ticks, weapon swap weap=1→9),
// then freeze+crash with no WeapFollow EXCEPTION line. Ped bone WRITE stays off.
// Safer weapon-at-controller path TBD (not CREATE_OBJECT every session).
void ApplyWeaponFollow(uint32_t /*ped*/) {
  (void)GetWeapFollowMode();  // logs HARD-OFF once; CREATE_OBJECT path not entered
}

void ApplyHandFollow() {
  (void)GetHandFollowMode();  // ped bone write stays hard-off
}

// GET_PED_BONE_POSITION native ABI failed on CE (all FAIL in logs).
// Use PedGetBonePos thiscall (same as Mode216 HEAD) instead.
void ProbeHandBones(uint32_t /*ped*/) {
  const DWORD now = GetTickCount();
  if (now - g_lastHandProbeTick < kHandProbeIntervalMs)
    return;
  g_lastHandProbeTick = now;

  struct Cand {
    uint32_t id;
    const char* name;
  };
  static const Cand kCands[] = {
      {kBoneHead, "HEAD"},     {kBoneRHand, "R_HAND"}, {kBoneRForearm, "R_FOREARM"},
      {kBoneRUpperArm, "R_UPPER"}, {kBoneLHand, "L_HAND"},
  };

  float headPos[3]{};
  const bool okHead = TryGetPedBoneWorldPos(kBoneHead, headPos);

  float camX = 0, camY = 0, camZ = 0;
  GetLastStereoCamPos(&camX, &camY, &camZ);

  // Controller world ≈ cam + (ctrlTrack - hmdTrack) in GTA axes.
  float ctrlW[3] = {camX, camY, camZ};
  bool haveCtrlW = EstimateControllerWorld(ctrlW);

  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = ++s_n;
  for (const Cand& c : kCands) {
    float bp[3]{};
    if (!TryGetPedBoneWorldPos(c.id, bp)) {
      if (n <= 8)
        Log("HandBone #%u: %s=0x%X FAIL(thiscall)", n, c.name, c.id);
      continue;
    }
    float dHead = -1.f;
    if (okHead) {
      const float dx = bp[0] - headPos[0], dy = bp[1] - headPos[1], dz = bp[2] - headPos[2];
      dHead = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    float dCtrl = -1.f;
    if (haveCtrlW) {
      const float dx = bp[0] - ctrlW[0], dy = bp[1] - ctrlW[1], dz = bp[2] - ctrlW[2];
      dCtrl = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (n <= 10 || (n % 8) == 0 || c.id == kBoneRHand)
      Log("HandBone #%u: %s=0x%X pos=(%.2f,%.2f,%.2f) dHead=%.2f dCtrl=%.2f", n, c.name, c.id,
          bp[0], bp[1], bp[2], dHead, dCtrl);
  }
  if (haveCtrlW && (n <= 10 || (n % 8) == 0))
    Log("HandBone #%u: ctrlWorld=(%.2f,%.2f,%.2f) cam=(%.2f,%.2f,%.2f)", n, ctrlW[0], ctrlW[1],
        ctrlW[2], camX, camY, camZ);
}

vr::TrackedDeviceIndex_t FindAimControllerIndex(vr::IVRSystem* sys, uint32_t poseCount,
                                                const vr::TrackedDevicePose_t* poses) {
  const vr::ETrackedControllerRole role = (GetAimHand() == 1)
                                              ? vr::TrackedControllerRole_LeftHand
                                              : vr::TrackedControllerRole_RightHand;
  vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(role);
  if (idx != vr::k_unTrackedDeviceIndexInvalid && idx < poseCount && poses[idx].bPoseIsValid &&
      poses[idx].bDeviceIsConnected)
    return idx;

  // G2 / WMR sometimes leaves roles unassigned — take first valid Controller.
  for (uint32_t i = 0; i < poseCount && i < vr::k_unMaxTrackedDeviceCount; ++i) {
    if (!poses[i].bDeviceIsConnected || !poses[i].bPoseIsValid)
      continue;
    if (sys->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller)
      continue;
    return static_cast<vr::TrackedDeviceIndex_t>(i);
  }
  return vr::k_unTrackedDeviceIndexInvalid;
}

bool ReadAimHeld(vr::IVRSystem* sys, vr::TrackedDeviceIndex_t idx) {
  // Right grip = aim (VrPad). Do not use left trigger / face buttons.
  if (IsVrPadEnabled() && IsVrPadAimHeld())
    return true;
  if (!sys || idx == vr::k_unTrackedDeviceIndexInvalid)
    return false;
  vr::VRControllerState_t st{};
  if (!sys->GetControllerState(idx, &st, sizeof(st)))
    return false;
  return (st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_Grip)) != 0;
}

bool ResolvePlayerChar(uint32_t* outPed) {
  if (!outPed || !g_fnGetPlayerChar)
    return false;
  uint32_t ped = 0;
  const uint32_t args[] = {0u, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ped))};
  if (!InvokeNativeFn(g_fnGetPlayerChar, args, 2, nullptr))
    return false;
  if (ped == 0)
    return false;
  *outPed = ped;
  return true;
}

bool IsCharInAnyCar(uint32_t ped) {
  if (!g_fnIsCharInAnyCar)
    return false;
  const uint32_t args[] = {ped};
  uint32_t result = 0;
  if (!InvokeNativeFn(g_fnIsCharInAnyCar, args, 1, &result))
    return false;
  return result != 0;
}

int ReadGiveGunsFile() {
  return ReadIntFileOnce("gtaiv_dxvk_vr.giveguns", 1, 0, 1);  // default ON for VR test
}

void TryGiveTestWeapons() {
  if (ReadGiveGunsFile() == 0)
    return;
  if (g_gunsDone.load())
    return;

  LogAimNativeHandlersOnce();
  if (!g_fnGetPlayerChar) {
    static std::atomic<bool> s_once{false};
    if (!s_once.exchange(true))
      Log("GiveGuns: GET_PLAYER_CHAR missing");
    return;
  }

  uint32_t ped = 0;
  if (!ResolvePlayerChar(&ped) || ped == 0)
    return;

  // GTA IV weapon type ints (ScriptHook / IV-Network). Ammo generous for testing.
  struct W {
    int id;
    int ammo;
  };
  static const W kLoadout[] = {
      {7, 250},   // pistol
      {9, 120},   // deagle
      {10, 80},   // shotgun
      {12, 300},  // micro uzi
      {13, 300},  // mp5
      {14, 300},  // ak47
      {15, 300},  // m4
      {1, 1},     // baseball bat
  };

  void* give = g_fnGiveDelayedWeapon ? g_fnGiveDelayedWeapon : g_fnGiveWeapon;
  if (!give) {
    static std::atomic<bool> s_miss{false};
    if (!s_miss.exchange(true))
      Log("GiveGuns: GIVE_WEAPON*_TO_CHAR MISS");
    return;
  }

  int ok = 0;
  __try {
    for (const W& w : kLoadout) {
      const uint32_t args[] = {ped, static_cast<uint32_t>(w.id), static_cast<uint32_t>(w.ammo),
                               0u};
      if (InvokeNativeFn(give, args, 4, nullptr))
        ++ok;
    }
    if (g_fnSetCurrentWeapon) {
      const uint32_t eq[] = {ped, 7u, 1u};  // equip pistol
      InvokeNativeFn(g_fnSetCurrentWeapon, eq, 3, nullptr);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log("GiveGuns: EXCEPTION — disabled");
    g_gunsDone.store(true);
    return;
  }

  g_gunsDone.store(true);
  Log("GiveGuns: granted %d weapons to ped=%u (pistol equipped). kill: giveguns=0", ok, ped);
}

}  // namespace

int GetAimMode() {
  static std::atomic<int> s_mode{-1};
  int m = s_mode.load();
  if (m < 0) {
    // max=3: Session 3 controller-origin (disk 3 was silent-0 when max was 2)
    m = ReadIntFileOnce("gtaiv_dxvk_vr.aimmode", 0, 0, 3);
    s_mode.store(m);
    if (m != 0)
      Log("AimDecouple: aimmode=%d (%s) — kill: write 0 to gtaiv_dxvk_vr.aimmode", m,
          m == 1   ? "probe + AimNative resolve"
          : m == 3 ? "controller-origin ray + once-per-frame natives"
                   : "Plan A TASK_AIM_GUN_AT_COORD while grip/trigger held");
  }
  return m;
}

int GetAimHand() {
  static std::atomic<int> s_hand{-1};
  int h = s_hand.load();
  if (h < 0) {
    h = ReadIntFileOnce("gtaiv_dxvk_vr.aimhand", 0, 0, 1);
    s_hand.store(h);
    if (GetAimMode() != 0)
      Log("AimDecouple: aimhand=%d (%s)", h, h == 0 ? "right" : "left");
  }
  return h;
}

void AimDecoupleOnPoses(const vr::TrackedDevicePose_t* poses, uint32_t poseCount) {
  // Always latch controller pose when OpenVR delivers poses — IkRay needs
  // src=ctrl even if aimmode=0 (disk aimmode=3 was clamped to max=2 → silent 0
  // and killed pose latch → 40× src=cam). Plan A natives stay gated on aimmode==2.
  if (!poses || poseCount == 0) {
    g_ctrlValid.store(false);
    g_gripHeld.store(false);
    return;
  }
  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys) {
    g_ctrlValid.store(false);
    g_gripHeld.store(false);
    return;
  }
  const vr::TrackedDeviceIndex_t idx = FindAimControllerIndex(sys, poseCount, poses);
  if (idx == vr::k_unTrackedDeviceIndexInvalid) {
    g_ctrlValid.store(false);
    g_gripHeld.store(false);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_ctrlMat = poses[idx].mDeviceToAbsoluteTracking;
    g_ctrlIdx = idx;
  }
  g_ctrlValid.store(true);
  // Aim hand = configured aimhand; LT aim uses left controller via VrPad.
  g_gripHeld.store(IsVrPadAimHeld() || ReadAimHeld(sys, idx));
}

bool GetControllerAimPose(float* posXyz, float* fwdXyz, float* upXyz) {
  if (!g_ctrlValid.load())
    return false;
  vr::HmdMatrix34_t m{};
  {
    std::lock_guard<std::mutex> lock(g_mu);
    m = g_ctrlMat;
  }
  // Position is tracking-space meters (OvrToGta axes only). World tip =
  // ComputeAimOrigin / EstimateControllerWorld (cam + ctrlTrk − hmdTrk).
  if (posXyz) {
    OvrToGta(m.m[0][3], m.m[1][3], m.m[2][3], &posXyz[0], &posXyz[1], &posXyz[2]);
  }
  if (fwdXyz) {
    // Grip −Z pitched toward local +Y ≈ G2 tip / OpenXR aim pose (not raw −Z).
    const float rad = kG2TipPitchDeg * static_cast<float>(M_PI) / 180.f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float ox = s * m.m[0][1] + c * (-m.m[0][2]);
    const float oy = s * m.m[1][1] + c * (-m.m[1][2]);
    const float oz = s * m.m[2][1] + c * (-m.m[2][2]);
    OvrToGta(ox, oy, oz, &fwdXyz[0], &fwdXyz[1], &fwdXyz[2]);
    if (!Normalize3(&fwdXyz[0], &fwdXyz[1], &fwdXyz[2]))
      return false;
  }
  if (upXyz) {
    OvrToGta(m.m[0][1], m.m[1][1], m.m[2][1], &upXyz[0], &upXyz[1], &upXyz[2]);
    Normalize3(&upXyz[0], &upXyz[1], &upXyz[2]);
  }
  return true;
}

bool ResolveWeaponOrHandMatrix(float* outMat16RowMajor) {
  (void)outMat16RowMajor;
  static std::atomic<bool> s_logged{false};
  if (GetAimMode() != 0 && !s_logged.exchange(true))
    Log("AimDecouple: ResolveWeaponOrHandMatrix STUB — needs home RE (bone/weapon matrix)");
  return false;
}

bool ComputeAimForward(float* fwdXyz) {
  if (!fwdXyz)
    return false;
  if (GetControllerAimPose(nullptr, fwdXyz, nullptr))
    return true;
  vr::HmdMatrix34_t h{};
  if (!GetHmdPoseMatrix(&h))
    return false;
  OvrToGta(-h.m[0][2], -h.m[1][2], -h.m[2][2], &fwdXyz[0], &fwdXyz[1], &fwdXyz[2]);
  return Normalize3(&fwdXyz[0], &fwdXyz[1], &fwdXyz[2]);
}

void SetMouseShoot(bool down) {
  const bool was = g_mouseShootDown.exchange(down);
  if (was == down)
    return;
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
  SendInput(1, &in, sizeof(INPUT));
}

// Game-world origin for aim / IkRay. Controller tip = cam + (ctrlTrk − hmdTrk)
// after OvrToGta — same world space as stereo cam / C20. Never return raw
// tracking-space meters as a "world" point (that made IkRay ref Z≈42 vs cam≈18).
bool ComputeAimOrigin(float* ox, float* oy, float* oz, bool* fromCtrl) {
  if (!ox || !oy || !oz)
    return false;
  if (fromCtrl)
    *fromCtrl = false;
  float ctrlW[3]{};
  if (EstimateControllerWorld(ctrlW)) {
    *ox = ctrlW[0];
    *oy = ctrlW[1];
    *oz = ctrlW[2];
    if (fromCtrl)
      *fromCtrl = true;
    return true;
  }
  float camX = 0.f, camY = 0.f, camZ = 0.f;
  if (!GetLastStereoCamPos(&camX, &camY, &camZ))
    return false;
  *ox = camX;
  *oy = camY;
  *oz = camZ;
  return true;
}

// Full 3D controller forward (keep pitch). Horizon-flatten (fwd[2]=0 / az=camZ)
// made FIRE_PED_WEAPON / aim ignore controller pitch — removed for motion aim.
// Origin is ComputeAimOrigin (ctrl world / cam), not cam-only — Session 3.
bool ComputeAimPoint(float* ax, float* ay, float* az, bool* fromCtrl) {
  if (fromCtrl)
    *fromCtrl = false;
  float fwd[3]{};
  bool haveCtrlFwd = false;
  if (GetControllerAimPose(nullptr, fwd, nullptr)) {
    haveCtrlFwd = true;
  } else if (!ComputeAimForward(fwd)) {
    return false;
  }
  if (!Normalize3(&fwd[0], &fwd[1], &fwd[2]))
    return false;
  float ox = 0.f, oy = 0.f, oz = 0.f;
  bool fromOriginCtrl = false;
  if (!ComputeAimOrigin(&ox, &oy, &oz, &fromOriginCtrl))
    return false;
  if (fromCtrl)
    *fromCtrl = haveCtrlFwd || fromOriginCtrl;
  *ax = ox + fwd[0] * kAimRayMeters;
  *ay = oy + fwd[1] * kAimRayMeters;
  *az = oz + fwd[2] * kAimRayMeters;
  return true;
}

bool ApplyAimOverride() {
  if (GetAimMode() != 2)
    return false;

  LogAimNativeHandlersOnce();
  if (!g_fnGetPlayerChar) {
    static std::atomic<bool> s_miss{false};
    if (!s_miss.exchange(true))
      Log("AimOverride: Plan A blocked — GET_PLAYER_CHAR missing");
    return false;
  }

  uint32_t ped = 0;
  if (!ResolvePlayerChar(&ped)) {
    static std::atomic<bool> s_noped{false};
    if (!s_noped.exchange(true))
      Log("AimOverride: GET_PLAYER_CHAR failed");
    return false;
  }
  if (IsCharInAnyCar(ped)) {
    SetMouseShoot(false);  // release any stale LMB; never inject while natives fire
    return false;
  }

  const bool aim = g_gripHeld.load() || IsVrPadAimHeld();
  const bool shoot = IsVrPadShootHeld();
  if (!aim && !shoot) {
    SetMouseShoot(false);
    return false;
  }

  // One projectile path only: FIRE_PED_WEAPON. No mouse LMB, no FIRE_SINGLE_BULLET
  // (build 004820 via=3 + LMB + VrPad RT = two+ bullets). VrPad zeros RT when aimmode=2.
  SetMouseShoot(false);

  float ax = 0.f, ay = 0.f, az = 0.f;
  if (!ComputeAimPoint(&ax, &ay, &az))
    return false;

  const DWORD now = GetTickCount();

  if (shoot) {
    const bool rising = !g_shootWasHeld;
    g_shootWasHeld = true;
    if (!g_fireNativesDead.load() && now - g_lastShootTick >= kFireIntervalMs) {
      g_lastShootTick = now;
      int via = 0;
      float fwd[3]{};
      ComputeAimForward(fwd);
      __try {
        // Clear aim-task only on trigger press — every-shot clear thrashes anim.
        if (rising && g_fnClearTasks) {
          const uint32_t cargs[] = {ped};
          InvokeNativeFn(g_fnClearTasks, cargs, 1, nullptr);
        }
        if (g_fnFirePedWeapon) {
          const uint32_t args[] = {ped, FloatBits(ax), FloatBits(ay), FloatBits(az)};
          if (InvokeNativeFn(g_fnFirePedWeapon, args, 4, nullptr))
            via |= 1;
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fireNativesDead.store(true);
        Log("AimOverride: EXCEPTION on FIRE_PED_WEAPON — natives OFF (004820 freeze)");
      }
      static std::atomic<uint32_t> s_shootN{0};
      const uint32_t n = ++s_shootN;
      if (n <= 12 || (n % 30) == 0)
        Log("AimOverride #%u: FIRE ped=%u target=(%.1f,%.1f,%.1f) fwd=(%.3f,%.3f,%.3f) "
            "via=%d tipPitch=%.0f (1=PED_WEAPON only)",
            n, ped, ax, ay, az, fwd[0], fwd[1], fwd[2], via, kG2TipPitchDeg);
    }
    return true;
  }

  g_shootWasHeld = false;

  // Session 3: TASK_AIM does NOT follow controllers (logs+headset: side-to-side only).
  // Hand bone probe uses PedGetBonePos thiscall — see TickAimFromGameThread.
  if (!kUseTaskAim || g_taskAimDead.load() || !g_fnAimAtCoord)
    return aim;
  if (now - g_lastAimTick < kAimIntervalMs)
    return true;
  g_lastAimTick = now;

  const uint32_t args[] = {ped, FloatBits(ax), FloatBits(ay), FloatBits(az),
                           static_cast<uint32_t>(kAimDurationMs)};
  __try {
    if (!InvokeNativeFn(g_fnAimAtCoord, args, 5, nullptr)) {
      static std::atomic<bool> s_fail{false};
      if (!s_fail.exchange(true))
        Log("AimOverride: TASK_AIM_GUN_AT_COORD InvokeNativeFn failed");
      return false;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    static std::atomic<bool> s_ex{false};
    if (!s_ex.exchange(true))
      Log("AimOverride: EXCEPTION on TASK_AIM — task-aim OFF (shoot still OK)");
    g_taskAimDead.store(true);
    return false;
  }

  static std::atomic<uint32_t> s_aimN{0};
  const uint32_t n = ++s_aimN;
  if (n <= 8 || (n % 40) == 0) {
    Log("AimOverride #%u: TASK_AIM ped=%u target=(%.1f,%.1f,%.1f) ctrl=%d", n, ped, ax, ay, az,
        g_ctrlValid.load() ? 1 : 0);
  }
  return true;
}

void TickAimFromGameThread() {
  const bool aimHeld = g_gripHeld.load() || IsVrPadAimHeld();
  // IkAim (ikaim=1 + ikoffs): every-frame write *(CPed+mgr)+vec while aim held.
  // Plan B test: ikoffs "D58 90" + ComputeAimOrigin (tip world), not far ray.
  // IkPin (ikpin=1): LOG ONLY d= vs same origin. Keep ikread=0.
  float ox = 0.f, oy = 0.f, oz = 0.f;
  if (aimHeld && ComputeAimOrigin(&ox, &oy, &oz))
    TickIkAim(ox, oy, oz, true);
  else
    TickIkAim(0.f, 0.f, 0.f, false);
  TickIkProbe();
  TickIkWriter(aimHeld, g_ctrlValid.load());
  TickIkEsi();
  TickIkCopy(aimHeld);
  TickIkPin(aimHeld);
  TickIkRay(aimHeld);
  // PAGE_GUARD reader probe — keep ikread=0 (crashed on aim in Session 6).
  TickIkRead(aimHeld);
  TryGiveTestWeapons();
  if (GetAimMode() != 2)
    return;
  ApplyAimOverride();
  ApplyHandFollow();  // no-op (ped bones)
  uint32_t ped = 0;
  if (g_fnGetPlayerChar && ResolvePlayerChar(&ped) && !IsCharInAnyCar(ped)) {
    ApplyWeaponFollow(ped);
    ProbeHandBones(ped);  // rare read-only
  }
}

void UpdateAimDecouple() {
  // AimProbe when aimmode on OR ikray/ikpin (confirm ctrl=1 cheaply).
  if (GetAimMode() == 0 && !IsIkRayEnabled() && !IsIkPinEnabled())
    return;

  LogAimNativeHandlersOnce();

  const DWORD now = GetTickCount();
  if (now - g_lastProbeTick < 1000)
    return;
  g_lastProbeTick = now;

  float cPos[3]{}, cFwd[3]{}, cUp[3]{};
  const bool haveCtrl = GetControllerAimPose(cPos, cFwd, cUp);
  float camX = 0.f, camY = 0.f, camZ = 0.f;
  const bool haveCam = GetLastStereoCamPos(&camX, &camY, &camZ);
  float hFwd[3]{};
  const bool haveHmdFwd = ComputeAimForward(hFwd);

  const uint32_t n = ++g_probeN;
  Log("AimProbe #%u: ctrl=%d grip=%d fwd=(%.3f,%.3f,%.3f) posTrk=(%.2f,%.2f,%.2f) | "
      "cam=%d pos=(%.2f,%.2f,%.2f) | ray=%d (%.3f,%.3f,%.3f)",
      n, haveCtrl ? 1 : 0, g_gripHeld.load() ? 1 : 0, cFwd[0], cFwd[1], cFwd[2], cPos[0],
      cPos[1], cPos[2], haveCam ? 1 : 0, camX, camY, camZ, haveHmdFwd ? 1 : 0, hFwd[0], hFwd[1],
      hFwd[2]);
}

}  // namespace asi
