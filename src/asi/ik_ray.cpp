#include "ik_ray.h"
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
  Log("IkRay: FindPlayerPed @ %p — LOG ONLY kill: ikray=0", reinterpret_cast<void*>(p));
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

// Hardened filter: aligned user-range, not ped, not EXE, not floatish.
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

bool RefUsableForHeap(float rx, float ry, float rz) {
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

struct NearestHit {
  float d = 1.0e9f;
  uint32_t pedOff = 0;
  uint32_t vec = 0;
  uint32_t qOff = 0;
  bool have = false;
};

void ConsiderNearest(NearestHit* n, float d, uint32_t pedOff, uint32_t vec, uint32_t qOff = 0) {
  if (!n || !(d == d) || d >= n->d)
    return;
  n->d = d;
  n->pedOff = pedOff;
  n->vec = vec;
  n->qOff = qOff;
  n->have = true;
}

int TryHeapVecMatch(uint8_t* base, uintptr_t P, uint32_t pedOff, uint32_t vec, float rx, float ry,
                    float rz, float matchMax, NearestHit* nearest) {
  if (vec + 8 > 0x120)
    return 0;
  auto* f = reinterpret_cast<float*>(base + vec);
  const float x = f[0], y = f[1], z = f[2];
  if (!SaneWorld(x, y, z))
    return 0;
  const float dRef = Dist3(x, y, z, rx, ry, rz);
  ConsiderNearest(nearest, dRef, pedOff, vec);
  if (dRef > matchMax)
    return 0;
  Log("IkRay: HEAP MATCH CPed+0x%X -> %p +0x%X float3=(%.2f,%.2f,%.2f) dRef=%.3f", pedOff,
      reinterpret_cast<void*>(P), vec, x, y, z, dRef);
  return 1;
}

int TryL2VecMatch(uintptr_t P, uint32_t pedOff, uint32_t qOff, uintptr_t Q, uint32_t vec, float rx,
                  float ry, float rz, float matchMax, NearestHit* nearest) {
  if (vec + 8 > 0x120)
    return 0;
  __try {
    auto* f = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(Q) + vec);
    const float x = f[0], y = f[1], z = f[2];
    if (!SaneWorld(x, y, z))
      return 0;
    const float dRef = Dist3(x, y, z, rx, ry, rz);
    ConsiderNearest(nearest, dRef, pedOff, vec, qOff);
    if (dRef > matchMax)
      return 0;
    Log("IkRay: L2 MATCH CPed+0x%X -> %p +0x%X -> %p +0x%X float3=(%.2f,%.2f,%.2f) dRef=%.3f",
        pedOff, reinterpret_cast<void*>(P), qOff, reinterpret_cast<void*>(Q), vec, x, y, z, dRef);
    return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

int ScanHeapObjForRef(uintptr_t P, uint32_t pedOff, float rx, float ry, float rz, float matchMax,
                      int maxLog, NearestHit* nearest) {
  if (maxLog <= 0)
    return 0;
  int logged = 0;
  __try {
    auto* base = reinterpret_cast<uint8_t*>(P);
    logged += TryHeapVecMatch(base, P, pedOff, 0x70, rx, ry, rz, matchMax, nearest);
    for (uint32_t vec = 0; vec + 8 <= 0x120 && logged < maxLog; vec += 4) {
      if (vec == 0x70)
        continue;
      logged += TryHeapVecMatch(base, P, pedOff, vec, rx, ry, rz, matchMax, nearest);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return logged;
}

// Level-2: Q ptrs at P+0..+0x100 (hardened) → float3 at Q+0..+0x120 prefer +0x70.
int ScanHeapL2(uintptr_t P, uint32_t pedOff, float rx, float ry, float rz, float matchMax,
               int maxLog, uintptr_t pedU, uintptr_t exeLo, uintptr_t exeHi, int maxQ,
               NearestHit* nearest) {
  if (maxLog <= 0 || maxQ <= 0)
    return 0;
  int logged = 0;
  int qScanned = 0;
  __try {
    auto* base = reinterpret_cast<uint8_t*>(P);
    PtrCand qs[40]{};
    int nQ = 0;
    for (uint32_t b = 0; b <= 0x100 && nQ < 40; b += 4) {
      const uintptr_t q = *reinterpret_cast<uintptr_t*>(base + b);
      if (q == P)
        continue;
      if (!LooksLikeHeapPtr(q, pedU, exeLo, exeHi))
        continue;
      qs[nQ].off = b;
      qs[nQ].p = q;
      ++nQ;
    }

    for (int i = 0; i < nQ && logged < maxLog && qScanned < maxQ; ++i) {
      const uintptr_t Q = qs[i].p;
      const uint32_t qOff = qs[i].off;
      ++qScanned;
      logged += TryL2VecMatch(P, pedOff, qOff, Q, 0x70, rx, ry, rz, matchMax, nearest);
      for (uint32_t vec = 0; vec + 8 <= 0x120 && logged < maxLog; vec += 4) {
        if (vec == 0x70)
          continue;
        logged += TryL2VecMatch(P, pedOff, qOff, Q, vec, rx, ry, rz, matchMax, nearest);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return logged;
}

// Match ref = game-world ray ORIGIN (ctrl tip / cam), same space as C20 — NOT the
// far cam+fwd*25 hit (that put IkRay ref Z≈42 while cam Z≈18 → 0 MATCH).
bool ResolveRayRef(float* rx, float* ry, float* rz, const char** srcOut) {
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

bool IkRayFileOn() {
  static const int v = ReadIntFile("gtaiv_dxvk_vr.ikray", 0);
  return v != 0;
}

}  // namespace

bool IsIkRayEnabled() {
  return IkRayFileOn();
}

void TickIkRay(bool aimHeld) {
  if (!IsIkRayEnabled() || g_dead.load())
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
    Log("IkRay: EXCEPTION FindPlayerPed — probe OFF");
    return;
  }
  if (!ped)
    return;

  constexpr uint32_t kScanLo = 0x200;
  constexpr uint32_t kScanHi = 0x1400;
  constexpr uint32_t kSkipC20 = 0xC20;
  constexpr float kMatchMax = 2.0f;
  constexpr int kMaxMatch = 8;
  constexpr int kMaxL1Ptrs = 6;   // few ptrs / tick
  constexpr int kMaxHeapMatch = 6;
  constexpr int kMaxL2Match = 8;
  constexpr int kMaxQPerP = 8;

  static bool s_banner = false;
  static bool s_skipC20Logged = false;
  static uint32_t s_heapCursor = 0;
  static uint32_t s_heapSkipFloatish = 0;
  static uint32_t s_heapSkipExe = 0;
  static uint32_t s_heapSkipWeakRef = 0;
  static DWORD s_lastSkipLog = 0;
  static DWORD s_lastNearestLog = 0;
  if (!s_banner) {
    s_banner = true;
    Log("IkRay: enabled — scan CPed+0x%X..+0x%X vs ctrl/cam WORLD ORIGIN (NOT far ray, "
        "NOT C20) + L1 heap + L2 (P+0..+0x100 -> Q +0..+0x120 prefer +0x70) while aim held. "
        "LOG ONLY. NO PAGE_GUARD. kill: ikray=0",
        kScanLo, kScanHi);
  }

  float rx = 0.f, ry = 0.f, rz = 0.f;
  const char* src = "cam";
  if (!ResolveRayRef(&rx, &ry, &rz, &src)) {
    Log("IkRay: no aim ref (need ComputeAimOrigin / cam) — keep aiming");
    return;
  }
  Log("IkRay: ref=(%.2f,%.2f,%.2f) src=%s", rx, ry, rz, src);

  uintptr_t exeLo = 0, exeHi = 0;
  GetExeRange(&exeLo, &exeHi);
  const uintptr_t pedU = reinterpret_cast<uintptr_t>(ped);

  __try {
    auto* base = reinterpret_cast<uint8_t*>(ped);
    int matches = 0;
    NearestHit nearPed{};
    NearestHit nearL1{};
    NearestHit nearL2{};

    for (uint32_t off = kScanLo; off + 8 <= kScanHi; off += 4) {
      if (off == kSkipC20) {
        if (!s_skipC20Logged) {
          s_skipC20Logged = true;
          Log("IkRay: skip C20");
        }
        continue;
      }
      auto* f = reinterpret_cast<float*>(base + off);
      const float x = f[0], y = f[1], z = f[2];
      if (!SaneWorld(x, y, z))
        continue;
      const float dRef = Dist3(x, y, z, rx, ry, rz);
      ConsiderNearest(&nearPed, dRef, off, 0);
      if (dRef <= kMatchMax && matches < kMaxMatch) {
        Log("IkRay: MATCH CPed+0x%X float3=(%.2f,%.2f,%.2f) dRef=%.3f", off, x, y, z, dRef);
        ++matches;
      }
    }
    if (matches == 0)
      Log("IkRay: no MATCH float3 within %.1f of ray ref in +0x%X..+0x%X (keep aiming)",
          kMatchMax, kScanLo, kScanHi);

    int heapLogged = 0;
    int l2Logged = 0;
    int scanned = 0;
    int nCand = 0;
    if (!RefUsableForHeap(rx, ry, rz)) {
      ++s_heapSkipWeakRef;
      if (now - s_lastSkipLog >= 5000) {
        s_lastSkipLog = now;
        Log("IkRay: HEAP skip (weak ray ref; floatish=%u exe=%u weak=%u)", s_heapSkipFloatish,
            s_heapSkipExe, s_heapSkipWeakRef);
      }
    } else {
      PtrCand cands[128]{};
      for (uint32_t pass = 0; pass < 2; ++pass) {
        const uint32_t lo = (pass == 0) ? 0xB80u : 0xA00u;
        const uint32_t hi = (pass == 0) ? 0xC40u : 0xE00u;
        for (uint32_t off = lo; off <= hi && nCand < 128; off += 4) {
          if (pass == 1 && off >= 0xB80u && off <= 0xC40u)
            continue;
          const uintptr_t v = *reinterpret_cast<uintptr_t*>(base + off);
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
        for (int i = 0; i < nCand && scanned < kMaxL1Ptrs &&
                        (heapLogged < kMaxHeapMatch || l2Logged < kMaxL2Match);
             ++i) {
          const int idx = static_cast<int>((start + static_cast<uint32_t>(i)) %
                                           static_cast<uint32_t>(nCand));
          const PtrCand& c = cands[idx];
          if (heapLogged < kMaxHeapMatch)
            heapLogged += ScanHeapObjForRef(c.p, c.off, rx, ry, rz, kMatchMax,
                                           kMaxHeapMatch - heapLogged, &nearL1);
          if (l2Logged < kMaxL2Match)
            l2Logged += ScanHeapL2(c.p, c.off, rx, ry, rz, kMatchMax, kMaxL2Match - l2Logged,
                                   pedU, exeLo, exeHi, kMaxQPerP, &nearL2);
          ++scanned;
        }
        s_heapCursor = start + static_cast<uint32_t>(scanned);
      }
      if (now - s_lastSkipLog >= 5000 &&
          (s_heapSkipFloatish | s_heapSkipExe | s_heapSkipWeakRef) != 0) {
        s_lastSkipLog = now;
        Log("IkRay: HEAP skip counts floatish=%u exe=%u weak=%u (ptrs=%d)", s_heapSkipFloatish,
            s_heapSkipExe, s_heapSkipWeakRef, nCand);
      }
    }
    if (heapLogged == 0)
      Log("IkRay: no HEAP MATCH (ptrs=%d scanned=%d/tick cap=%d) — keep aiming", nCand, scanned,
          kMaxL1Ptrs);
    if (l2Logged == 0)
      Log("IkRay: no L2 MATCH (ptrs=%d scanned=%d/tick) — keep aiming", nCand, scanned);

    // LOG-ONLY nearest distances (~1–2 Hz; tick already ≥600 ms).
    if (now - s_lastNearestLog >= 500) {
      s_lastNearestLog = now;
      if (nearPed.have)
        Log("IkRay: nearest ped d=%.3f at +0x%X", nearPed.d, nearPed.pedOff);
      else
        Log("IkRay: nearest ped d=none");
      if (nearL1.have || nearL2.have) {
        Log("IkRay: nearest L1/L2 d=%.3f/+0x%X+0x%X / %.3f/+0x%X+0x%X+0x%X",
            nearL1.have ? nearL1.d : -1.f, nearL1.pedOff, nearL1.vec,
            nearL2.have ? nearL2.d : -1.f, nearL2.pedOff, nearL2.qOff, nearL2.vec);
      } else {
        Log("IkRay: nearest L1/L2 d=none");
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("IkRay: EXCEPTION reading ped floats — probe OFF");
  }
}

}  // namespace asi
