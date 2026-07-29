#include "vr_pad.h"
#include "aim_decouple.h"
#include "log.h"

#include "../../thirdparty/minhook/include/MinHook.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace asi {
namespace {

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetState_t g_realGetState13 = nullptr;
XInputGetState_t g_realGetState910 = nullptr;
std::atomic<bool> g_hooksOn{false};
std::atomic<int> g_enabled{-1};

std::mutex g_mu;
XINPUT_GAMEPAD g_pad{};
std::atomic<uint32_t> g_packet{0};
std::atomic<bool> g_vrInject{false};  // true only while VR sticks/buttons active
std::atomic<bool> g_aimHeld{false};    // right grip
std::atomic<bool> g_shootHeld{false};  // right trigger
std::atomic<float> g_rsX{0.f};

constexpr float kDead = 0.18f;
constexpr float kGripAim = 0.35f;

bool PadHasActivity(const XINPUT_GAMEPAD& p) {
  if (p.wButtons != 0)
    return true;
  if (p.bLeftTrigger > 30 || p.bRightTrigger > 30)
    return true;
  auto stickLive = [](SHORT v) {
    return std::abs(static_cast<int>(v)) > static_cast<int>(kDead * 32767.f);
  };
  return stickLive(p.sThumbLX) || stickLive(p.sThumbLY) || stickLive(p.sThumbRX) ||
         stickLive(p.sThumbRY);
}

void MergeGamepad(XINPUT_GAMEPAD* dst, const XINPUT_GAMEPAD& vr) {
  // OR buttons; take stronger triggers; VR stick wins when deflected, else keep dst.
  dst->wButtons = static_cast<WORD>(dst->wButtons | vr.wButtons);
  if (vr.bLeftTrigger > dst->bLeftTrigger)
    dst->bLeftTrigger = vr.bLeftTrigger;
  if (vr.bRightTrigger > dst->bRightTrigger)
    dst->bRightTrigger = vr.bRightTrigger;
  auto takeStick = [](SHORT& d, SHORT v) {
    if (std::abs(static_cast<int>(v)) > std::abs(static_cast<int>(d)))
      d = v;
  };
  takeStick(dst->sThumbLX, vr.sThumbLX);
  takeStick(dst->sThumbLY, vr.sThumbLY);
  takeStick(dst->sThumbRX, vr.sThumbRX);
  takeStick(dst->sThumbRY, vr.sThumbRY);
}

void SendWeaponScroll(int wheelNotches) {
  if (wheelNotches == 0)
    return;
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = MOUSEEVENTF_WHEEL;
  in.mi.mouseData = static_cast<DWORD>(wheelNotches * WHEEL_DELTA);
  SendInput(1, &in, sizeof(INPUT));
}

void SendKeyTap(WORD vk) {
  INPUT down{};
  down.type = INPUT_KEYBOARD;
  down.ki.wVk = vk;
  INPUT up = down;
  up.ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(1, &down, sizeof(INPUT));
  SendInput(1, &up, sizeof(INPUT));
}

int ReadVrPadFile() {
  char path[MAX_PATH]{};
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&ReadVrPadFile), &self))
    return 1;
  if (!GetModuleFileNameA(self, path, MAX_PATH))
    return 1;
  char* slash = strrchr(path, '\\');
  if (!slash)
    slash = strrchr(path, '/');
  if (!slash)
    return 1;
  slash[1] = 0;
  strcat_s(path, "gtaiv_dxvk_vr.vrpad");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return 1;
  char buf[8]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0)
    return 1;
  int v = 1;
  if (sscanf_s(buf, "%d", &v) != 1)
    return 1;
  return (v != 0) ? 1 : 0;
}

