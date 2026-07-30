#include "vr_input.h"
#include "log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace asi {
namespace {

constexpr const char* kActionSet = "/actions/gtaivvr";
constexpr float kFireThreshold = 0.35f;
constexpr float kAimThreshold = 0.35f;

bool GetAsiDirLocal(char* out, size_t cap) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&GetAsiDirLocal), &self))
    return false;
  if (!GetModuleFileNameA(self, out, static_cast<DWORD>(cap)))
    return false;
  char* slash = strrchr(out, '\\');
  if (!slash)
    slash = strrchr(out, '/');
  if (!slash)
    return false;
  slash[1] = 0;
  return true;
}

int ReadIntFile(const char* name, int defValue) {
  char path[MAX_PATH]{};
  if (!GetAsiDirLocal(path, MAX_PATH))
    return defValue;
  strcat_s(path, name);
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return defValue;
  char buf[16]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  int v = defValue;
  if (n == 0 || sscanf_s(buf, "%d", &v) != 1)
    return defValue;
  return v;
}

struct Handles {
  vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
  vr::VRActionHandle_t tip[2]{};      // 0 right, 1 left
  vr::VRActionHandle_t gripPose[2]{};
  vr::VRActionHandle_t stick[2]{};
  vr::VRActionHandle_t trigger[2]{};
  vr::VRActionHandle_t squeeze[2]{};
  vr::VRActionHandle_t primary[2]{};
  vr::VRActionHandle_t secondary[2]{};
  vr::VRActionHandle_t stickClick[2]{};
  vr::VRActionHandle_t menu = vr::k_ulInvalidActionHandle;
  vr::VRActionHandle_t vrMenu = vr::k_ulInvalidActionHandle;
};

std::mutex g_mu;
Handles g_h;
VrInputHand g_hand[2];
vr::HmdMatrix34_t g_tipMat[2]{};
vr::HmdMatrix34_t g_gripMat[2]{};
bool g_tipOk[2]{};
bool g_gripOk[2]{};
std::atomic<bool> g_active{false};
std::atomic<bool> g_initDone{false};
std::atomic<bool> g_initOk{false};
std::atomic<bool> g_vrMenuEdge{false};

bool ResolveAction(const char* name, vr::VRActionHandle_t* out) {
  const vr::EVRInputError err = vr::VRInput()->GetActionHandle(name, out);
  if (err != vr::VRInputError_None || *out == vr::k_ulInvalidActionHandle) {
    Log("VrInput: action MISS %s err=%d", name, static_cast<int>(err));
    return false;
  }
  return true;
}

// Handle names differ only by the hand prefix — resolve both in one call.
bool ResolvePair(const char* rightName, const char* leftName, vr::VRActionHandle_t* pair) {
  const bool r = ResolveAction(rightName, &pair[0]);
  const bool l = ResolveAction(leftName, &pair[1]);
  return r && l;
}

bool ReadPose(vr::VRActionHandle_t h, vr::HmdMatrix34_t* out) {
  if (h == vr::k_ulInvalidActionHandle)
    return false;
  vr::InputPoseActionData_t data{};
  const vr::EVRInputError err = vr::VRInput()->GetPoseActionDataRelativeToNow(
      h, vr::TrackingUniverseStanding, 0.f, &data, sizeof(data), vr::k_ulInvalidInputValueHandle);
  if (err != vr::VRInputError_None || !data.bActive || !data.pose.bPoseIsValid)
    return false;
  *out = data.pose.mDeviceToAbsoluteTracking;
  return true;
}

bool ReadDigital(vr::VRActionHandle_t h) {
  if (h == vr::k_ulInvalidActionHandle)
    return false;
  vr::InputDigitalActionData_t data{};
  if (vr::VRInput()->GetDigitalActionData(h, &data, sizeof(data),
                                          vr::k_ulInvalidInputValueHandle) !=
      vr::VRInputError_None)
    return false;
  return data.bActive && data.bState;
}

bool ReadDigitalRising(vr::VRActionHandle_t h) {
  if (h == vr::k_ulInvalidActionHandle)
    return false;
  vr::InputDigitalActionData_t data{};
  if (vr::VRInput()->GetDigitalActionData(h, &data, sizeof(data),
                                          vr::k_ulInvalidInputValueHandle) !=
      vr::VRInputError_None)
    return false;
  return data.bActive && data.bState && data.bChanged;
}

