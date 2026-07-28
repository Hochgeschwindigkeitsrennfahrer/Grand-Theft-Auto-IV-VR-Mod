#include "hud_layout.h"

#include "log.h"
#include "stereo_config.h"

#include <Windows.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace asi {
namespace {

// Illustration (red→blue): pull corner HUD inward — NOT screen-center.
// Normalized [HD] deltas applied to stock hud.dat positions.
constexpr float kInL = 0.12f;   // left-edge items → right
constexpr float kInR = 0.22f;   // right-edge items → left
constexpr float kInT = 0.22f;   // top-edge items → further down/center
constexpr float kInB = 0.16f;   // bottom-edge items → up
// Mission objective / “drive to …” bottom-middle text — stronger lift.
constexpr float kInMissionB = 0.28f;
// Wanted stars: further toward screen center than other top-right HUD.
constexpr float kInWantedR = 0.34f;
constexpr float kInWantedT = 0.30f;
// Area (white) + street (green): lift up; street lifts more → tighter under district.
// Stock HD gap 0.040 → ~0.022 after (area −0.20, street −0.218).
constexpr float kInAreaB = 0.20f;
constexpr float kInStreetB = 0.218f;
constexpr float kInVehicleNameB = 0.20f;  // keep above area stack

void InsetXY(bool left, bool right, bool top, bool bottom, float* x, float* y) {
  if (left)
    *x += kInL;
  if (right)
    *x -= kInR;
  if (top)
    *y += kInT;
  if (bottom)
    *y -= kInB;
}

// Per-item overrides after base InsetXY (wanted / area-street spacing).
void ApplyNamedHudInset(const char* name, float stockX, float stockY, bool left, bool right,
                        bool top, bool bottom, float* nx, float* ny) {
  *nx = stockX;
  *ny = stockY;
  if (std::strcmp(name, "HUD_SUBITILES") == 0 ||
      std::strcmp(name, "HUD_LOWERRIGHT_MESSAGE") == 0) {
    *ny = stockY - kInMissionB;
    if (right)
      *nx = stockX - kInR;
    return;
  }
  if (std::strcmp(name, "HUD_WANTED_FRONT") == 0 ||
      std::strcmp(name, "HUD_WANTED_BACK") == 0) {
    *nx = stockX - kInWantedR;
    *ny = stockY + kInWantedT;
    return;
  }
  if (std::strcmp(name, "HUD_AREA_NAME") == 0) {
    *nx = stockX - kInR;
    *ny = stockY - kInAreaB;
    return;
  }
  if (std::strcmp(name, "HUD_STREET_NAME") == 0) {
    *nx = stockX - kInR;
    *ny = stockY - kInStreetB;
    return;
  }
  if (std::strcmp(name, "HUD_VEHICLE_NAME") == 0) {
    *nx = stockX - kInR;
    *ny = stockY - kInVehicleNameB;
    return;
  }
  InsetXY(left, right, top, bottom, nx, ny);
}

bool GetAsiDir(char* out, DWORD n) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&GetAsiDir), &self) ||
      !self)
    return false;
  if (!GetModuleFileNameA(self, out, n))
    return false;
  char* slash = strrchr(out, '\\');
  if (!slash)
    return false;
  slash[1] = 0;
  return true;
}

bool HudDatPath(char* out, DWORD n) {
  if (!GetAsiDir(out, n))
    return false;
  strcat_s(out, n, "common\\data\\hud.dat");
  return true;
}

bool HudDatBakPath(char* out, DWORD n) {
  if (!HudDatPath(out, n))
    return false;
  strcat_s(out, n, ".gtaiv-dxvk-vr.bak");
  return true;
}

bool ReadWholeFile(const char* path, std::string* out) {
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return false;
  fseek(f, 0, SEEK_END);
  const long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 2 * 1024 * 1024) {
    fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(sz));
  const size_t n = fread(&(*out)[0], 1, static_cast<size_t>(sz), f);
  fclose(f);
  out->resize(n);
  return n > 0;
}

