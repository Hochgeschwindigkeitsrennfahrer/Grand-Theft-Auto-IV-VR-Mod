#include "vr_menu.h"

#include "log.h"
#include "menu_bridge.h"
#include "menu_draw.h"
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

// --- keys -------------------------------------------------------------------
// F4..F11 are taken by the existing hotkeys (F4 profile cycle, F5 vres,
// F6 stereoscale, F7 worldscale, F8 ipd, F9 recenter, F10 cam, F11 truescale),
// F12 is Steam's screenshot. F3 is free on both sides.
constexpr int kToggleKey = VK_F3;
const int kUpKeys[] = {VK_UP, VK_NUMPAD8};
const int kDownKeys[] = {VK_DOWN, VK_NUMPAD2};
const int kLeftKeys[] = {VK_LEFT, VK_NUMPAD4};
const int kRightKeys[] = {VK_RIGHT, VK_NUMPAD6};
const int kEnterKeys[] = {VK_RETURN, VK_NUMPAD5};

// --- colours (ARGB) ---------------------------------------------------------
constexpr D3DCOLOR kColShadow = 0xB0000000;
constexpr D3DCOLOR kColPanel = 0xE00E1319;
constexpr D3DCOLOR kColBorder = 0xFF3C82F6;
constexpr D3DCOLOR kColTitle = 0xFFFFFFFF;
constexpr D3DCOLOR kColStatus = 0xFF8FA3B8;
constexpr D3DCOLOR kColLabel = 0xFFC8D2DC;
constexpr D3DCOLOR kColLabelSel = 0xFFFFFFFF;
constexpr D3DCOLOR kColValue = 0xFF7FD1A0;
constexpr D3DCOLOR kColSelBg = 0xFF1D4E89;
constexpr D3DCOLOR kColHelp = 0xFF7E8A96;
constexpr D3DCOLOR kColAction = 0xFFE8C170;

enum class Kind { Value, Preset, Action, Gap };

struct Row {
  Kind kind;
  const char* label;
  const char* help;
  int (*get)();
  void (*set)(int);
  int lo;
  int hi;
  int step;
  const char* unit;
  const char* (*presetName)(int);
  int (*presetCount)();
  bool noRepeat;  // heavy rows (mode reload) must not fire on key auto-repeat
  void (*action)();
};

bool g_open = false;
int g_sel = 0;
int g_enabled = -1;  // -1 = not read yet
char g_buildId[48]{};

// --- small local file helpers (same idiom as stereo_config / menu_bridge) ----
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

bool Enabled() {
  if (g_enabled >= 0)
    return g_enabled != 0;
  char buf[8]{};
  g_enabled = 1;
  if (ReadTextFile("gtaiv_dxvk_vr.menu", buf, sizeof(buf)) > 0 && buf[0] == '0')
    g_enabled = 0;
  if (ReadTextFile("gtaiv_dxvk_vr.buildid", g_buildId, sizeof(g_buildId)) > 0) {
    for (char* p = g_buildId; *p; ++p)
      if (*p == '\r' || *p == '\n') {
        *p = 0;
        break;
      }
  }
  Log("VrMenu: overlay %s — F3 opens it (kill: put a single 0 in gtaiv_dxvk_vr.menu)",
      g_enabled ? "ON" : "OFF");
  return g_enabled != 0;
}

// --- row adapters -----------------------------------------------------------
int GetProfile() {
  const int i = MenuBridgeCurrentProfile();
  return i < 0 ? 0 : i;
}
void SetProfile(int i) {
  MenuBridgeApplyProfile(i, "menu");
}
const char* ProfileName(int i) {
  return MenuBridgeProfileLabel(i);
}
int ProfileCount() {
  return MenuBridgeProfileCount();
}

int GetWsPreset() {
  return MenuGetWorldScalePreset();
}
void SetWsPreset(int i) {
  MenuSetWorldScalePreset(i);
}
const char* WsPresetName(int i) {
  return MenuWorldScalePresetName(i);
}
int WsPresetCount() {
  return MenuWorldScalePresetCount();
}

int GetVres() {
  return static_cast<int>(GetCanvasMaxDim());
}
void SetVres(int v) {
  SetCanvasMaxDim(static_cast<uint32_t>(v), false);
}