void ReadAnalog(vr::VRActionHandle_t h, float* x, float* y) {
  if (h == vr::k_ulInvalidActionHandle)
    return;
  vr::InputAnalogActionData_t data{};
  if (vr::VRInput()->GetAnalogActionData(h, &data, sizeof(data),
                                         vr::k_ulInvalidInputValueHandle) !=
      vr::VRInputError_None)
    return;
  if (!data.bActive)
    return;
  if (x)
    *x = data.x;
  if (y)
    *y = data.y;
}

}  // namespace

bool IsVrInputEnabled() {
  static std::atomic<int> s_v{-1};
  int v = s_v.load();
  if (v < 0) {
    v = (ReadIntFile("gtaiv_dxvk_vr.vrinput", 0) != 0) ? 1 : 0;
    s_v.store(v);
    if (v)
      Log("VrInput: vrinput=1 (IVRInput action manifest) — kill: write 0 to "
          "gtaiv_dxvk_vr.vrinput");
  }
  return v != 0;
}

bool VrInputInit() {
  if (!IsVrInputEnabled())
    return false;
  if (g_initDone.load())
    return g_initOk.load();
  g_initDone.store(true);

  if (!vr::VRInput()) {
    Log("VrInput: VRInput() null — staying on legacy input");
    return false;
  }

  char manifest[MAX_PATH]{};
  if (!GetAsiDirLocal(manifest, MAX_PATH))
    return false;
  strcat_s(manifest, "gtaiv_dxvk_vr_actions.json");
  if (GetFileAttributesA(manifest) == INVALID_FILE_ATTRIBUTES) {
    Log("VrInput: manifest MISSING (%s) — copy config/vr_actions/*.json next to the ASI. "
        "Staying on legacy input",
        manifest);
    return false;
  }

  const vr::EVRInputError merr = vr::VRInput()->SetActionManifestPath(manifest);
  if (merr != vr::VRInputError_None) {
    Log("VrInput: SetActionManifestPath err=%d (%s) — staying on legacy input",
        static_cast<int>(merr), manifest);
    return false;
  }

  Handles h{};
  const bool setOk =
      vr::VRInput()->GetActionSetHandle(kActionSet, &h.set) == vr::VRInputError_None &&
      h.set != vr::k_ulInvalidActionSetHandle;
  if (!setOk) {
    Log("VrInput: action set %s MISS — staying on legacy input", kActionSet);
    return false;
  }

  // Tip poses are the one hard requirement; everything else degrades gracefully.
  const bool tipOk =
      ResolvePair("/actions/gtaivvr/in/RightTipPose", "/actions/gtaivvr/in/LeftTipPose", h.tip);
  ResolvePair("/actions/gtaivvr/in/RightGripPose", "/actions/gtaivvr/in/LeftGripPose",
              h.gripPose);
  ResolvePair("/actions/gtaivvr/in/RightStick", "/actions/gtaivvr/in/LeftStick", h.stick);
  ResolvePair("/actions/gtaivvr/in/RightTrigger", "/actions/gtaivvr/in/LeftTrigger", h.trigger);
  ResolvePair("/actions/gtaivvr/in/RightSqueeze", "/actions/gtaivvr/in/LeftSqueeze", h.squeeze);
  ResolvePair("/actions/gtaivvr/in/RightPrimary", "/actions/gtaivvr/in/LeftPrimary", h.primary);
  ResolvePair("/actions/gtaivvr/in/RightSecondary", "/actions/gtaivvr/in/LeftSecondary",
              h.secondary);
  ResolvePair("/actions/gtaivvr/in/RightStickClick", "/actions/gtaivvr/in/LeftStickClick",
              h.stickClick);
  ResolveAction("/actions/gtaivvr/in/Menu", &h.menu);
  ResolveAction("/actions/gtaivvr/in/VrMenu", &h.vrMenu);

  if (!tipOk) {
    Log("VrInput: tip pose actions missing — staying on legacy input");
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_h = h;
  }
  g_initOk.store(true);
  Log("VrInput: manifest loaded (%s) — tip poses active, 55deg grip hack bypassed", manifest);
  return true;
}