bool WriteWholeFile(const char* path, const std::string& text) {
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f)
    return false;
  fwrite(text.data(), 1, text.size(), f);
  fclose(f);
  return true;
}

void EnsureStockBak() {
  char path[MAX_PATH]{};
  char bak[MAX_PATH]{};
  if (!HudDatPath(path, MAX_PATH) || !HudDatBakPath(bak, MAX_PATH))
    return;
  if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    return;
  if (GetFileAttributesA(bak) != INVALID_FILE_ATTRIBUTES)
    return;
  if (CopyFileA(path, bak, TRUE))
    Log("HudLayout: saved stock bak %s", bak);
}

// Replace first x,y pair on lines starting with name.
int ReplaceHudPos(std::string& text, const char* name, float oldX, float oldY, float newX,
                  float newY) {
  int hits = 0;
  size_t search = 0;
  const size_t nameLen = strlen(name);
  char newTok[48]{};
  sprintf_s(newTok, "%.3f,%.3f", newX, newY);

  while ((search = text.find(name, search)) != std::string::npos) {
    const size_t lineStart = text.rfind('\n', search);
    const size_t start = (lineStart == std::string::npos) ? 0 : lineStart + 1;
    // Require name at beginning of line (ignore commented ;NAME).
    if (text.compare(start, nameLen, name) != 0) {
      search += nameLen;
      continue;
    }
    const size_t lineEnd = text.find('\n', search);
    const size_t end = (lineEnd == std::string::npos) ? text.size() : lineEnd;
    std::string line = text.substr(start, end - start);

    // Find first digit of a coordinate pair.
    size_t i = nameLen;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      ++i;
    size_t numStart = i;
    while (i < line.size() &&
           (isdigit(static_cast<unsigned char>(line[i])) || line[i] == '.' || line[i] == '-' ||
            line[i] == '+'))
      ++i;
    if (i >= line.size() || line[i] != ',') {
      search = end;
      continue;
    }
    ++i;  // comma
    while (i < line.size() &&
           (isdigit(static_cast<unsigned char>(line[i])) || line[i] == '.' || line[i] == '-' ||
            line[i] == '+'))
      ++i;
    const size_t numEnd = i;
    float fx = 0.f, fy = 0.f;
    if (sscanf_s(line.c_str() + numStart, "%f,%f", &fx, &fy) < 2) {
      search = end;
      continue;
    }
    // Only rewrite if this line still has the stock (or near-stock) coords.
    if (std::fabs(fx - oldX) > 0.002f || std::fabs(fy - oldY) > 0.002f) {
      search = end;
      continue;
    }
    line.replace(numStart, numEnd - numStart, newTok);
    text.replace(start, end - start, line);
    ++hits;
    search = start + line.size();
  }
  (void)oldX;
  (void)oldY;
  return hits;
}