SHORT StickFromAxis(float v) {
  if (v > 1.f)
    v = 1.f;
  if (v < -1.f)
    v = -1.f;
  if (std::fabs(v) < kDead)
    return 0;
  const float s = (std::fabs(v) - kDead) / (1.f - kDead);
  const float signedS = (v < 0.f) ? -s : s;
  const int i = static_cast<int>(signedS * 32767.f);
  if (i > 32767)
    return 32767;
  if (i < -32767)
    return -32767;
  return static_cast<SHORT>(i);
}

BYTE TriggerFromAxis(float v) {
  if (v < 0.f)
    v = 0.f;
  if (v > 1.f)
    v = 1.f;
  return static_cast<BYTE>(v * 255.f);
}

vr::TrackedDeviceIndex_t RoleOrScan(vr::IVRSystem* sys, const vr::TrackedDevicePose_t* poses,
                                    uint32_t poseCount, vr::ETrackedControllerRole role) {
  vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(role);
  if (idx != vr::k_unTrackedDeviceIndexInvalid && idx < poseCount && poses[idx].bPoseIsValid &&
      poses[idx].bDeviceIsConnected)
    return idx;
  int found = 0;
  const int want = (role == vr::TrackedControllerRole_LeftHand) ? 0 : 1;
  for (uint32_t i = 0; i < poseCount && i < vr::k_unMaxTrackedDeviceCount; ++i) {
    if (!poses[i].bDeviceIsConnected || !poses[i].bPoseIsValid)
      continue;
    if (sys->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller)
      continue;
    if (found == want)
      return static_cast<vr::TrackedDeviceIndex_t>(i);
    ++found;
  }
  return vr::k_unTrackedDeviceIndexInvalid;
}

// Official G2 / SteamVR hpmotioncontroller: stick = /input/joystick (NOT trackpad Axis0).
// OpenVR: query Prop_AxisNType — Joystick=2, Trigger=3, TrackPad=1.
int FindAxisOfType(vr::IVRSystem* sys, vr::TrackedDeviceIndex_t idx, vr::EVRControllerAxisType want) {
  for (int a = 0; a < 5; ++a) {
    vr::ETrackedPropertyError err = vr::TrackedProp_Success;
    const int32_t t = sys->GetInt32TrackedDeviceProperty(
        idx, static_cast<vr::ETrackedDeviceProperty>(vr::Prop_Axis0Type_Int32 + a), &err);
    if (err == vr::TrackedProp_Success && t == static_cast<int32_t>(want))
      return a;
  }
  return -1;
}

struct HandState {
  float stickX = 0, stickY = 0;
  float trigger = 0;
  float gripAnalog = 0;
  uint64_t buttons = 0;
  int stickAxis = -1;
  int trigAxis = -1;
  bool ok = false;
};

HandState ReadHand(vr::IVRSystem* sys, vr::TrackedDeviceIndex_t idx) {
  HandState h{};
  if (!sys || idx == vr::k_unTrackedDeviceIndexInvalid)
    return h;
  vr::VRControllerState_t st{};
  if (!sys->GetControllerState(idx, &st, sizeof(st)))
    return h;
  h.ok = true;
  h.buttons = st.ulButtonPressed;

  h.stickAxis = FindAxisOfType(sys, idx, vr::k_eControllerAxis_Joystick);
  h.trigAxis = FindAxisOfType(sys, idx, vr::k_eControllerAxis_Trigger);
  // G2 fallback (SteamVR compositor bindings use "joystick"): Axis2 stick, Axis1 trigger.
  if (h.stickAxis < 0)
    h.stickAxis = 2;
  if (h.trigAxis < 0)
    h.trigAxis = 1;

  h.stickX = st.rAxis[h.stickAxis].x;
  h.stickY = st.rAxis[h.stickAxis].y;
  h.trigger = st.rAxis[h.trigAxis].x;
  if (h.trigger < 0.f)
    h.trigger = 0.f;

  // Squeeze analog: some builds put grip force on a remaining axis; else digital grip.
  for (int a = 0; a < 5; ++a) {
    if (a == h.stickAxis || a == h.trigAxis)
      continue;
    const float ax = st.rAxis[a].x;
    if (ax > h.gripAnalog)
      h.gripAnalog = ax;
  }
  return h;
}