void VrInputUpdate() {
  if (!IsVrInputEnabled())
    return;
  if (!g_initOk.load() && !VrInputInit())
    return;

  Handles h;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    h = g_h;
  }

  vr::VRActiveActionSet_t active{};
  active.ulActionSet = h.set;
  active.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
  const vr::EVRInputError uerr =
      vr::VRInput()->UpdateActionState(&active, sizeof(vr::VRActiveActionSet_t), 1);
  if (uerr != vr::VRInputError_None) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true))
      Log("VrInput: UpdateActionState err=%d", static_cast<int>(uerr));
    return;
  }

  vr::HmdMatrix34_t tip[2]{}, grip[2]{};
  bool tipOk[2]{}, gripOk[2]{};
  VrInputHand hand[2];
  for (int i = 0; i < 2; ++i) {
    tipOk[i] = ReadPose(h.tip[i], &tip[i]);
    gripOk[i] = ReadPose(h.gripPose[i], &grip[i]);

    VrInputHand s{};
    ReadAnalog(h.stick[i], &s.stickX, &s.stickY);
    ReadAnalog(h.trigger[i], &s.trigger, nullptr);
    ReadAnalog(h.squeeze[i], &s.grip, nullptr);
    s.fire = s.trigger >= kFireThreshold;
    s.aim = s.grip >= kAimThreshold;
    s.primary = ReadDigital(h.primary[i]);
    s.secondary = ReadDigital(h.secondary[i]);
    s.stickClick = ReadDigital(h.stickClick[i]);
    s.menu = ReadDigital(h.menu);
    s.valid = tipOk[i] || gripOk[i];
    hand[i] = s;
  }

  const bool vrMenu = ReadDigitalRising(h.vrMenu);

  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int i = 0; i < 2; ++i) {
      g_tipMat[i] = tip[i];
      g_gripMat[i] = grip[i];
      g_tipOk[i] = tipOk[i];
      g_gripOk[i] = gripOk[i];
      g_hand[i] = hand[i];
    }
  }
  if (vrMenu)
    g_vrMenuEdge.store(true);

  const bool anyPose = tipOk[0] || tipOk[1] || gripOk[0] || gripOk[1];
  if (anyPose && !g_active.exchange(true))
    Log("VrInput: ACTIVE — tipR=%d tipL=%d gripR=%d gripL=%d", tipOk[0] ? 1 : 0, tipOk[1] ? 1 : 0,
        gripOk[0] ? 1 : 0, gripOk[1] ? 1 : 0);

  static uint32_t s_log = 0;
  if (g_active.load() && (++s_log % 300) == 1)
    Log("VrInput: R trig=%.2f grip=%.2f stick=(%.2f,%.2f) aim=%d fire=%d | L stick=(%.2f,%.2f)",
        hand[0].trigger, hand[0].grip, hand[0].stickX, hand[0].stickY, hand[0].aim ? 1 : 0,
        hand[0].fire ? 1 : 0, hand[1].stickX, hand[1].stickY);
}

bool VrInputActive() {
  return IsVrInputEnabled() && g_active.load();
}

bool VrInputGetTipPose(int hand, vr::HmdMatrix34_t* out) {
  if (!out || hand < 0 || hand > 1 || !VrInputActive())
    return false;
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_tipOk[hand])
    return false;
  *out = g_tipMat[hand];
  return true;
}

bool VrInputGetGripPose(int hand, vr::HmdMatrix34_t* out) {
  if (!out || hand < 0 || hand > 1 || !VrInputActive())
    return false;
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_gripOk[hand])
    return false;
  *out = g_gripMat[hand];
  return true;
}

bool VrInputGetHand(int hand, VrInputHand* out) {
  if (!out || hand < 0 || hand > 1 || !VrInputActive())
    return false;
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_hand[hand].valid)
    return false;
  *out = g_hand[hand];
  return true;
}

bool VrInputMenuChordPressed() {
  return g_vrMenuEdge.exchange(false);
}

}  // namespace asi