void ApplyInsetToStockText(std::string& text) {
  struct Item {
    const char* name;
    float x, y;
    // which edges: L/R/T/B
    bool left, right, top, bottom;
  };
  // Stock [HD] gameplay HUD (skip centered crosshair / wasted / credits).
  const Item items[] = {
      {"HUD_RADAR", 0.064f, 0.745f, true, false, false, true},
      {"HUD_HELP_MESSAGE", 0.072f, 0.055f, true, false, true, false},
      {"HUD_HELP_MESSAGE_ICON", 0.005f, 0.008f, true, false, true, false},
      {"HUD_MP_CLOCK", 0.929f, 0.030f, false, true, true, false},
      {"HUD_MP_CASH", 0.929f, 0.030f, false, true, true, false},
      {"HUD_WANTED_FRONT", 0.929f, 0.032f, false, true, true, false},
      {"HUD_WANTED_BACK", 0.929f, 0.032f, false, true, true, false},
      {"HUD_CASH", 0.929f, 0.055f, false, true, true, false},
      {"HUD_MISSION_PASSED_CASH", 0.929f, 0.030f, false, true, true, false},
      {"HUD_AMMO", 0.929f, 0.030f, false, true, true, false},
      {"HUD_WEAPON_ICON", 0.936f, 0.025f, false, true, true, false},
      {"HUD_MISSION_CLOCK", 0.929f, 0.030f, false, true, true, false},
      {"HUD_AREA_NAME", 0.929f, 0.872f, false, true, false, true},
      {"HUD_STREET_NAME", 0.929f, 0.912f, false, true, false, true},
      {"HUD_VEHICLE_NAME", 0.929f, 0.832f, false, true, false, true},
      {"HUD_PHONE_MESSAGE_ICON", 0.106f, 0.716f, true, false, false, true},
      {"HUD_PHONE_MESSAGE_NUMBER", 0.101f, 0.721f, true, false, false, true},
      {"HUD_PHONE_MESSAGE_BOX", 0.069f, 0.714f, true, false, false, true},
      {"HUD_TEXT_MESSAGE_ICON", 0.069f, 0.928f, true, false, false, true},
      {"HUD_SLEEP_MODE_ICON", 0.194f, 0.922f, true, false, false, true},
      {"HUD_MISSION_COUNTER_NAME_1", 0.932f, 0.22f, false, true, true, false},
      {"HUD_MISSION_COUNTER_NUMBER_1", 0.844f, 0.226f, false, true, true, false},
      {"HUD_MISSION_COUNTER_NAME_2", 0.932f, 0.22f, false, true, true, false},
      {"HUD_MISSION_COUNTER_NUMBER_2", 0.844f, 0.226f, false, true, true, false},
      {"HUD_MISSION_COUNTER_NAME_3", 0.932f, 0.22f, false, true, true, false},
      {"HUD_MISSION_COUNTER_NUMBER_3", 0.844f, 0.226f, false, true, true, false},
      {"HUD_MISSION_COUNTER_NAME_4", 0.932f, 0.22f, false, true, true, false},
      {"HUD_MISSION_COUNTER_NUMBER_4", 0.844f, 0.226f, false, true, true, false},
      {"HUD_LOWERRIGHT_MESSAGE", 0.629f, 0.672f, false, true, false, true},
      {"HUD_BIG_MESSAGE_TITLE", 0.929f, 0.957f, false, true, false, true},
      // Bottom-middle mission directions (“drive to Romans…”) — lift toward center.
      {"HUD_SUBITILES", 0.5f, 0.952f, false, false, false, true},
  };

  int total = 0;
  for (const Item& it : items) {
    float nx = it.x;
    float ny = it.y;
    ApplyNamedHudInset(it.name, it.x, it.y, it.left, it.right, it.top, it.bottom, &nx, &ny);
    total += ReplaceHudPos(text, it.name, it.x, it.y, nx, ny);
  }

  // CRT section (second HUD_RADAR etc.) — same name, different stock coords.
  const Item crt[] = {
      {"HUD_RADAR", 0.089f, 0.720f, true, false, false, true},
      {"HUD_HELP_MESSAGE", 0.098f, 0.084f, true, false, true, false},
      {"HUD_HELP_MESSAGE_ICON", 0.005f, 0.011f, true, false, true, false},
      {"HUD_MP_CLOCK", 0.905f, 0.037f, false, true, true, false},
      {"HUD_MP_CASH", 0.905f, 0.037f, false, true, true, false},
      {"HUD_WANTED_FRONT", 0.905f, 0.040f, false, true, true, false},
      {"HUD_WANTED_BACK", 0.905f, 0.040f, false, true, true, false},
      {"HUD_CASH", 0.905f, 0.078f, false, true, true, false},
      {"HUD_MISSION_PASSED_CASH", 0.905f, 0.037f, false, true, true, false},
      {"HUD_AMMO", 0.905f, 0.037f, false, true, true, false},
      {"HUD_WEAPON_ICON", 0.912f, 0.030f, false, true, true, false},
      {"HUD_MISSION_CLOCK", 0.905f, 0.037f, false, true, true, false},
      {"HUD_AREA_NAME", 0.905f, 0.835f, false, true, false, true},
      {"HUD_STREET_NAME", 0.905f, 0.875f, false, true, false, true},
      {"HUD_VEHICLE_NAME", 0.905f, 0.795f, false, true, false, true},
      {"HUD_LOWERRIGHT_MESSAGE", 0.629f, 0.672f, false, true, false, true},
      {"HUD_SUBITILES", 0.505f, 0.9165f, false, false, false, true},
  };
  for (const Item& it : crt) {
    float nx = it.x;
    float ny = it.y;
    ApplyNamedHudInset(it.name, it.x, it.y, it.left, it.right, it.top, it.bottom, &nx, &ny);
    total += ReplaceHudPos(text, it.name, it.x, it.y, nx, ny);
  }
  Log("HudLayout: inset illustration applied to hud.dat text (hits=%d) "
      "wantedR=-%.2f areaB=-%.2f streetB=-%.2f",
      total, kInWantedR, kInAreaB, kInStreetB);
}

