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
  char label[40]{};
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
    {243, "243 checkpoint"},
    {271, "271 AER"},
    {920, "920 DirectBounds"},
    {937, "937 ParentExact"},
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
  // LIVE render profiles. Override via gtaiv_dxvk_vr.menumap.
  static const int kDefaultModes[] = {243, 271, 920, 937};
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

// --- FusionFix cfg follow -------------------------------------------------
// A pause-menu row's value reaches us through plugins\GTAIV.EFLC.FusionFix.cfg.
// Which cfg key a given PREF_ writes is NOT guessable — it is whatever FusionFix
// named it — so every cfg key change is logged. One run of the game with the
// row moved tells you the key to put in gtaiv_dxvk_vr.menufollow.

enum class Follow { Mode, Ipd, Fov };

struct FollowRow {
  const char* tag;  // prefix in gtaiv_dxvk_vr.menufollow
  Follow what;
  char key[48];
  int last;
};

FollowRow g_follow[] = {
    {"mode=", Follow::Mode, {}, INT_MIN},
    {"ipd=", Follow::Ipd, {}, INT_MIN},
    {"fov=", Follow::Fov, {}, INT_MIN},
};
constexpr int kFollowN = static_cast<int>(sizeof(g_follow) / sizeof(g_follow[0]));

struct CfgPair {
  char key[48];
  int value;
};
CfgPair g_cfgPrev[96];
int g_cfgPrevN = 0;