DWORD WINAPI HookGetState(DWORD userIndex, XINPUT_STATE* pState, XInputGetState_t realFn) {
  if (!pState)
    return ERROR_BAD_ARGUMENTS;

  // Always ask the real pad first (physical Xbox / none).
  DWORD realErr = ERROR_DEVICE_NOT_CONNECTED;
  XINPUT_STATE realSt{};
  if (realFn)
    realErr = realFn(userIndex, &realSt);

  if (userIndex != 0 || !IsVrPadEnabled()) {
    if (realErr == ERROR_SUCCESS) {
      *pState = realSt;
      return ERROR_SUCCESS;
    }
    return realErr;
  }

  const bool vrOn = g_vrInject.load();
  if (!vrOn) {
    // VR idle → pass through. No pad at all → NOT_CONNECTED so GTA keeps keyboard.
    if (realErr == ERROR_SUCCESS) {
      *pState = realSt;
      return ERROR_SUCCESS;
    }
    return ERROR_DEVICE_NOT_CONNECTED;
  }

  // VR active: merge onto real pad (or fresh empty if no Xbox plugged in).
  XINPUT_GAMEPAD merged{};
  if (realErr == ERROR_SUCCESS)
    merged = realSt.Gamepad;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    MergeGamepad(&merged, g_pad);
  }
  pState->dwPacketNumber = g_packet.load();
  if (realErr == ERROR_SUCCESS && realSt.dwPacketNumber > pState->dwPacketNumber)
    pState->dwPacketNumber = realSt.dwPacketNumber + 1;
  pState->Gamepad = merged;
  return ERROR_SUCCESS;
}

DWORD WINAPI HookGetState13(DWORD userIndex, XINPUT_STATE* pState) {
  return HookGetState(userIndex, pState, g_realGetState13);
}

DWORD WINAPI HookGetState910(DWORD userIndex, XINPUT_STATE* pState) {
  return HookGetState(userIndex, pState, g_realGetState910);
}

bool HookOneXInput(const char* dllName, XInputGetState_t* outReal, XInputGetState_t detour,
                   const char* tag) {
  HMODULE mod = GetModuleHandleA(dllName);
  if (!mod)
    mod = LoadLibraryA(dllName);
  if (!mod) {
    Log("VrPad: %s not loaded", dllName);
    return false;
  }
  void* proc = reinterpret_cast<void*>(GetProcAddress(mod, "XInputGetState"));
  if (!proc) {
    Log("VrPad: %s XInputGetState export MISS", dllName);
    return false;
  }
  if (MH_CreateHook(proc, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(outReal)) !=
          MH_OK ||
      MH_EnableHook(proc) != MH_OK) {
    Log("VrPad: MH hook FAIL %s", tag);
    return false;
  }
  Log("VrPad: hooked %s!XInputGetState", dllName);
  return true;
}

}  // namespace

bool IsVrPadEnabled() {
  int e = g_enabled.load();
  if (e < 0) {
    e = ReadVrPadFile();
    g_enabled.store(e);
    Log("VrPad: vrpad=%d (%s) — kill: gtaiv_dxvk_vr.vrpad=0", e, e ? "ON" : "OFF");
  }
  return e != 0;
}

bool IsVrPadAimHeld() {
  return g_aimHeld.load();
}

bool IsVrPadShootHeld() {
  return g_shootHeld.load();
}

float GetVrPadRightStickX() {
  return g_rsX.load();
}

bool InstallVrPadHooks() {
  if (g_hooksOn.load())
    return true;
  bool any = false;
  any |= HookOneXInput("xinput1_3.dll", &g_realGetState13, &HookGetState13, "1_3");
  any |= HookOneXInput("xinput9_1_0.dll", &g_realGetState910, &HookGetState910, "9_1_0");
  g_hooksOn.store(any);
  if (any)
    Log("VrPad: merge mode — VR idle passes Xbox/keyboard through; VR active merges. "
        "Weapons: Y or R-stick-click = next (D-pad/scroll), L-stick-click = prev");
  return any;
}