int GetFpFov() {
  return static_cast<int>(GetFpForwardFovDegrees() + 0.5f);
}
void SetFpFov(int v) {
  // Only the forward FOV is exposed; rear and foot keep whatever they had.
  ForceFpFovDegrees(v, static_cast<int>(GetFpRearFovDegrees() + 0.5f),
                    static_cast<int>(GetFpFootFovDegrees() + 0.5f));
}

int GetEyeFwd() {
  return MenuGetEyeForwardCm();
}
void SetEyeFwd(int v) {
  SetEyeForwardCm(v);
}

void ActReloadFiles() {
  ReloadStereoMode();
  Log("VrMenu: re-read gtaiv_dxvk_vr.stereo — live mode is now %d",
      static_cast<int>(GetStereoMode()));
}

void ActClose() {
  g_open = false;
  Log("VrMenu: closed");
}

const Row kRows[] = {
    {Kind::Preset, "VR render mode", "Stereo family. Most only fully arm after a game restart.",
     &GetProfile, &SetProfile, 0, 0, 1, nullptr, &ProfileName, &ProfileCount, true, nullptr},
    {Kind::Value, "Eye separation (F8)", "Stereo baseline. Lower fuses easier, higher looks smaller.",
     &MenuGetSepCm, &MenuSetSepCm, 1, 12, 1, "cm", nullptr, nullptr, false, nullptr},
    {Kind::Value, "Stereo strength (F6)", "Disparity multiplier. Higher = stronger 3D, smaller world.",
     &MenuGetStereoScalePercent, &MenuSetStereoScalePercent, 100, 140, 5, "%", nullptr, nullptr,
     false, nullptr},
    {Kind::Preset, "Framing preset (F7)", "Zoom vs open. Sets FOV add + stereo strength together.",
     &GetWsPreset, &SetWsPreset, 0, 0, 1, nullptr, &WsPresetName, &WsPresetCount, false, nullptr},
    {Kind::Value, "6DoF move scale (F11)", "How far the world moves when your head does. Not framing.",
     &MenuGetWorldScalePercent, &MenuSetWorldScalePercent, 50, 200, 5, "%", nullptr, nullptr, false,
     nullptr},
    {Kind::Value, "FOV add", "Canvas crop. IGNORED on modes past 162 - use first-person FOV.",
     &MenuGetFovAddDegrees, &MenuSetFovAddDegrees, 0, 40, 1, "deg", nullptr, nullptr, false,
     nullptr},
    {Kind::Value, "Eye canvas (F5)", "Per-eye render target cap. Costs GPU. No effect on flat.",
     &GetVres, &SetVres, 1024, 2048, 128, "px", nullptr, nullptr, false, nullptr},
    {Kind::Value, "Eye forward", "Camera offset out of the skull. 0 = you see your own jacket.",
     &GetEyeFwd, &SetEyeFwd, 0, 60, 2, "cm", nullptr, nullptr, false, nullptr},
    {Kind::Value, "First-person FOV", "The live FOV knob on modes past 162 (gtaiv_dxvk_vr.fpfov).",
     &GetFpFov, &SetFpFov, 60, 130, 5, "deg", nullptr, nullptr, false, nullptr},
    {Kind::Gap, "", "", nullptr, nullptr, 0, 0, 0, nullptr, nullptr, nullptr, false, nullptr},
    {Kind::Action, "Re-read settings files", "Re-reads gtaiv_dxvk_vr.stereo from disk.", nullptr,
     nullptr, 0, 0, 0, nullptr, nullptr, nullptr, true, &ActReloadFiles},
    {Kind::Action, "Close", "Same as pressing F3.", nullptr, nullptr, 0, 0, 0, nullptr, nullptr,
     nullptr, true, &ActClose},
};
constexpr int kRowN = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

bool Selectable(int i) {
  return i >= 0 && i < kRowN && kRows[i].kind != Kind::Gap;
}

void MoveSel(int delta) {
  for (int n = 0; n < kRowN; ++n) {
    g_sel = (g_sel + delta + kRowN) % kRowN;
    if (Selectable(g_sel))
      return;
  }
}

// --- input ------------------------------------------------------------------
struct Repeat {
  bool down = false;
  DWORD next = 0;
};

bool AnyDown(const int* vks, int n) {
  for (int i = 0; i < n; ++i)
    if (GetAsyncKeyState(vks[i]) & 0x8000)
      return true;
  return false;
}

