#include "menu_bridge.h"

#include "log.h"
#include "stereo_config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>

#include <openvr.h>

namespace asi {
namespace {

constexpr int kMaxProfiles = 8;

struct Profile {
  int value = 0;       // FusionFix pref value that selects this profile
  int stereoMode = 0;  // gtaiv_dxvk_vr.stereo written when selected
  char label[24]{};
};

Profile g_profiles[kMaxProfiles];
int g_profileCount = 0;
int g_profileIdx = -1;
bool g_configRead = false;

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

void MakeLabel(Profile* p) {
  // Mode 0 is the only one with a name everybody knows; the rest are numbers
  // and the log tells you what they are.
  if (p->stereoMode == 0)
    strcpy_s(p->label, "0 (flat)");
  else
    sprintf_s(p->label, "%d", p->stereoMode);
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
  for (int i = 0; i < g_profileCount; ++i)
    MakeLabel(&g_profiles[i]);
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
      MakeLabel(&g_profiles[g_profileCount]);
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

bool KeyEdge(int vk, bool* wasDown) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool edge = down && !*wasDown;
  *wasDown = down;
  return edge;
}

}  // namespace

void MenuBridgeInit() {
  if (g_configRead)
    return;
  g_configRead = true;

  ParseProfileMap();

  char list[256]{};
  int used = 0;
  for (int i = 0; i < g_profileCount && used < static_cast<int>(sizeof(list)) - 16; ++i)
    used += snprintf(list + used, sizeof(list) - used, "%d ", g_profiles[i].stereoMode);
  Log("MenuBridge: %d profiles: %s(F4 cycles; override in gtaiv_dxvk_vr.menumap)", g_profileCount,
      list);

  g_profileIdx = MenuBridgeCurrentProfile();
}

int MenuBridgeProfileCount() {
  MenuBridgeInit();
  return g_profileCount;
}

int MenuBridgeProfileMode(int idx) {
  MenuBridgeInit();
  if (idx < 0 || idx >= g_profileCount)
    return -1;
  return g_profiles[idx].stereoMode;
}

const char* MenuBridgeProfileLabel(int idx) {
  MenuBridgeInit();
  if (idx < 0 || idx >= g_profileCount)
    return "?";
  return g_profiles[idx].label;
}

int MenuBridgeCurrentProfile() {
  MenuBridgeInit();
  const int live = static_cast<int>(GetStereoMode());
  for (int i = 0; i < g_profileCount; ++i)
    if (g_profiles[i].stereoMode == live)
      return i;
  return -1;
}

void MenuBridgeApplyProfile(int idx, const char* why) {
  MenuBridgeInit();
  if (idx < 0 || idx >= g_profileCount)
    return;
  const int mode = g_profiles[idx].stereoMode;
  const int prev = static_cast<int>(GetStereoMode());
  g_profileIdx = idx;
  if (mode == prev)
    return;

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

void MenuBridgeTickHotkey() {
  MenuBridgeInit();
  static bool s_wasF4 = false;
  if (!KeyEdge(VK_F4, &s_wasF4))
    return;
  if (g_profileCount <= 0)
    return;
  // Re-sync first: the mode may have been changed by the overlay or a file edit.
  const int cur = MenuBridgeCurrentProfile();
  const int from = (cur >= 0) ? cur : g_profileIdx;
  MenuBridgeApplyProfile((from + 1) % g_profileCount, "F4");
}

}  // namespace asi