void UpdateVrPadFromPoses(const vr::TrackedDevicePose_t* poses, uint32_t poseCount) {
  if (!IsVrPadEnabled())
    return;
  InstallVrPadHooks();

  vr::IVRSystem* sys = vr::VRSystem();
  if (!sys || !poses || poseCount == 0) {
    g_vrInject.store(false);
    g_aimHeld.store(false);
    g_shootHeld.store(false);
    return;
  }

  const vr::TrackedDeviceIndex_t li =
      RoleOrScan(sys, poses, poseCount, vr::TrackedControllerRole_LeftHand);
  const vr::TrackedDeviceIndex_t ri =
      RoleOrScan(sys, poses, poseCount, vr::TrackedControllerRole_RightHand);

  const HandState L = ReadHand(sys, li);
  const HandState R = ReadHand(sys, ri);
  if (!L.ok && !R.ok) {
    g_vrInject.store(false);
    g_aimHeld.store(false);
    g_shootHeld.store(false);
    return;
  }

  // OpenXR HP G2: /input/a /b /x /y /menu /joystick /trigger /squeeze
  // Menu = ApplicationMenu (NOT Windows/System).
  const uint64_t maskA = vr::ButtonMaskFromId(vr::k_EButton_A);
  const uint64_t maskMenu = vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu);
  const uint64_t maskGrip = vr::ButtonMaskFromId(vr::k_EButton_Grip);
  const uint64_t maskSys = vr::ButtonMaskFromId(vr::k_EButton_System);
  const uint64_t maskStickClk = vr::ButtonMaskFromId(vr::k_EButton_Axis2);
  const uint64_t maskDpadLeft = vr::ButtonMaskFromId(vr::k_EButton_DPad_Left);
  const uint64_t maskDpadUp = vr::ButtonMaskFromId(vr::k_EButton_DPad_Up);
  const uint64_t maskDpadRight = vr::ButtonMaskFromId(vr::k_EButton_DPad_Right);
  const uint64_t maskDpadDown = vr::ButtonMaskFromId(vr::k_EButton_DPad_Down);
  const uint64_t maskReserved0 = vr::ButtonMaskFromId(vr::k_EButton_Reserved0);
  const uint64_t maskReserved1 = vr::ButtonMaskFromId(vr::k_EButton_Reserved1);

  WORD buttons = 0;

  // Face A/X (OpenVR A id per hand)
  if (R.buttons & maskA)
    buttons |= XINPUT_GAMEPAD_A;  // sprint
  if (L.buttons & maskA)
    buttons |= XINPUT_GAMEPAD_X;  // jump

  // Face B (right secondary)
  if (R.buttons & (maskReserved0 | maskReserved1 | maskDpadRight | maskDpadDown))
    buttons |= XINPUT_GAMEPAD_B;  // enter/exit

  // GTA IV weapon cycle = D-Pad L/R (NOT the Y face button — Y is reload).
  // Left Y / aliases → next weapon; stick clicks → prev/next (easy to find).
  const bool yFace =
      (L.buttons & (maskReserved0 | maskReserved1 | maskDpadLeft | maskDpadUp)) != 0;
  if (yFace)
    buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;  // next weapon
  if (L.buttons & maskStickClk)
    buttons |= XINPUT_GAMEPAD_DPAD_LEFT;  // previous weapon
  if (R.buttons & maskStickClk)
    buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;  // next weapon

  // Edge: also scroll mouse wheel so keyboard-control mode still switches weapons.
  static bool s_prevNext = false, s_prevPrev = false;
  const bool wantNext = yFace || (R.buttons & maskStickClk) != 0;
  const bool wantPrev = (L.buttons & maskStickClk) != 0;
  if (wantNext && !s_prevNext)
    SendWeaponScroll(1);
  if (wantPrev && !s_prevPrev)
    SendWeaponScroll(-1);
  s_prevNext = wantNext;
  s_prevPrev = wantPrev;

  // Menu → Start. Windows unmapped.
  if ((L.buttons | R.buttons) & maskMenu)
    buttons |= XINPUT_GAMEPAD_START;
  (void)maskSys;

  // Face B rarely reports on G2 legacy bits (logs never showed 0x2000).
  // Left grip → Enter/Exit: Xbox B + keyboard F (GTA IV enter key).
  static bool s_prevEnter = false;
  const bool lGrip = (L.buttons & maskGrip) != 0 || L.gripAnalog >= kGripAim;
  if (lGrip)
    buttons |= XINPUT_GAMEPAD_B;
  if (lGrip && !s_prevEnter)
    SendKeyTap('F');
  s_prevEnter = lGrip;

  // Right grip = aim → Xbox LT. Right trigger = shoot held for natives.
  // aimmode=2: do NOT inject RT — stock gun fire was a second bullet alongside FIRE_*.
  const bool rGrip = (R.buttons & maskGrip) != 0 || R.gripAnalog >= kGripAim;
  const bool rTrig = R.trigger >= 0.35f ||
                     (R.buttons & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
  g_aimHeld.store(rGrip);
  g_shootHeld.store(rTrig);
  const bool nativeShoot = (GetAimMode() == 2);

  XINPUT_GAMEPAD pad{};
  pad.wButtons = buttons;
  pad.bLeftTrigger = rGrip ? 255 : 0;
  pad.bRightTrigger = nativeShoot ? 0 : (rTrig ? 255 : TriggerFromAxis(R.trigger));
  pad.sThumbLX = StickFromAxis(L.stickX);
  pad.sThumbLY = StickFromAxis(L.stickY);
  pad.sThumbRX = StickFromAxis(R.stickX);
  pad.sThumbRY = StickFromAxis(R.stickY);

  {
    float x = R.stickX;
    if (std::fabs(x) < kDead)
      x = 0.f;
    else {
      const float s = (std::fabs(x) - kDead) / (1.f - kDead);
      x = (x < 0.f) ? -s : s;
    }
    g_rsX.store(x);
  }

  const bool active = PadHasActivity(pad);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pad = pad;
  }
  if (active)
    g_packet.fetch_add(1);
  g_vrInject.store(active);

  static bool s_axisLogged = false;
  if (!s_axisLogged && (L.ok || R.ok)) {
    s_axisLogged = true;
    Log("VrPad: axis map L stick=%d trig=%d | R stick=%d trig=%d", L.stickAxis, L.trigAxis,
        R.stickAxis, R.trigAxis);
  }

  static bool s_faceLogged = false;
  if (!s_faceLogged && (L.buttons | R.buttons) != 0) {
    const uint64_t interesting =
        (L.buttons | R.buttons) &
        ~(maskGrip | maskStickClk | vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger));
    if (interesting) {
      s_faceLogged = true;
      Log("VrPad: raw buttons L=0x%llX R=0x%llX xbox=0x%04X inject=%d "
          "(Y/R-click=next weapon, L-click=prev; idle=keyboard/Xbox pass-through)",
          static_cast<unsigned long long>(L.buttons), static_cast<unsigned long long>(R.buttons),
          buttons, active ? 1 : 0);
    }
  }

  static uint32_t s_log = 0;
  if ((++s_log % 300) == 1) {
    Log("VrPad: inject=%d btn=0x%04X LT=%u RT=%u LS=(%d,%d) RS=(%d,%d) aimGrip=%d",
        active ? 1 : 0, buttons, pad.bLeftTrigger, pad.bRightTrigger, pad.sThumbLX, pad.sThumbLY,
        pad.sThumbRX, pad.sThumbRY, rGrip ? 1 : 0);
  }
}

}  // namespace asi
