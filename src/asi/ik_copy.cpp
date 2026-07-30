#include "ik_copy.h"
#include "aob.h"
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

bool IsIkCopyEnabled() {
  static const int v = ReadIntFile("gtaiv_dxvk_vr.ikcopy", 0);
  return v != 0;
}

bool EnsureFindPlayerPed() {
  if (g_FindPlayerPed)
    return true;
  const uintptr_t p = FindPattern(nullptr, "8B 44 24 04 85 C0 75 18 A1");
  if (!p)
    return false;
  g_FindPlayerPed = reinterpret_cast<FindPlayerPed_t>(p);
  Log("IkCopy: FindPlayerPed @ %p — LOG ONLY kill: ikcopy=0", reinterpret_cast<void*>(p));
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

struct MoverHit {
  uint32_t off = 0;
  float x = 0.f, y = 0.f, z = 0.f;
  float dx = 0.f, dy = 0.f, dz = 0.f;
  float mag = 0.f;
};

struct PtrCand {
  uint32_t off = 0;
  uintptr_t p = 0;
};

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
  // Soft reject: common IEEE float high bytes (incl. negative).
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

// Strict pointer filter: aligned user-range dword, not ped, not EXE image,
// not float-looking bit patterns (false positives like 0x3F800000 = 1.0f).
bool LooksLikeHeapPtr(uintptr_t v, uintptr_t ped, uintptr_t exeLo, uintptr_t exeHi) {
  if (v < 0x01000000u || v >= 0xFFF00000u)
    return false;
  if ((v & 3u) != 0u)
    return false;
  if (IsFloatishHighByte(static_cast<uint8_t>((v >> 24) & 0xFFu)))
    return false;
  // Skip self-pointer to the ped object.
  if (v == ped)
    return false;
  // Skip GTAIV.exe / module image (statics like 00EDxxxx).
  if (exeLo && exeHi && v >= exeLo && v < exeHi)
    return false;
  return true;
}

// C20 must be a real world aim point — reject zero / near-zero junk that caused
// HEAP MATCH false positives (dRef≈0 vs (0,0,0)).
bool C20RefUsableForHeap(float rx, float ry, float rz) {
  if (!SaneWorld(rx, ry, rz))
    return false;
  const float ax = fabsf(rx), ay = fabsf(ry), az = fabsf(rz);
  if (ax < 1.f && ay < 1.f && az < 1.f)
    return false;
  const float len = sqrtf(rx * rx + ry * ry + rz * rz);
  if (len < 10.f)
    return false;
  return true;
}

// One float3 slot: SaneWorld + dRef<=matchMax. Returns 1 if logged.
int TryHeapVecMatch(uint8_t* base, uintptr_t P, uint32_t pedOff, uint32_t vec, float rx, float ry,
                    float rz, float matchMax) {
  if (vec + 8 > 0x120)
    return 0;
  auto* f = reinterpret_cast<float*>(base + vec);
  const float x = f[0], y = f[1], z = f[2];
  if (!SaneWorld(x, y, z))
    return 0;
  const float dRef = Dist3(x, y, z, rx, ry, rz);
  if (dRef > matchMax)
    return 0;
  Log("IkCopy: HEAP MATCH CPed+0x%X -> %p +0x%X float3=(%.2f,%.2f,%.2f) dRef=%.3f", pedOff,
      reinterpret_cast<void*>(P), vec, x, y, z, dRef);
  return 1;
}

// Per-pointer SEH: bad heap ptr must not kill the whole probe (do not set g_dead).
// Prefer classic vecAimTarget at +0x70 first, then walk 0..0x120.
// Returns how many HEAP MATCH lines were logged (capped by maxLog).
int ScanHeapObjForC20(uintptr_t P, uint32_t pedOff, float rx, float ry, float rz, float matchMax,
                      int maxLog) {
  if (maxLog <= 0)
    return 0;
  int logged = 0;
  __try {
    auto* base = reinterpret_cast<uint8_t*>(P);
    // Classic vecAimTarget first.
    logged += TryHeapVecMatch(base, P, pedOff, 0x70, rx, ry, rz, matchMax);
    for (uint32_t vec = 0; vec + 8 <= 0x120 && logged < maxLog; vec += 4) {
      if (vec == 0x70)
        continue;
      logged += TryHeapVecMatch(base, P, pedOff, vec, rx, ry, rz, matchMax);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Skip this pointer only.
  }
  return logged;
}

}  // namespace

// Scan CPed for float3 copies of C20 (aim OUTPUT) and top movers while LT held.
// Also follow heap-looking pointers near +0xB80..+0xC40 / +0xA00..+0xE00 and SEH-scan
// each object for float3 near C20 (IK read-side candidates). Never writes. ~1–2 Hz.
// Kill: ikcopy=0.
void TickIkCopy(bool aimHeld) {
  if (!IsIkCopyEnabled() || g_dead.load())
    return;
  if (!aimHeld)
    return;
  if (!EnsureFindPlayerPed() || !g_FindPlayerPed)
    return;

  static DWORD s_last = 0;
  const DWORD now = GetTickCount();
  if (now - s_last < 600)
    return;
  s_last = now;

  void* ped = nullptr;
  __try {
    ped = g_FindPlayerPed(0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("IkCopy: EXCEPTION FindPlayerPed — probe OFF");
    return;
  }
  if (!ped)
    return;

  // C20 = aim OUTPUT (freeze MISS). Hunt other slots IK might *read*.
  constexpr uint32_t kRefOff = 0xC20;
  constexpr uint32_t kScanLo = 0x800;
  constexpr uint32_t kScanHi = 0xE00;
  constexpr float kMatchMax = 2.0f;  // same aim-point copy within ~0.5–2 units
  constexpr int kMaxMatch = 16;
  constexpr int kMaxMover = 8;
  constexpr int kMaxHeapPtrs = 32;
  constexpr int kMaxHeapMatch = 12;
  constexpr size_t kSlots = (kScanHi - kScanLo) / 4 + 4;

  static float s_prev[kSlots]{};
  static bool s_havePrev = false;
  static bool s_banner = false;
  static uint32_t s_heapCursor = 0;
  static uint32_t s_heapSkipFloatish = 0;
  static uint32_t s_heapSkipExe = 0;
  static uint32_t s_heapSkipWeakRef = 0;
  static DWORD s_lastSkipLog = 0;
  if (!s_banner) {
    s_banner = true;
    Log("IkCopy: enabled — scan CPed+0x%X..+0x%X vs ref C20 + hardened heap "
        "(ptr filter, C20 mag gate, prefer +0x70) while aim held. "
        "LOG ONLY (no writes). kill: ikcopy=0",
        kScanLo, kScanHi);
  }

  uintptr_t exeLo = 0, exeHi = 0;
  GetExeRange(&exeLo, &exeHi);
  const uintptr_t pedU = reinterpret_cast<uintptr_t>(ped);

  __try {
    auto* base = reinterpret_cast<uint8_t*>(ped);
    auto* fRef = reinterpret_cast<float*>(base + kRefOff);
    const float rx = fRef[0], ry = fRef[1], rz = fRef[2];
    if (!SaneWorld(rx, ry, rz)) {
      Log("IkCopy: ref C20=(%.2f,%.2f,%.2f) not world-sane — skip (hold LT, aim outdoors)",
          rx, ry, rz);
      return;
    }
    Log("IkCopy: ref C20=(%.2f,%.2f,%.2f)", rx, ry, rz);

    int matches = 0;
    MoverHit movers[kMaxMover]{};
    int nMovers = 0;

    for (uint32_t off = kScanLo; off + 8 <= kScanHi; off += 4) {
      if (off == kRefOff)
        continue;
      auto* f = reinterpret_cast<float*>(base + off);
      const float x = f[0], y = f[1], z = f[2];
      if (!SaneWorld(x, y, z))
        continue;

      const float dRef = Dist3(x, y, z, rx, ry, rz);
      if (dRef <= kMatchMax && matches < kMaxMatch) {
        Log("IkCopy: MATCH CPed+0x%X float3=(%.2f,%.2f,%.2f) dRef=%.3f", off, x, y, z,
            dRef);
        ++matches;
      }

      const size_t idx = (off - kScanLo) / 4;
      if (s_havePrev && idx + 2 < kSlots &&
          SaneWorld(s_prev[idx], s_prev[idx + 1], s_prev[idx + 2])) {
        const float dx = x - s_prev[idx];
        const float dy = y - s_prev[idx + 1];
        const float dz = z - s_prev[idx + 2];
        const float mag = sqrtf(dx * dx + dy * dy + dz * dz);
        if (mag >= 0.05f) {
          // Keep top movers by delta magnitude.
          int slot = nMovers;
          if (nMovers < kMaxMover) {
            ++nMovers;
          } else {
            slot = 0;
            for (int i = 1; i < kMaxMover; ++i) {
              if (movers[i].mag < movers[slot].mag)
                slot = i;
            }
            if (mag <= movers[slot].mag)
              slot = -1;
          }
          if (slot >= 0) {
            movers[slot] = {off, x, y, z, dx, dy, dz, mag};
          }
        }
      }
    }

    // Sort movers descending for stable log order (tiny N).
    for (int i = 0; i + 1 < nMovers; ++i) {
      for (int j = i + 1; j < nMovers; ++j) {
        if (movers[j].mag > movers[i].mag) {
          const MoverHit t = movers[i];
          movers[i] = movers[j];
          movers[j] = t;
        }
      }
    }
    for (int i = 0; i < nMovers; ++i) {
      const MoverHit& m = movers[i];
      Log("IkCopy: MOVER CPed+0x%X float3=(%.2f,%.2f,%.2f) d=(%.2f,%.2f,%.2f)", m.off, m.x,
          m.y, m.z, m.dx, m.dy, m.dz);
    }

    if (matches == 0)
      Log("IkCopy: no MATCH float3 within %.1f of C20 in +0x%X..+0x%X (keep aiming)",
          kMatchMax, kScanLo, kScanHi);

    // --- Heap pointer follow: hunt float3 near C20 inside pointed-to objects ---
    // Gate: C20 must be clearly nonzero world (prior build matched vs zero → FP).
    int heapLogged = 0;
    int scanned = 0;
    int nCand = 0;
    if (!C20RefUsableForHeap(rx, ry, rz)) {
      ++s_heapSkipWeakRef;
      if (now - s_lastSkipLog >= 5000) {
        s_lastSkipLog = now;
        Log("IkCopy: HEAP skip (weak C20 ref — need nonzero world; floatish=%u exe=%u weak=%u)",
            s_heapSkipFloatish, s_heapSkipExe, s_heapSkipWeakRef);
      }
    } else {
      PtrCand cands[128]{};
      // Priority window (ikMgr neighborhood), then broader A00..E00.
      for (uint32_t pass = 0; pass < 2; ++pass) {
        const uint32_t lo = (pass == 0) ? 0xB80u : 0xA00u;
        const uint32_t hi = (pass == 0) ? 0xC40u : 0xE00u;
        for (uint32_t off = lo; off <= hi && nCand < 128; off += 4) {
          if (pass == 1 && off >= 0xB80u && off <= 0xC40u)
            continue;
          const uintptr_t v = *reinterpret_cast<uintptr_t*>(base + off);
          // Count soft rejects for occasional skip log (not spam).
          if (v >= 0x01000000u && v < 0xFFF00000u && (v & 3u) == 0u) {
            if (IsFloatishHighByte(static_cast<uint8_t>((v >> 24) & 0xFFu))) {
              ++s_heapSkipFloatish;
              continue;
            }
            if (exeLo && exeHi && v >= exeLo && v < exeHi) {
              ++s_heapSkipExe;
              continue;
            }
          }
          if (!LooksLikeHeapPtr(v, pedU, exeLo, exeHi))
            continue;
          cands[nCand].off = off;
          cands[nCand].p = v;
          ++nCand;
        }
      }

      if (nCand > 0) {
        const uint32_t start = s_heapCursor % static_cast<uint32_t>(nCand);
        for (int i = 0; i < nCand && scanned < kMaxHeapPtrs && heapLogged < kMaxHeapMatch; ++i) {
          const int idx = static_cast<int>((start + static_cast<uint32_t>(i)) %
                                           static_cast<uint32_t>(nCand));
          const PtrCand& c = cands[idx];
          heapLogged +=
              ScanHeapObjForC20(c.p, c.off, rx, ry, rz, kMatchMax, kMaxHeapMatch - heapLogged);
          ++scanned;
        }
        s_heapCursor = start + static_cast<uint32_t>(scanned);
      }
      if (now - s_lastSkipLog >= 5000 &&
          (s_heapSkipFloatish | s_heapSkipExe | s_heapSkipWeakRef) != 0) {
        s_lastSkipLog = now;
        Log("IkCopy: HEAP skip counts floatish=%u exe=%u weak=%u (ptrs=%d)", s_heapSkipFloatish,
            s_heapSkipExe, s_heapSkipWeakRef, nCand);
      }
    }
    if (heapLogged == 0)
      Log("IkCopy: no HEAP MATCH (ptrs=%d scanned=%d/tick cap=%d) — keep aiming", nCand,
          scanned, kMaxHeapPtrs);

    for (uint32_t off = kScanLo; off <= kScanHi; off += 4) {
      const size_t idx = (off - kScanLo) / 4;
      if (idx >= kSlots)
        break;
      s_prev[idx] = *reinterpret_cast<float*>(base + off);
    }
    s_havePrev = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("IkCopy: EXCEPTION reading ped floats — probe OFF");
  }
}

}  // namespace asi