// Edge on press, then auto-repeat after a hold delay. `allowRepeat` off means
// the caller only ever gets the initial edge.
bool Fired(const int* vks, int n, Repeat* st, bool allowRepeat) {
  const bool down = AnyDown(vks, n);
  if (!down) {
    st->down = false;
    return false;
  }
  const DWORD now = GetTickCount();
  if (!st->down) {
    st->down = true;
    st->next = now + 450;
    return true;
  }
  if (allowRepeat && now >= st->next) {
    st->next = now + 70;
    return true;
  }
  return false;
}

bool Edge(int vk, bool* was) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool edge = down && !*was;
  *was = down;
  return edge;
}

bool GameHasFocus(IDirect3DDevice9* dev) {
  D3DDEVICE_CREATION_PARAMETERS cp{};
  if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow)
    return true;  // unknown window — better to work than to lock the user out
  const HWND fg = GetForegroundWindow();
  if (!fg)
    return false;
  return fg == cp.hFocusWindow || GetAncestor(fg, GA_ROOT) == GetAncestor(cp.hFocusWindow, GA_ROOT);
}

void Adjust(const Row& r, int dir, bool fast) {
  if (!r.get || !r.set)
    return;
  if (r.kind == Kind::Preset) {
    const int n = r.presetCount ? r.presetCount() : 0;
    if (n <= 0)
      return;
    r.set(((r.get() + dir) % n + n) % n);
    return;
  }
  const int step = r.step * (fast ? 5 : 1);
  int v = r.get() + dir * (step > 0 ? step : 1);
  if (v < r.lo)
    v = r.lo;
  if (v > r.hi)
    v = r.hi;
  r.set(v);
}

void TickInput(IDirect3DDevice9* dev) {
  static bool s_wasToggle = false;

  if (!GameHasFocus(dev)) {
    // Drop held state so an alt-tab does not leave a key latched.
    s_wasToggle = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0;
    return;
  }

  if (Edge(kToggleKey, &s_wasToggle)) {
    g_open = !g_open;
    if (g_open && !Selectable(g_sel))
      MoveSel(1);
    Log("VrMenu: %s (F3)", g_open ? "opened" : "closed");
  }
  if (!g_open)
    return;

  static Repeat s_up, s_down, s_left, s_right;
  static Repeat s_enter;

  if (Fired(kUpKeys, 2, &s_up, true))
    MoveSel(-1);
  if (Fired(kDownKeys, 2, &s_down, true))
    MoveSel(1);

  const Row& row = kRows[g_sel];
  const bool fast = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool repeatOk = !row.noRepeat;
  if (Fired(kLeftKeys, 2, &s_left, repeatOk))
    Adjust(row, -1, fast);
  if (Fired(kRightKeys, 2, &s_right, repeatOk))
    Adjust(row, +1, fast);
  if (Fired(kEnterKeys, 2, &s_enter, false)) {
    if (row.kind == Kind::Action && row.action)
      row.action();
    else
      Adjust(row, +1, fast);
  }
}

// --- drawing ----------------------------------------------------------------
void FormatValue(const Row& r, char* out, size_t cap) {
  out[0] = 0;
  if (!r.get)
    return;
  if (r.kind == Kind::Preset) {
    const int i = r.get();
    sprintf_s(out, cap, "%s", r.presetName ? r.presetName(i) : "?");
  } else if (r.kind == Kind::Value) {
    if (r.unit)
      sprintf_s(out, cap, "%d %s", r.get(), r.unit);
    else
      sprintf_s(out, cap, "%d", r.get());
  }
}

