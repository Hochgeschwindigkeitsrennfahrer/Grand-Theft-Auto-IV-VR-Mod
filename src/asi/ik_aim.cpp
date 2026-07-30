#include "ik_aim.h"
#include "aob.h"
#include "log.h"

#include "../../thirdparty/minhook/include/MinHook.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

// MinHook trampoline + naked detour (file scope — asm needs a plain symbol).
static void* g_asi_ikesi_tramp = nullptr;
extern "C" void __cdecl AsiIkEsiOnHit(void* esiVal);
extern "C" __declspec(naked) void AsiHookIkEsi() {
  __asm {
    pushad
    pushfd
    push esi
    call AsiIkEsiOnHit
    add esp, 4
    popfd
    popad
    jmp dword ptr [g_asi_ikesi_tramp]
  }
}

namespace asi {

void IkEsiProbeHit(void* esiVal);

namespace {

using FindPlayerPed_t = void*(__cdecl*)(int32_t);
FindPlayerPed_t g_FindPlayerPed = nullptr;

std::atomic<bool> g_dead{false};
std::atomic<bool> g_offsReady{false};
uint32_t g_ikMgrOff = 0;
uint32_t g_vecOff = 0;

std::atomic<bool> g_ikesiDead{false};
std::atomic<bool> g_ikesiHookOk{false};
std::atomic<bool> g_ikesiHookFail{false};
void* g_ikesiTarget = nullptr;

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

bool IsIkAimEnabled() {
  static const int v = ReadIntFile("gtaiv_dxvk_vr.ikaim", 0);
  return v != 0;
}

bool IsIkProbeEnabled() {
  static const int v = ReadIntFile("gtaiv_dxvk_vr.ikprobe", 0);
  return v != 0;
}

bool IsIkWriterEnabled() {
  static const int v = ReadIntFile("gtaiv_dxvk_vr.ikwriter", 0);
  return v != 0;
}

bool IsIkEsiEnabled() {
  static const int v = ReadIntFile("gtaiv_dxvk_vr.ikesi", 0);
  return v != 0;
}

bool EnsureMinHook() {
  const MH_STATUS st = MH_Initialize();
  return st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED;
}

// Session6 CE site: 89 46 70 F3 0F 11 4E 74 F3 0F 11 46 78 (mov [esi+70],eax + movss).
// MinHook steals ≥5 bytes via full instructions (3+5) — never a 3-byte mid-hook alone.
uintptr_t FindSession6WriterSite(uintptr_t base, unsigned* outRva, int* outDelta) {
  constexpr uintptr_t kHintRva = 0x89F1AF;
  constexpr const char* kLongPat = "89 46 70 F3 0F 11 4E 74 F3 0F 11 46 78";

  const uintptr_t longHit = FindPattern(nullptr, kLongPat);
  if (longHit) {
    const uintptr_t rva = base ? (longHit - base) : 0;
    if (outRva)
      *outRva = static_cast<unsigned>(rva);
    if (outDelta)
      *outDelta = static_cast<int>(static_cast<intptr_t>(rva) -
                                  static_cast<intptr_t>(kHintRva));
    return longHit;
  }

  // Fallback: closest plain 89 46 70 to hint, accept only if next bytes are movss.
  uintptr_t best = 0;
  intptr_t bestAbs = 0x7FFFFFFF;
  int bestDelta = 0;
  unsigned bestRva = 0;
  for (int i = 0; i < 128; ++i) {
    const uintptr_t p = FindPatternN(nullptr, "89 46 70", i);
    if (!p)
      break;
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    if (!(b[3] == 0xF3 && b[4] == 0x0F && b[5] == 0x11 && b[6] == 0x4E && b[7] == 0x74))
      continue;
    const uintptr_t rva = base ? (p - base) : 0;
    const intptr_t d = static_cast<intptr_t>(rva) - static_cast<intptr_t>(kHintRva);
    const intptr_t ad = d < 0 ? -d : d;
    if (ad < bestAbs) {
      bestAbs = ad;
      best = p;
      bestDelta = static_cast<int>(d);
      bestRva = static_cast<unsigned>(rva);
    }
  }
  if (outRva)
    *outRva = bestRva;
  if (outDelta)
    *outDelta = bestDelta;
  return best;
}

bool InstallIkEsiHookOnce() {
  if (g_ikesiHookOk.load() || g_ikesiHookFail.load() || g_ikesiDead.load())
    return g_ikesiHookOk.load();
  if (!IsIkEsiEnabled())
    return false;
  if (!EnsureMinHook()) {
    Log("IkEsi: MH_Initialize FAIL — will retry");
    return false;
  }

  HMODULE mod = GetModuleHandleA(nullptr);
  MODULEINFO mi{};
  const uintptr_t base =
      (mod && GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
          ? reinterpret_cast<uintptr_t>(mi.lpBaseOfDll)
          : 0;

  unsigned rva = 0;
  int delta = 0;
  const uintptr_t site = FindSession6WriterSite(base, &rva, &delta);
  if (!site) {
    g_ikesiHookFail.store(true);
    Log("IkEsi: FAIL no Session6 writer site (long AOB / nearHint 89 46 70+movss) — probe OFF");
    return false;
  }

  const auto* b = reinterpret_cast<const uint8_t*>(site);
  if (!(b[0] == 0x89 && b[1] == 0x46 && b[2] == 0x70 && b[3] == 0xF3 && b[4] == 0x0F &&
        b[5] == 0x11 && b[6] == 0x4E && b[7] == 0x74)) {
    g_ikesiHookFail.store(true);
    Log("IkEsi: FAIL unexpected bytes @%p %02X %02X %02X %02X %02X %02X %02X %02X — probe OFF",
        reinterpret_cast<void*>(site), b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    return false;
  }

  Log("IkEsi: site @%p rva=0x%X deltaHint=%+d bytes=%02X %02X %02X %02X %02X %02X %02X %02X "
      "(MinHook steals mov+[movss]; LOG ONLY — kill: ikesi=0)",
      reinterpret_cast<void*>(site), rva, delta, b[0], b[1], b[2], b[3], b[4], b[5], b[6],
      b[7]);

  g_asi_ikesi_tramp = nullptr;
  if (MH_CreateHook(reinterpret_cast<void*>(site), reinterpret_cast<void*>(&AsiHookIkEsi),
                    &g_asi_ikesi_tramp) != MH_OK ||
      MH_EnableHook(reinterpret_cast<void*>(site)) != MH_OK) {
    g_ikesiHookFail.store(true);
    Log("IkEsi: MH_CreateHook/Enable FAIL @%p — probe OFF", reinterpret_cast<void*>(site));
    return false;
  }

  g_ikesiTarget = reinterpret_cast<void*>(site);
  g_ikesiHookOk.store(true);
  Log("IkEsi: hook OK tramp=%p — dumps ESI ≤2Hz when mov [esi+70],eax runs",
      g_asi_ikesi_tramp);
  return true;
}

bool EnsureFindPlayerPed() {
  if (g_FindPlayerPed)
    return true;
  const uintptr_t p = FindPattern(nullptr, "8B 44 24 04 85 C0 75 18 A1");
  if (!p)
    return false;
  g_FindPlayerPed = reinterpret_cast<FindPlayerPed_t>(p);
  Log("IkAim: FindPlayerPed @ %p", reinterpret_cast<void*>(p));
  return true;
}

// Load "BC0 70" style hex offsets once. Refuse obviously-bogus values.
bool LoadOffsetsOnce() {
  if (g_offsReady.load())
    return g_ikMgrOff != 0 && g_vecOff != 0;
  g_offsReady.store(true);

  char path[MAX_PATH]{};
  if (!GetAsiDir(path, MAX_PATH))
    return false;
  strcat_s(path, "gtaiv_dxvk_vr.ikoffs");
  FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0 || !f) {
    Log("IkAim: ikoffs MISSING — write \"<ikMgrOffHex> <vecOffHex>\" after CE §5.4. "
        "No memory writes until then");
    return false;
  }
  char buf[64]{};
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  unsigned mgr = 0, vec = 0;
  if (n == 0 || sscanf_s(buf, "%x %x", &mgr, &vec) != 2) {
    Log("IkAim: ikoffs parse FAIL (want: BC0 70 hex). No writes");
    return false;
  }
  // Sanity: CPed field offsets live in a plausible window; vecAimTarget is small.
  if (mgr < 0x800 || mgr > 0x2000 || vec > 0x200) {
    Log("IkAim: ikoffs out of range mgr=0x%X vec=0x%X — refused (protect against "
        "blind 1.0.7 paste)",
        mgr, vec);
    return false;
  }
  g_ikMgrOff = mgr;
  g_vecOff = vec;
  Log("IkAim: offsets loaded ikMgr=+0x%X vecAim=+0x%X — kill: ikaim=0", mgr, vec);
  return true;
}

bool SaneWorld(float x, float y, float z) {
  // Liberty City extents are generous; reject NaN / insane.
  if (!(x == x) || !(y == y) || !(z == z))
    return false;
  if (x < -10000.f || x > 10000.f || y < -10000.f || y > 10000.f || z < -500.f || z > 2000.f)
    return false;
  return true;
}

}  // namespace

void TickIkProbe() {
  if (!IsIkProbeEnabled() || g_dead.load())
    return;
  if (!EnsureFindPlayerPed() || !g_FindPlayerPed)
    return;
  void* ped = nullptr;
  __try {
    ped = g_FindPlayerPed(0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("IkAim: EXCEPTION FindPlayerPed — probe OFF");
    return;
  }
  if (!ped)
    return;

  static DWORD s_last = 0;
  const DWORD now = GetTickCount();
  if (now - s_last < 2000)
    return;
  s_last = now;

  // Print the raw CPed* so CE "Memory View → go to address" lands inside the ped.
  // Also dump a few candidate pointer slots in the 1.0.7 window for Structure Dissect.
  Log("IkProbe: CPed*=%p (paste into CE Memory View). Scan +0xB80..+0xC40 for "
      "ikManager pointer; vecAimTarget is a float3 that moves when you aim",
      ped);

  __try {
    const uint8_t* base = reinterpret_cast<const uint8_t*>(ped);
    for (uint32_t off = 0xB80; off <= 0xC40; off += 4) {
      const uintptr_t* pSlot = reinterpret_cast<const uintptr_t*>(base + off);
      const uintptr_t v = *pSlot;
      // Heuristic: heap pointer in user space, 16-byte aligned-ish.
      if (v > 0x10000u && v < 0xFFF00000u && (v & 3u) == 0u)
        Log("IkProbe: CPed+0x%X -> %p (candidate ikMgr)", off, reinterpret_cast<void*>(v));
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log("IkProbe: EXCEPTION walking CPed — probe OFF");
    g_dead.store(true);
  }
}

// Session 6 CE: WRITE @ mov [esi+70],eax → CPed+0xC20 (= BB0+70) is aim OUTPUT.
// Log-only hunt: AOB all 89 46 70 in GTAIV.exe + ped float3 diffs (~2 Hz).
// Mid-fn MinHook skipped (3-byte site, hot path). No writes. Kill: ikwriter=0.
// Sampling does NOT require VR grip/pose (game LT writes C20 without controller tip).
void ScanWriterAobOnce() {
  static bool s_done = false;
  if (s_done)
    return;
  s_done = true;

  HMODULE mod = GetModuleHandleA(nullptr);
  MODULEINFO mi{};
  const uintptr_t base =
      (mod && GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
          ? reinterpret_cast<uintptr_t>(mi.lpBaseOfDll)
          : 0;

  // Session 6 CE abs 00C9F1AF — if classic base 00400000, RVA=0089F1AF (ASLR may differ).
  constexpr uintptr_t kHintRva = 0x89F1AF;
  Log("IkWriter: AOB scan \"89 46 70\" (mov [esi+70],eax) in GTAIV.exe base=%p size=0x%X "
      "hintRva=0x%X (Session6 CE abs≈base+hint). LOG ONLY — kill: ikwriter=0",
      reinterpret_cast<void*>(base), static_cast<unsigned>(mi.SizeOfImage),
      static_cast<unsigned>(kHintRva));

  int hits = 0;
  int nearHint = 0;
  for (int i = 0; i < 128; ++i) {
    const uintptr_t p = FindPatternN(nullptr, "89 46 70", i);
    if (!p)
      break;
    const uintptr_t rva = base ? (p - base) : 0;
    const intptr_t d = static_cast<intptr_t>(rva) - static_cast<intptr_t>(kHintRva);
    const bool close = (d >= -0x40 && d <= 0x40);
    if (close)
      ++nearHint;
    Log("IkWriter: AOB hit#%d @ %p rva=0x%X%s", i, reinterpret_cast<void*>(p),
        static_cast<unsigned>(rva), close ? " << NEAR Session6 hint" : "");
    ++hits;
  }
  Log("IkWriter: AOB done hits=%d nearHint=%d — CE next: Memory View → hit addr, "
      "breakpoint on write, or dissect ESI when [esi+70] is CPed+0xC20",
      hits, nearHint);
}

void TickIkWriter(bool gripHeld, bool ctrlValid) {
  if (!IsIkWriterEnabled() || g_dead.load())
    return;

  ScanWriterAobOnce();

  // Always sample when ped is valid — do not wait for VR grip/pose (ctrl/grip=0 boots).
  if (!EnsureFindPlayerPed() || !g_FindPlayerPed)
    return;

  static DWORD s_last = 0;
  const DWORD now = GetTickCount();
  if (now - s_last < 500)
    return;
  s_last = now;

  void* ped = nullptr;
  __try {
    ped = g_FindPlayerPed(0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("IkWriter: EXCEPTION FindPlayerPed — probe OFF");
    return;
  }
  if (!ped)
    return;

  // CPed+0xBB0 + 0x70 = CPed+0xC20 — Session 6 writer-slot (freeze MISS / aim OUTPUT).
  constexpr uint32_t kWriterSlot = 0xC20;
  constexpr uint32_t kScanLo = 0xB00;
  constexpr uint32_t kScanHi = 0xD80;
  constexpr int kMaxDiffs = 12;

  static float s_prev[0x300 / 4]{};
  static bool s_havePrev = false;
  static uint32_t s_n = 0;
  const uint32_t n = ++s_n;

  __try {
    auto* base = reinterpret_cast<uint8_t*>(ped);
    auto* fWriter = reinterpret_cast<float*>(base + kWriterSlot);
    const float wx = fWriter[0], wy = fWriter[1], wz = fWriter[2];
    Log("IkWriter #%u: CPed*=%p writer-slot CPed+0x%X (=BB0+70) float3=(%.2f,%.2f,%.2f) "
        "sane=%d grip=%d ctrl=%d — LOG ONLY (not vecAimTarget)",
        n, ped, kWriterSlot, wx, wy, wz, SaneWorld(wx, wy, wz) ? 1 : 0,
        gripHeld ? 1 : 0, ctrlValid ? 1 : 0);

    constexpr size_t kSlots = sizeof(s_prev) / sizeof(s_prev[0]);
    int diffs = 0;
    // Compare float3s at 4-byte starts against prior sample, then refresh slot values.
    for (uint32_t off = kScanLo; off + 8 <= kScanHi; off += 4) {
      const size_t idx = (off - kScanLo) / 4;
      if (idx + 2 >= kSlots)
        break;
      auto* f = reinterpret_cast<float*>(base + off);
      const float x = f[0], y = f[1], z = f[2];
      if (s_havePrev && SaneWorld(x, y, z) && SaneWorld(s_prev[idx], s_prev[idx + 1],
                                                         s_prev[idx + 2])) {
        const float dx = x - s_prev[idx];
        const float dy = y - s_prev[idx + 1];
        const float dz = z - s_prev[idx + 2];
        const float mag2 = dx * dx + dy * dy + dz * dz;
        if (mag2 >= 0.01f && diffs < kMaxDiffs) {
          Log("IkWriter #%u: DIFF CPed+0x%X float3=(%.2f,%.2f,%.2f) d=(%.2f,%.2f,%.2f)%s",
              n, off, x, y, z, dx, dy, dz, (off == kWriterSlot) ? " [writer-slot]" : "");
          ++diffs;
        }
      }
    }
    for (uint32_t off = kScanLo; off <= kScanHi; off += 4) {
      const size_t idx = (off - kScanLo) / 4;
      if (idx >= kSlots)
        break;
      s_prev[idx] = *reinterpret_cast<float*>(base + off);
    }
    s_havePrev = true;
    if (diffs == 0 && n > 1)
      Log("IkWriter #%u: no other changing world float3s in +0x%X..+0x%X (hold game LT, "
          "sweep aim)",
          n, kScanLo, kScanHi);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("IkWriter: EXCEPTION reading ped floats — probe OFF");
  }
}

void TickIkAim(float aimX, float aimY, float aimZ, bool aiming) {
  if (!IsIkAimEnabled() || g_dead.load())
    return;
  if (!aiming)
    return;
  if (!SaneWorld(aimX, aimY, aimZ))
    return;
  if (!LoadOffsetsOnce())
    return;
  if (!EnsureFindPlayerPed() || !g_FindPlayerPed)
    return;

  void* ped = nullptr;
  __try {
    ped = g_FindPlayerPed(0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("IkAim: EXCEPTION FindPlayerPed — IK OFF");
    return;
  }
  if (!ped)
    return;

  __try {
    uint8_t* pedBytes = reinterpret_cast<uint8_t*>(ped);
    void** ikSlot = reinterpret_cast<void**>(pedBytes + g_ikMgrOff);
    void* ik = *ikSlot;
    if (!ik)
      return;
    float* target = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(ik) + g_vecOff);
    target[0] = aimX;
    target[1] = aimY;
    target[2] = aimZ;
    // Some builds store a float count/flag in [3]; leave it alone.
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("IkAim: EXCEPTION writing vecAimTarget (mgr=+0x%X vec=+0x%X) — IK OFF. "
        "Offsets wrong for CE; re-do §5.4",
        g_ikMgrOff, g_vecOff);
    return;
  }

  static uint32_t s_n = 0;
  const uint32_t n = ++s_n;
  if (n <= 8 || (n % 60) == 0)
    Log("IkAim #%u: wrote (%.1f,%.1f,%.1f) via CPed+0x%X -> +0x%X", n, aimX, aimY, aimZ,
        g_ikMgrOff, g_vecOff);
}

void IkEsiProbeHit(void* esiVal) {
  if (!IsIkEsiEnabled() || g_ikesiDead.load())
    return;

  static DWORD s_last = 0;
  const DWORD now = GetTickCount();
  if (now - s_last < 500)
    return;
  s_last = now;

  if (!EnsureFindPlayerPed() || !g_FindPlayerPed)
    return;

  void* ped = nullptr;
  __try {
    ped = g_FindPlayerPed(0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_ikesiDead.store(true);
    Log("IkEsi: EXCEPTION FindPlayerPed — probe OFF");
    return;
  }
  if (!ped || !esiVal)
    return;

  constexpr uint32_t kWriterSlot = 0xC20;
  constexpr uintptr_t kPedSpan = 0x2000;

  float fx = 0.f, fy = 0.f, fz = 0.f;
  int matchC20 = 0;
  int insidePed = 0;
  intptr_t delta = 0;
  void* dst = nullptr;
  void* pedC20 = nullptr;

  __try {
    auto* esiBytes = reinterpret_cast<uint8_t*>(esiVal);
    dst = esiBytes + 0x70;
    pedC20 = reinterpret_cast<uint8_t*>(ped) + kWriterSlot;
    matchC20 = (dst == pedC20) ? 1 : 0;

    const uintptr_t esiU = reinterpret_cast<uintptr_t>(esiVal);
    const uintptr_t pedU = reinterpret_cast<uintptr_t>(ped);
    if (esiU >= pedU && esiU < pedU + kPedSpan) {
      insidePed = 1;
      delta = static_cast<intptr_t>(esiU - pedU);
    }

    auto* f3 = reinterpret_cast<float*>(esiBytes + 0x70);
    fx = f3[0];
    fy = f3[1];
    fz = f3[2];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_ikesiDead.store(true);
    Log("IkEsi: EXCEPTION reading ESI/+0x70 — probe OFF");
    return;
  }

  Log("IkEsi: esi=%p ped=%p ped+C20=%p dst=%p f3=(%.2f,%.2f,%.2f) matchC20addr=%d"
      "%s%+d — LOG ONLY",
      esiVal, ped, pedC20, dst, fx, fy, fz, matchC20,
      insidePed ? " insidePed=1 esi-ped=" : " insidePed=0 esi-ped=",
      static_cast<int>(delta));
}

void TickIkEsi() {
  if (!IsIkEsiEnabled() || g_ikesiDead.load())
    return;
  InstallIkEsiHookOnce();
}

}  // namespace asi

extern "C" void __cdecl AsiIkEsiOnHit(void* esiVal) {
  asi::IkEsiProbeHit(esiVal);
}