// Copies "Key" out of "  Key = 7  ". Returns false when the line is not a pair.
bool ParseCfgLine(const char* line, const char* eol, char* outKey, size_t cap, int* outValue) {
  while (*line == ' ' || *line == '\t')
    ++line;
  if (*line == '[' || *line == ';' || *line == '\r' || *line == '\n' || !*line)
    return false;
  const char* eq = strchr(line, '=');
  if (!eq || (eol && eq > eol))
    return false;
  const char* keyEnd = eq;
  while (keyEnd > line && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t'))
    --keyEnd;
  const size_t len = static_cast<size_t>(keyEnd - line);
  if (len == 0 || len >= cap)
    return false;
  memcpy(outKey, line, len);
  outKey[len] = 0;
  const char* v = eq + 1;
  while (*v == ' ' || *v == '\t')
    ++v;
  *outValue = atoi(v);
  return true;
}

void ReadFollowConfig() {
  char buf[256]{};
  if (ReadTextFile("gtaiv_dxvk_vr.menufollow", buf, sizeof(buf)) == 0) {
    // Back-compat with the single-row file this started as.
    char legacy[64]{};
    const size_t n = ReadTextFile("gtaiv_dxvk_vr.menukey", legacy, sizeof(legacy));
    size_t end = n;
    while (end > 0 && (legacy[end - 1] == '\r' || legacy[end - 1] == '\n' ||
                       legacy[end - 1] == ' ' || legacy[end - 1] == '\t'))
      --end;
    if (end > 0 && end < sizeof(g_follow[0].key)) {
      memcpy(g_follow[0].key, legacy, end);
      Log("MenuBridge: following pause-menu mode pref \"%s\" (legacy .menukey)",
          g_follow[0].key);
    }
    return;
  }
  for (const char* p = buf; *p;) {
    const char* eol = strchr(p, '\n');
    for (int i = 0; i < kFollowN; ++i) {
      const size_t tagLen = strlen(g_follow[i].tag);
      if (_strnicmp(p, g_follow[i].tag, tagLen) != 0)
        continue;
      const char* v = p + tagLen;
      size_t len = 0;
      while (v[len] && v[len] != '\r' && v[len] != '\n' && v[len] != ' ')
        ++len;
      if (len > 0 && len < sizeof(g_follow[i].key)) {
        memcpy(g_follow[i].key, v, len);
        g_follow[i].key[len] = 0;
        Log("MenuBridge: following pause-menu %s\"%s\"", g_follow[i].tag, g_follow[i].key);
      }
    }
    if (!eol)
      break;
    p = eol + 1;
  }
}

void ApplyFollow(FollowRow* r, int value) {
  switch (r->what) {
    case Follow::Mode:
      for (int i = 0; i < g_profileCount; ++i) {
        if (g_profiles[i].value == value) {
          MenuBridgeApplyProfile(i, "pause menu");
          return;
        }
      }
      Log("MenuBridge: pause-menu \"%s\" = %d has no profile in gtaiv_dxvk_vr.menumap — ignored",
          r->key, value);
      return;
    case Follow::Ipd: {
      // The row runs scaler=13 with a VALUE_SLIDERBAR, so the number the player
      // sees IS the value here — map it straight to centimetres. Floor at 1:
      // a 0 cm baseline is mono, not stereo.
      const int cm = value < 1 ? 1 : value;
      MenuSetSepCm(cm);
      Log("MenuBridge: pause menu set eye separation %d cm", cm);
      return;
    }
    case Follow::Fov: {
      // Slider 0..9 -> 70..115 deg in 5 deg steps.
      const int deg = 70 + value * 5;
      ForceFpFovDegrees(deg, static_cast<int>(GetFpRearFovDegrees() + 0.5f),
                        static_cast<int>(GetFpFootFovDegrees() + 0.5f));
      Log("MenuBridge: pause menu set first-person FOV %d deg (slider %d)", deg, value);
      return;
    }
  }
}

void MenuBridgeTickPrefFile() {
  MenuBridgeInit();

  static bool s_read = false;
  static DWORD s_lastPoll = 0;
  static FILETIME s_lastWrite{};

  if (!s_read) {
    s_read = true;
    ReadFollowConfig();
  }

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
  const bool first = (s_lastWrite.dwLowDateTime == 0 && s_lastWrite.dwHighDateTime == 0);
  s_lastWrite = fad.ftLastWriteTime;

  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return;
  char cfg[8192]{};
  const size_t n = fread(cfg, 1, sizeof(cfg) - 1, f);
  fclose(f);
  cfg[n] = 0;

  CfgPair now_[96];
  int nowN = 0;
  for (const char* p = cfg; *p && nowN < 96;) {
    const char* eol = strchr(p, '\n');
    if (ParseCfgLine(p, eol, now_[nowN].key, sizeof(now_[nowN].key), &now_[nowN].value))
      ++nowN;
    if (!eol)
      break;
    p = eol + 1;
  }

  // Report what moved. This is how you discover which cfg key a repurposed
  // PREF_ actually writes — toggle the row, read the log.
  if (!first) {
    for (int i = 0; i < nowN; ++i) {
      for (int j = 0; j < g_cfgPrevN; ++j) {
        if (_stricmp(now_[i].key, g_cfgPrev[j].key) == 0) {
          if (now_[i].value != g_cfgPrev[j].value)
            Log("MenuBridge: FusionFix.cfg %s %d -> %d", now_[i].key, g_cfgPrev[j].value,
                now_[i].value);
          break;
        }
      }
    }
  }
  memcpy(g_cfgPrev, now_, sizeof(CfgPair) * static_cast<size_t>(nowN));
  g_cfgPrevN = nowN;

  for (int r = 0; r < kFollowN; ++r) {
    if (!g_follow[r].key[0])
      continue;
    int value = INT_MIN;
    for (int i = 0; i < nowN; ++i) {
      if (_stricmp(now_[i].key, g_follow[r].key) == 0) {
        value = now_[i].value;
        break;
      }
    }
    if (value == INT_MIN)
      continue;
    if (g_follow[r].last == INT_MIN) {
      // First sighting is adopted, never applied: the gtaiv_dxvk_vr.* files
      // still decide everything at startup.
      g_follow[r].last = value;
      Log("MenuBridge: pause-menu \"%s\" = %d at startup (not applied)", g_follow[r].key, value);
      continue;
    }
    if (value == g_follow[r].last)
      continue;
    g_follow[r].last = value;
    ApplyFollow(&g_follow[r], value);
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