void DrawPanel() {
  const int lh = MenuFontHeight();
  if (lh <= 0)
    return;
  const int pad = lh;
  const int rowH = lh + lh / 4;
  const int gapW = MenuTextWidth("     ");

  char title[96];
  sprintf_s(title, "GTA IV VR  -  settings%s%s", g_buildId[0] ? "   build " : "", g_buildId);

  char status[128];
  sprintf_s(status, "stereo %d   %s   %dx%d", static_cast<int>(GetStereoMode()),
            vr::VRSystem() ? "SteamVR: connected" : "SteamVR: not running (flat)", MenuDrawWidth(),
            MenuDrawHeight());

  const char* footer1 = "Up/Down or Num8/Num2 select      Left/Right or Num4/Num6 change";
  const char* footer2 = "Shift = x5      Enter or Num5 = apply/run      F3 = close";

  // Width: widest of everything we are about to draw.
  int labelW = 0, valueW = 0, helpW = 0;
  char buf[96];
  for (int i = 0; i < kRowN; ++i) {
    const int w = MenuTextWidth(kRows[i].label);
    if (w > labelW)
      labelW = w;
    FormatValue(kRows[i], buf, sizeof(buf));
    // "< " and " >" are drawn around the value of the selected row.
    char deco[128];
    sprintf_s(deco, "< %s >", buf);
    const int vw = MenuTextWidth(deco);
    if (vw > valueW)
      valueW = vw;
    const int hw = MenuTextWidth(kRows[i].help);
    if (hw > helpW)
      helpW = hw;
  }
  int inner = labelW + gapW + valueW;
  const int candidates[] = {MenuTextWidth(title), MenuTextWidth(status), MenuTextWidth(footer1),
                            MenuTextWidth(footer2), helpW};
  for (int i = 0; i < 5; ++i)
    if (candidates[i] > inner)
      inner = candidates[i];

  int panelW = inner + pad * 2;
  const int maxW = MenuDrawWidth() - lh * 2;
  if (maxW > 0 && panelW > maxW)
    panelW = maxW;

  const int headerH = rowH * 2 + lh / 2;
  // help line + two footer lines + the quarter-line of air above the help
  const int footerH = rowH * 2 + lh + lh / 4;
  const int panelH = pad * 2 + headerH + kRowN * rowH + footerH;

  const float px = static_cast<float>((MenuDrawWidth() - panelW) / 2);
  const float py = static_cast<float>((MenuDrawHeight() - panelH) / 2);
  const float pw = static_cast<float>(panelW);
  const float ph = static_cast<float>(panelH);

  // Drop shadow, body, 2px accent border.
  MenuDrawRect(px + 6.f, py + 6.f, pw, ph, kColShadow);
  MenuDrawRect(px, py, pw, ph, kColPanel);
  MenuDrawRect(px, py, pw, 2.f, kColBorder);
  MenuDrawRect(px, py + ph - 2.f, pw, 2.f, kColBorder);
  MenuDrawRect(px, py, 2.f, ph, kColBorder);
  MenuDrawRect(px + pw - 2.f, py, 2.f, ph, kColBorder);

  float y = py + pad;
  const float x = px + pad;
  MenuDrawText(x, y, kColTitle, title);
  y += rowH;
  MenuDrawText(x, y, kColStatus, status);
  y += rowH;
  MenuDrawRect(x, y + lh / 4.f, pw - pad * 2.f, 1.f, 0x40FFFFFF);
  y += lh / 2.f;

  for (int i = 0; i < kRowN; ++i) {
    const Row& r = kRows[i];
    const bool sel = (i == g_sel);
    if (r.kind == Kind::Gap) {
      y += rowH;
      continue;
    }
    if (sel)
      MenuDrawRect(px + 2.f, y - lh / 8.f, pw - 4.f, static_cast<float>(rowH), kColSelBg);

    const D3DCOLOR labelCol =
        (r.kind == Kind::Action) ? kColAction : (sel ? kColLabelSel : kColLabel);
    MenuDrawText(x, y, labelCol, r.label);

    if (r.kind != Kind::Action) {
      FormatValue(r, buf, sizeof(buf));
      char deco[128];
      if (sel)
        sprintf_s(deco, "< %s >", buf);
      else
        sprintf_s(deco, "  %s  ", buf);
      const float vx = px + pw - pad - MenuTextWidth(deco);
      MenuDrawText(vx, y, sel ? kColLabelSel : kColValue, deco);
    }
    y += rowH;
  }

  y += lh / 4.f;
  if (Selectable(g_sel))
    MenuDrawText(x, y, kColHelp, kRows[g_sel].help);
  y += rowH;
  MenuDrawText(x, y, kColHelp, footer1);
  y += static_cast<float>(lh);
  MenuDrawText(x, y, kColHelp, footer2);
}

}  // namespace

bool VrMenuIsOpen() {
  return g_open;
}

void VrMenuSetOpen(bool open) {
  g_open = open;
  if (open && !Selectable(g_sel))
    MoveSel(1);
}

void VrMenuOnEndScene(IDirect3DDevice9* dev) {
  if (!dev || !Enabled())
    return;

  MenuBridgeTickHotkey();
  MenuBridgeTickPrefFile();
  TickInput(dev);

  if (!g_open)
    return;
  if (!MenuDrawBegin(dev))
    return;
  DrawPanel();
  MenuDrawEnd();
}

}  // namespace asi