// Live memory poke so HUD moves without needing pause-menu reload of hud.dat.
struct MemSlot {
  float* p = nullptr;
  float stockX = 0.f;
  float stockY = 0.f;
  bool left = false, right = false, top = false, bottom = false;
};
constexpr int kMaxMem = 48;
MemSlot g_mem[kMaxMem]{};
int g_memCount = 0;
bool g_memScanned = false;

bool FloatNear(float a, float b) {
  return std::fabs(a - b) < 0.0002f;
}

void ScanHudMemOnce() {
  if (g_memScanned)
    return;
  g_memScanned = true;
  g_memCount = 0;

  struct Want {
    float x, y;
    bool left, right, top, bottom;
  };
  const Want want[] = {
      {0.064f, 0.745f, true, false, false, true},   // radar
      {0.072f, 0.055f, true, false, true, false},   // help
      {0.005f, 0.008f, true, false, true, false},   // help icon
      {0.929f, 0.030f, false, true, true, false},   // clock/ammo cluster
      {0.929f, 0.032f, false, true, true, false},   // wanted
      {0.929f, 0.055f, false, true, true, false},   // cash
      {0.936f, 0.025f, false, true, true, false},   // weapon
      {0.929f, 0.872f, false, true, false, true},   // area
      {0.929f, 0.912f, false, true, false, true},   // street
      {0.929f, 0.832f, false, true, false, true},   // vehicle name
      {0.629f, 0.672f, false, true, false, true},   // lower-right / objective
      {0.5f, 0.952f, false, false, false, true},    // HUD_SUBITILES mission text
  };

  HMODULE mod = GetModuleHandleA(nullptr);
  if (!mod)
    return;
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return;
  const DWORD size = nt->OptionalHeader.SizeOfImage;
  auto* base = reinterpret_cast<uint8_t*>(mod);
  for (DWORD i = 0; i + 8 < size && g_memCount < kMaxMem; i += 4) {
    auto* f = reinterpret_cast<float*>(base + i);
    for (const Want& w : want) {
      if (!FloatNear(f[0], w.x) || !FloatNear(f[1], w.y))
        continue;
      bool dup = false;
      for (int j = 0; j < g_memCount; ++j) {
        if (g_mem[j].p == f) {
          dup = true;
          break;
        }
      }
      if (dup)
        break;
      MemSlot& s = g_mem[g_memCount++];
      s.p = f;
      s.stockX = w.x;
      s.stockY = w.y;
      s.left = w.left;
      s.right = w.right;
      s.top = w.top;
      s.bottom = w.bottom;
      break;
    }
  }
  Log("HudLayout: mem-scan slots=%d (live poke avoids pause-menu jump)", g_memCount);
}

