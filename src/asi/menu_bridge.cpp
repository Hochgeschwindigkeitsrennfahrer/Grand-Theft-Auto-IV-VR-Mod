#include "menu_bridge.h"

#include "log.h"
#include "stereo_config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
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

// Names for the modes that shipped as milestones. A bare number in a menu row
// tells you nothing; anything not listed still falls back to the number.
struct ModeName {
  int mode;
  const char* name;
};
constexpr ModeName kModeNames[] = {
    {243, "243 TrueStereo"},  // late-latch checkpoint (prebuilt/mode243)
    {204, "204 Fused"},       // fused stereo milestone (prebuilt/mode204)
    {170, "170 Clean"},       // clean branch milestone (prebuilt/mode170)
    {53, "53 AER"},           // AER temporal family
    {0, "0 Flat"},
};

void MakeLabel(Profile* p) {
  for (const ModeName& n : kModeNames) {
    if (n.mode == p->stereoMode) {
      strcpy_s(p->label, n.name);
      return;
    }
  }
  sprintf_s(p->label, "%d", p->stereoMode);
}

void SetDefaultProfiles() {
  // Ordered WORST to BEST on purpose. The pause-menu row borrows a four-state
  // display whose strings are "Low / Medium / High / Very High" (see
  // docs/menu-integration/NATIVE_MENU.md — the strings come from the display
  // enum, not from the label, and cannot be renamed without an RE session).
  // Ascending fidelity makes those words mean something: Low = flat, Very High
  // = TrueStereo. Override the whole list in gtaiv_dxvk_vr.menumap rather than
  // editing this.
  static const int kDefaultModes[] = {0, 53, 170, 243};
  g_profileCount = static_cast<int>(sizeof(kDefaultModes) / sizeof(kDefaultModes[0]));
  for (int i = 0; i < g_profileCount; ++i) {
    g_profiles[i].value = i;
    g_profiles[i].stereoMode = kDefaultModes[i];
    MakeLabel(&g_profiles[i]);
  }
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

void MenuBridgeTickPrefFile() {
  MenuBridgeInit();

  static bool s_read = false;
  static char s_key[48]{};
  static DWORD s_lastPoll = 0;
  static FILETIME s_lastWrite{};
  static int s_lastValue = INT_MIN;

  if (!s_read) {
    s_read = true;
    char buf[64]{};
    const size_t n = ReadTextFile("gtaiv_dxvk_vr.menukey", buf, sizeof(buf));
    size_t end = n;
    while (end > 0 && (buf[end - 1] == '\r' || buf[end - 1] == '\n' || buf[end - 1] == ' ' ||
                       buf[end - 1] == '\t'))
      --end;
    if (end > 0 && end < sizeof(s_key)) {
      memcpy(s_key, buf, end);
      Log("MenuBridge: following pause-menu pref \"%s\" in FusionFix.cfg "
          "(kill: delete gtaiv_dxvk_vr.menukey)",
          s_key);
    }
  }
  if (!s_key[0])
    return;

  const DWORD now = GetTickCount();
  if (now - s_lastPoll < 500)
    return;
  s_lastPoll = now;

  char path[MAX_PATH]{};
  if (!GetAsiDirLocal(path, MAX_PATH))
    return;
  strcat_s(path, "plugins\\GTAIV.EFLC.FusionFix.cfg");

  // Skip the read entirely unless FusionFix actually rewrote the file.
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
    return;
  if (fad.ftLastWriteTime.dwLowDateTime == s_lastWrite.dwLowDateTime &&
      fad.ftLastWriteTime.dwHighDateTime == s_lastWrite.dwHighDateTime)
    return;
  s_lastWrite = fad.ftLastWriteTime;

  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return;
  char cfg[4096]{};
  const size_t n = fread(cfg, 1, sizeof(cfg) - 1, f);
  fclose(f);
  cfg[n] = 0;

  // "Key = 1" anywhere in the file. Match on a line start so a key that is a
  // suffix of another key cannot win.
  const size_t keyLen = strlen(s_key);
  int value = INT_MIN;
  for (const char* p = cfg; *p;) {
    const char* eol = strchr(p, '\n');
    if (_strnicmp(p, s_key, keyLen) == 0) {
      const char* q = p + keyLen;
      while (*q == ' ' || *q == '\t')
        ++q;
      if (*q == '=') {
        ++q;
        while (*q == ' ' || *q == '\t')
          ++q;
        value = atoi(q);
        break;
      }
    }
    if (!eol)
      break;
    p = eol + 1;
  }
  if (value == INT_MIN)
    return;

  if (s_lastValue == INT_MIN) {
    // First sighting: adopt it silently so gtaiv_dxvk_vr.stereo still wins at
    // startup. Only a change the player makes in the pause menu switches modes.
    s_lastValue = value;
    Log("MenuBridge: pause-menu pref \"%s\" = %d at startup (mode left at %d)", s_key, value,
        static_cast<int>(GetStereoMode()));
    return;
  }
  if (value == s_lastValue)
    return;
  s_lastValue = value;

  for (int i = 0; i < g_profileCount; ++i) {
    if (g_profiles[i].value == value) {
      MenuBridgeApplyProfile(i, "pause menu");
      return;
    }
  }
  Log("MenuBridge: pause-menu pref \"%s\" = %d has no profile in gtaiv_dxvk_vr.menumap — ignored",
      s_key, value);
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
