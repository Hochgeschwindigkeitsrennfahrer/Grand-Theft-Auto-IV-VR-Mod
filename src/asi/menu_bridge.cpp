#include "menu_bridge.h"
#include "log.h"
#include "native_invoke.h"
#include "stereo_config.h"
#include "vr_input.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include <openvr.h>

namespace asi {
namespace {

constexpr DWORD kPollIntervalMs = 400;
constexpr int kMaxProfiles = 8;

struct Profile {
  int value = 0;      // FusionFix pref value that selects this profile
  int stereoMode = 0; // gtaiv_dxvk_vr.stereo written when selected
};

Profile g_profiles[kMaxProfiles];
int g_profileCount = 0;
int g_profileIdx = -1;
char g_prefName[64]{};
bool g_configRead = false;
DWORD g_lastPoll = 0;
int g_lastPrefValue = -0x7FFFFFFF;
void* g_fnPrefRead = nullptr;
std::atomic<bool> g_prefNativeDead{false};

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

size_t ReadTextFile(const char* name, char* buf, size_t bufLen) {
  char path[MAX_PATH]{};
  if (!GetAsiDirLocal(path, MAX_PATH))
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

void SetDefaultProfiles() {
  // 243 = TrueStereo late-latch checkpoint (prebuilt/mode243).
  // 53  = AER temporal family (UsesAerPoseSubmit + distinct L/R, no flatten).
  // 0   = flat DXVK.
  // Which AER build to compare against is a taste call — override the whole list
  // in gtaiv_dxvk_vr.menumap rather than editing this.
  g_profiles[0] = {0, 243};
  g_profiles[1] = {1, 53};
  g_profiles[2] = {2, 0};
  g_profileCount = 3;
}

void ParseProfileMap() {
  char buf[512]{};
  const size_t n = ReadTextFile("gtaiv_dxvk_vr.menumap", buf, sizeof(buf));
  if (n == 0) {
    SetDefaultProfiles();
    return;
  }
  g_profileCount = 0;
  const char* p = buf;
  while (*p && g_profileCount < kMaxProfiles) {
    int value = 0, mode = 0;
    if (sscanf_s(p, "%d = %d", &value, &mode) == 2 && mode >= 0 && mode < 1000) {
      g_profiles[g_profileCount].value = value;
      g_profiles[g_profileCount].stereoMode = mode;
      ++g_profileCount;
    }
    const char* nl = strchr(p, '\n');
    if (!nl)
      break;
    p = nl + 1;
  }
  if (g_profileCount == 0)
    SetDefaultProfiles();
}

void ReadConfigOnce() {
  if (g_configRead)
    return;
  g_configRead = true;

  char buf[80]{};
  const size_t n = ReadTextFile("gtaiv_dxvk_vr.menupref", buf, sizeof(buf));
  if (n > 0) {
    // Trim whitespace / newline so the name matches the FusionFix table exactly.
    size_t start = 0;
    while (buf[start] == ' ' || buf[start] == '\t')
      ++start;
    size_t end = strlen(buf + start);
    while (end > 0 && (buf[start + end - 1] == '\r' || buf[start + end - 1] == '\n' ||
                       buf[start + end - 1] == ' ' || buf[start + end - 1] == '\t'))
      --end;
    if (end > 0 && end < sizeof(g_prefName)) {
      memcpy(g_prefName, buf + start, end);
      g_prefName[end] = 0;
    }
  }

  ParseProfileMap();

  char list[256]{};
  int used = 0;
  for (int i = 0; i < g_profileCount && used < static_cast<int>(sizeof(list)) - 16; ++i)
    used += snprintf(list + used, sizeof(list) - used, "%d=%d ", g_profiles[i].value,
                     g_profiles[i].stereoMode);
  Log("MenuBridge: pref=\"%s\" profiles: %s(F4 or right menu button cycles; "
      "kill: delete gtaiv_dxvk_vr.menupref)",
      g_prefName[0] ? g_prefName : "(none)", list);
}

void ApplyProfile(int idx, const char* why) {
  if (idx < 0 || idx >= g_profileCount)
    return;
  const int mode = g_profiles[idx].stereoMode;
  const int prev = static_cast<int>(GetStereoMode());
  if (mode == prev) {
    g_profileIdx = idx;
    return;
  }
  g_profileIdx = idx;
  WriteStereoModeFile(mode);
  ReloadStereoMode();
  const int now = static_cast<int>(GetStereoMode());
  Log("MenuBridge: profile %d -> stereo %d (was %d, live now %d) via %s. If the headset "
      "image does not change, restart the game — that family arms its hooks at startup",
      idx, mode, prev, now, why);

  // Short haptic pulse so the switch is noticeable without looking at the log.
  if (vr::VRSystem()) {
    const vr::TrackedDeviceIndex_t r =
        vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    if (r != vr::k_unTrackedDeviceIndexInvalid)
      vr::VRSystem()->TriggerHapticPulse(r, 0, 3000);
  }
}

bool KeyEdge(int vk, bool* wasDown) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool edge = down && !*wasDown;
  *wasDown = down;
  return edge;
}

}  // namespace

int FusionFixGetPref(const char* prefName, int defValue) {
  if (!prefName || !prefName[0] || g_prefNativeDead.load())
    return defValue;
  if (!g_fnPrefRead) {
    // FusionFix chains this native: a "PREF_*" string returns the setting value.
    g_fnPrefRead = GetNativeHandlerByName("GET_NUMBER_OF_INSTANCES_OF_STREAMED_SCRIPT");
    if (!g_fnPrefRead) {
      g_prefNativeDead.store(true);
      Log("MenuBridge: GET_NUMBER_OF_INSTANCES_OF_STREAMED_SCRIPT MISS — no FusionFix pref "
          "read (F4 / VR menu button still work)");
      return defValue;
    }
  }
  uint32_t result = 0;
  const uint32_t args[] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(prefName))};
  __try {
    if (!InvokeNativeFn(g_fnPrefRead, args, 1, &result))
      return defValue;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_prefNativeDead.store(true);
    Log("MenuBridge: EXCEPTION reading pref \"%s\" — pref read OFF", prefName);
    return defValue;
  }
  return static_cast<int>(result);
}

int GetMenuProfileIndex() {
  return g_profileIdx;
}

void TickMenuBridge() {
  ReadConfigOnce();

  static bool s_wasF4 = false;
  if (KeyEdge(VK_F4, &s_wasF4) || VrInputMenuChordPressed()) {
    const int next = (g_profileIdx + 1) % (g_profileCount > 0 ? g_profileCount : 1);
    ApplyProfile(next, "cycle");
    return;
  }

  if (!g_prefName[0])
    return;

  const DWORD now = GetTickCount();
  if (now - g_lastPoll < kPollIntervalMs)
    return;
  g_lastPoll = now;

  const int value = FusionFixGetPref(g_prefName, g_lastPrefValue);
  if (value == g_lastPrefValue)
    return;
  g_lastPrefValue = value;

  for (int i = 0; i < g_profileCount; ++i) {
    if (g_profiles[i].value == value) {
      ApplyProfile(i, g_prefName);
      return;
    }
  }
  Log("MenuBridge: pref \"%s\" = %d has no profile in gtaiv_dxvk_vr.menumap — ignored",
      g_prefName, value);
}

}  // namespace asi
