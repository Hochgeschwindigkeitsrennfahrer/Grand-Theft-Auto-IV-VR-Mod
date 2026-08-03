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

// VR HUD: Money → Wanted → Ammo → Weapon (stacked), pulled toward center.
// Radar also moved inward (right from left edge). Absolute HD/CRT targets —
// do NOT blind-scan unrelated floats (that crashed hud3).

struct AbsPos {
  const char* name;
  float hdX, hdY;
  float crtX, crtY;
};

// Illustration targets (normalized 0–1). Tune here only.
constexpr AbsPos kAbs[] = {
    {"HUD_RADAR", 0.300f, 0.580f, 0.320f, 0.560f},
    {"HUD_CASH", 0.680f, 0.155f, 0.655f, 0.175f},
    {"HUD_WANTED_FRONT", 0.680f, 0.200f, 0.655f, 0.220f},
    {"HUD_WANTED_BACK", 0.680f, 0.200f, 0.655f, 0.220f},
    {"HUD_AMMO", 0.680f, 0.255f, 0.655f, 0.275f},
    {"HUD_WEAPON_ICON", 0.685f, 0.315f, 0.660f, 0.335f},
    {"HUD_MP_CASH", 0.680f, 0.155f, 0.655f, 0.175f},
    {"HUD_MP_CLOCK", 0.680f, 0.130f, 0.655f, 0.150f},
    {"HUD_MISSION_CLOCK", 0.680f, 0.130f, 0.655f, 0.150f},
    {"HUD_MISSION_PASSED_CASH", 0.680f, 0.130f, 0.655f, 0.150f},
    {"HUD_HELP_MESSAGE", 0.192f, 0.275f, 0.218f, 0.304f},
    {"HUD_AREA_NAME", 0.709f, 0.672f, 0.685f, 0.635f},
    {"HUD_STREET_NAME", 0.709f, 0.694f, 0.685f, 0.657f},
    {"HUD_VEHICLE_NAME", 0.709f, 0.632f, 0.685f, 0.595f},
};

// Stock coords used only as ReplaceHudPos match keys (from stock hud.dat).
struct StockKey {
  const char* name;
  float hdX, hdY;
  float crtX, crtY;
};
constexpr StockKey kStock[] = {
    {"HUD_RADAR", 0.064f, 0.745f, 0.089f, 0.720f},
    {"HUD_CASH", 0.929f, 0.055f, 0.905f, 0.078f},
    {"HUD_WANTED_FRONT", 0.929f, 0.032f, 0.905f, 0.040f},
    {"HUD_WANTED_BACK", 0.929f, 0.032f, 0.905f, 0.040f},
    {"HUD_AMMO", 0.929f, 0.030f, 0.905f, 0.037f},
    {"HUD_WEAPON_ICON", 0.936f, 0.025f, 0.912f, 0.030f},
    {"HUD_MP_CASH", 0.929f, 0.030f, 0.905f, 0.037f},
    {"HUD_MP_CLOCK", 0.929f, 0.030f, 0.905f, 0.037f},
    {"HUD_MISSION_CLOCK", 0.929f, 0.030f, 0.905f, 0.037f},
    {"HUD_MISSION_PASSED_CASH", 0.929f, 0.030f, 0.905f, 0.037f},
    {"HUD_HELP_MESSAGE", 0.072f, 0.055f, 0.098f, 0.084f},
    {"HUD_AREA_NAME", 0.929f, 0.872f, 0.905f, 0.835f},
    {"HUD_STREET_NAME", 0.929f, 0.912f, 0.905f, 0.875f},
    {"HUD_VEHICLE_NAME", 0.929f, 0.832f, 0.905f, 0.795f},
};

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
    if (text.compare(start, nameLen, name) != 0) {
      search += nameLen;
      continue;
    }
    const size_t lineEnd = text.find('\n', search);
    const size_t end = (lineEnd == std::string::npos) ? text.size() : lineEnd;
    std::string line = text.substr(start, end - start);

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
    ++i;
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
    // Match stock OR already-patched coords (idempotent).
    const bool stockish =
        (std::fabs(fx - oldX) < 0.002f && std::fabs(fy - oldY) < 0.002f) ||
        (std::fabs(fx - newX) < 0.002f && std::fabs(fy - newY) < 0.002f);
    if (!stockish) {
      search = end;
      continue;
    }
    line.replace(numStart, numEnd - numStart, newTok);
    text.replace(start, end - start, line);
    ++hits;
    search = start + line.size();
    break;  // one hit per call (HD/CRT invoked separately)
  }
  return hits;
}

void ApplyAbsoluteStack(std::string& text) {
  int total = 0;
  for (size_t i = 0; i < sizeof(kAbs) / sizeof(kAbs[0]); ++i) {
    const AbsPos& a = kAbs[i];
    const StockKey& s = kStock[i];
    total += ReplaceHudPos(text, a.name, s.hdX, s.hdY, a.hdX, a.hdY);
    total += ReplaceHudPos(text, a.name, s.crtX, s.crtY, a.crtX, a.crtY);
  }
  Log("HudLayout: absolute stack applied (hits=%d) cash/wanted/ammo/weapon + radar center",
      total);
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
  ApplyAbsoluteStack(text);
  if (WriteWholeFile(path, text))
    Log("HudLayout: wrote VR stack hud.dat (money→stars→ammo→weapon, radar inset)");
  else
    Log("HudLayout: FAILED writing hud.dat");
}

void UpdateHudLayoutForVr() {
  // File-based layout only — no live whole-image float poking (unsafe).
  (void)IsOursFpHudLayout(GetStereoMode());
}

}  // namespace asi