void ApplyHudMemInset() {
  ScanHudMemOnce();
  for (int i = 0; i < g_memCount; ++i) {
    MemSlot& s = g_mem[i];
    if (!s.p)
      continue;
    float nx = s.stockX;
    float ny = s.stockY;
    // Match by stock coords (mem scan has no names).
    if (std::fabs(s.stockX - 0.629f) < 0.01f && std::fabs(s.stockY - 0.672f) < 0.01f) {
      ApplyNamedHudInset("HUD_LOWERRIGHT_MESSAGE", s.stockX, s.stockY, s.left, s.right,
                         s.top, s.bottom, &nx, &ny);
    } else if (std::fabs(s.stockX - 0.5f) < 0.01f &&
               std::fabs(s.stockY - 0.952f) < 0.01f) {
      ApplyNamedHudInset("HUD_SUBITILES", s.stockX, s.stockY, s.left, s.right, s.top,
                         s.bottom, &nx, &ny);
    } else if (std::fabs(s.stockX - 0.929f) < 0.01f &&
               std::fabs(s.stockY - 0.032f) < 0.002f) {
      ApplyNamedHudInset("HUD_WANTED_FRONT", s.stockX, s.stockY, s.left, s.right, s.top,
                         s.bottom, &nx, &ny);
    } else if (std::fabs(s.stockX - 0.929f) < 0.01f &&
               std::fabs(s.stockY - 0.872f) < 0.002f) {
      ApplyNamedHudInset("HUD_AREA_NAME", s.stockX, s.stockY, s.left, s.right, s.top,
                         s.bottom, &nx, &ny);
    } else if (std::fabs(s.stockX - 0.929f) < 0.01f &&
               std::fabs(s.stockY - 0.912f) < 0.002f) {
      ApplyNamedHudInset("HUD_STREET_NAME", s.stockX, s.stockY, s.left, s.right, s.top,
                         s.bottom, &nx, &ny);
    } else if (std::fabs(s.stockX - 0.929f) < 0.01f &&
               std::fabs(s.stockY - 0.832f) < 0.002f) {
      ApplyNamedHudInset("HUD_VEHICLE_NAME", s.stockX, s.stockY, s.left, s.right, s.top,
                         s.bottom, &nx, &ny);
    } else {
      InsetXY(s.left, s.right, s.top, s.bottom, &nx, &ny);
    }
    DWORD old = 0;
    if (VirtualProtect(s.p, 8, PAGE_READWRITE, &old)) {
      s.p[0] = nx;
      s.p[1] = ny;
      VirtualProtect(s.p, 8, old, &old);
    }
  }
  static bool once = false;
  if (!once && g_memCount > 0) {
    once = true;
    Log("HudLayout: live mem inset ON (wantedR=-%.2f areaB=-%.2f streetB=-%.2f)",
        kInWantedR, kInAreaB, kInStreetB);
  }
}

}  // namespace

void RestoreHudDatStock() {
  char path[MAX_PATH]{};
  char bak[MAX_PATH]{};
  if (!HudDatPath(path, MAX_PATH) || !HudDatBakPath(bak, MAX_PATH))
    return;
  if (GetFileAttributesA(bak) == INVALID_FILE_ATTRIBUTES) {
    Log("HudLayout: no bak to restore (%s)", bak);
    return;
  }
  if (CopyFileA(bak, path, FALSE))
    Log("HudLayout: restored STOCK hud.dat from bak (all modes use corners again)");
  else
    Log("HudLayout: FAILED restore stock hud.dat");
}

void EnsureHudOffDefaults() {
  // Mode 166 only: write VR-inset hud.dat from stock bak. Does NOT stay if you
  // later RestoreHudDatStock() (kill→162 / leave 166).
  EnsureStockBak();
  char path[MAX_PATH]{};
  char bak[MAX_PATH]{};
  if (!HudDatPath(path, MAX_PATH) || !HudDatBakPath(bak, MAX_PATH))
    return;
  std::string text;
  if (!ReadWholeFile(bak, &text)) {
    if (!ReadWholeFile(path, &text)) {
      Log("HudLayout: cannot read hud.dat");
      return;
    }
  }
  ApplyInsetToStockText(text);
  if (WriteWholeFile(path, text))
    Log("HudLayout: wrote Mode166 VR-inset hud.dat (T=+%.2f); live mem poke also ON", kInT);
  else
    Log("HudLayout: FAILED writing hud.dat");
  g_memScanned = false;
  g_memCount = 0;
  ApplyHudMemInset();
}

void UpdateHudLayoutForVr() {
  if (!IsOursFpHudLayout(GetStereoMode()))
    return;
  // Every EndScene — applies before pause menu; avoids “jumps only after menu”.
  ApplyHudMemInset();
}

}  // namespace asi
