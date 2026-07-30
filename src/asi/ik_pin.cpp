#include "ik_pin.h"
#include "aim_decouple.h"
#include "aob.h"
#include "cam_matrix.h"
#include "log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace asi {
namespace {

using FindPlayerPed_t = void*(__cdecl*)(int32_t);
FindPlayerPed_t g_FindPlayerPed = nullptr;

std::atomic<bool> g_dead{false};

bool GetAsiDir(char* out, size_t cap) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&GetAsiDir), &self))
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
  if (!GetAsiDir(path, MAX_PATH))
    return defValue;
  strcat_s(path, name);
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f)
    return defValue;
  char buf[32]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  int v = defValue;
  if (n == 0 || sscanf_s(buf, "%d", &v) != 1)
    return defValue;
  return v;
}

bool EnsureFindPlayerPed() {
  if (g_FindPlayerPed)
    return true;
  const uintptr_t p = FindPattern(nullptr, "8B 44 24 04 85 C0 75 18 A1");
  if (!p)
    return false;
  g_FindPlayerPed = reinterpret_cast<FindPlayerPed_t>(p);
  Log("IkPin: FindPlayerPed @ %p — LOG ONLY kill: ikpin=0", reinterpret_cast<void*>(p));
  return true;
}

bool SaneWorld(float x, float y, float z) {
  if (!(x == x) || !(y == y) || !(z == z))
    return false;
  if (x < -10000.f || x > 10000.f || y < -10000.f || y > 10000.f || z < -500.f || z > 2000.f)
    return false;
  return true;
}

float Dist3(float ax, float ay, float az, float bx, float by, float bz) {
  const float dx = ax - bx, dy = ay - by, dz = az - bz;
  return sqrtf(dx * dx + dy * dy + dz * dz);
}

void GetExeRange(uintptr_t* lo, uintptr_t* hi) {
  *lo = 0;
  *hi = 0;
  HMODULE mod = GetModuleHandleA(nullptr);
  MODULEINFO mi{};
  if (!mod || !GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
    return;
  *lo = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
  *hi = *lo + static_cast<uintptr_t>(mi.SizeOfImage);
}

bool IsFloatishHighByte(uint8_t hi) {
  switch (hi) {
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0xBE:
    case 0xBF:
      return true;
    default:
      return false;
  }
}

bool LooksLikeHeapPtr(uintptr_t v, uintptr_t ped, uintptr_t exeLo, uintptr_t exeHi) {
  if (v < 0x01000000u || v >= 0xFFF00000u)
    return false;
  if ((v & 3u) != 0u)
    return false;
  if (IsFloatishHighByte(static_cast<uint8_t>((v >> 24) & 0xFFu)))
    return false;
  if (v == ped)
    return false;
  if (exeLo && exeHi && v >= exeLo && v < exeHi)
    return false;
  return true;
}

bool ResolveAimRef(float* rx, float* ry, float* rz, const char** srcOut) {
  bool fromCtrl = false;
  if (ComputeAimOrigin(rx, ry, rz, &fromCtrl) && SaneWorld(*rx, *ry, *rz)) {
    *srcOut = fromCtrl ? "ctrl" : "cam";
    return true;
  }
  float camX = 0.f, camY = 0.f, camZ = 0.f;
  if (!GetLastStereoCamPos(&camX, &camY, &camZ) || !SaneWorld(camX, camY, camZ))
    return false;
  *rx = camX;
  *ry = camY;
  *rz = camZ;
  *srcOut = "cam";
  return true;
}

bool IkPinFileOn() {
  static const int v = ReadIntFile("gtaiv_dxvk_vr.ikpin", 0);
  return v != 0;
}

}  // namespace

bool IsIkPinEnabled() {
  return IkPinFileOn();
}

void TickIkPin(bool aimHeld) {
  if (!IsIkPinEnabled() || g_dead.load())
    return;
  if (!aimHeld)
    return;
  if (!EnsureFindPlayerPed() || !g_FindPlayerPed)
    return;

  // ~2 Hz
  static DWORD s_last = 0;
  const DWORD now = GetTickCount();
  if (now - s_last < 500)
    return;
  s_last = now;

  static bool s_once = false;
  if (!s_once) {
    s_once = true;
    Log("IkPin: enabled — CPed+0xD58 -> P+0x90 vs ComputeAimOrigin. LOG ONLY. "
        "NO PAGE_GUARD. kill: ikpin=0");
  }

  void* ped = nullptr;
  __try {
    ped = g_FindPlayerPed(0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("IkPin: EXCEPTION FindPlayerPed — probe OFF");
    return;
  }
  if (!ped)
    return;

  float rx = 0.f, ry = 0.f, rz = 0.f;
  const char* src = "none";
  if (!ResolveAimRef(&rx, &ry, &rz, &src)) {
    static DWORD s_noRef = 0;
    if (now - s_noRef > 3000) {
      s_noRef = now;
      Log("IkPin: no aim ref (need ComputeAimOrigin / cam) — keep aiming");
    }
    return;
  }

  constexpr uint32_t kPedOff = 0xD58;
  constexpr uint32_t kVec90 = 0x90;
  constexpr uint32_t kVecA0 = 0xA0;

  uintptr_t exeLo = 0, exeHi = 0;
  GetExeRange(&exeLo, &exeHi);
  const uintptr_t pedU = reinterpret_cast<uintptr_t>(ped);

  __try {
    auto* pedB = reinterpret_cast<uint8_t*>(ped);
    const uintptr_t P = *reinterpret_cast<uintptr_t*>(pedB + kPedOff);
    if (!LooksLikeHeapPtr(P, pedU, exeLo, exeHi)) {
      static DWORD s_bad = 0;
      if (now - s_bad > 3000) {
        s_bad = now;
        Log("IkPin: CPed+0xD58 ptr=0x%08X hardened FAIL", static_cast<uint32_t>(P));
      }
      return;
    }

    auto* base = reinterpret_cast<uint8_t*>(P);
    const float x = *reinterpret_cast<float*>(base + kVec90);
    const float y = *reinterpret_cast<float*>(base + kVec90 + 4);
    const float z = *reinterpret_cast<float*>(base + kVec90 + 8);
    if (!SaneWorld(x, y, z)) {
      static DWORD s_badF = 0;
      if (now - s_badF > 3000) {
        s_badF = now;
        Log("IkPin: D58->%p +90 insane (%.2f,%.2f,%.2f)", reinterpret_cast<void*>(P), x, y, z);
      }
      return;
    }

    const float d = Dist3(x, y, z, rx, ry, rz);
    Log("IkPin: D58->%p +90=(%.2f,%.2f,%.2f) ref=(%.2f,%.2f,%.2f) d=%.3f src=%s",
        reinterpret_cast<void*>(P), x, y, z, rx, ry, rz, d, src);

    static bool s_sib = false;
    if (!s_sib) {
      s_sib = true;
      const float ax = *reinterpret_cast<float*>(base + kVecA0);
      const float ay = *reinterpret_cast<float*>(base + kVecA0 + 4);
      const float az = *reinterpret_cast<float*>(base + kVecA0 + 8);
      const float dA0 = Dist3(ax, ay, az, rx, ry, rz);
      Log("IkPin: sibling +0xA0=(%.2f,%.2f,%.2f) d=%.3f sane=%d (once)", ax, ay, az, dA0,
          SaneWorld(ax, ay, az) ? 1 : 0);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("IkPin: EXCEPTION reading D58/+90 — probe OFF");
  }
}

}  // namespace asi
