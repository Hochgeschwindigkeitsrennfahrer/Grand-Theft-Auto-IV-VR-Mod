#include "ik_read.h"
#include "aob.h"
#include "log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace asi {
namespace {

using FindPlayerPed_t = void*(__cdecl*)(int32_t);
FindPlayerPed_t g_FindPlayerPed = nullptr;

constexpr uint32_t kOffC20 = 0xC20;
constexpr uint32_t kWatchBytes = 12;
constexpr int kMaxUnique = 64;
// Safety only: after this many C20-range hits, disarm (unique EIPs already logged).
// Never storm on raw pageFaults — the ped page is hot; almost all faults are non-C20.
constexpr uint32_t kC20HitCap = 2000;
constexpr DWORD kFlushMs = 250;
constexpr uintptr_t kCeReaderHint = 0x7228AD3Au;  // CE abs (ASLR); proximity tag only
constexpr uintptr_t kCeNearWindow = 0x100000u;    // 1 MiB

std::atomic<bool> g_dead{false};
std::atomic<bool> g_armed{false};
std::atomic<uintptr_t> g_page{0};
std::atomic<uintptr_t> g_watchLo{0};
std::atomic<uintptr_t> g_watchHi{0};
std::atomic<uint32_t> g_pageFaults{0};  // diagnostic only (whole ped page)
std::atomic<uint32_t> g_c20Hits{0};
std::atomic<uint32_t> g_doneFlag{0};  // 1=unique cap, 2=c20Hit cap

DWORD g_savedProt = PAGE_READWRITE;
PVOID g_veh = nullptr;

// Lock-free ring: VEH pushes, Tick drains. Never Log() from VEH (mutex/alloc unsafe).
struct Hit {
  uintptr_t eip = 0;
  uint32_t offC20 = 0;  // fault addr − (ped+C20)
  uint32_t isWrite = 0;
};
constexpr uint32_t kQCap = 256;
Hit g_q[kQCap]{};
std::atomic<uint32_t> g_qHead{0};
std::atomic<uint32_t> g_qTail{0};

uintptr_t g_seenEip[kMaxUnique]{};
int g_seenN = 0;

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

bool IsIkReadEnabled() {
  static const int v = ReadIntFile("gtaiv_dxvk_vr.ikread", 0);
  return v != 0;
}

bool EnsureFindPlayerPed() {
  if (g_FindPlayerPed)
    return true;
  const uintptr_t p = FindPattern(nullptr, "8B 44 24 04 85 C0 75 18 A1");
  if (!p)
    return false;
  g_FindPlayerPed = reinterpret_cast<FindPlayerPed_t>(p);
  Log("IkRead: FindPlayerPed @ %p — LOG ONLY kill: ikread=0", reinterpret_cast<void*>(p));
  return true;
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

bool PushHit(uintptr_t eip, uint32_t offC20, uint32_t isWrite) {
  const uint32_t head = g_qHead.load(std::memory_order_relaxed);
  const uint32_t next = (head + 1u) % kQCap;
  if (next == g_qTail.load(std::memory_order_acquire))
    return false;  // full — drop
  g_q[head].eip = eip;
  g_q[head].offC20 = offC20;
  g_q[head].isWrite = isWrite;
  g_qHead.store(next, std::memory_order_release);
  return true;
}

bool PopHit(Hit* out) {
  const uint32_t tail = g_qTail.load(std::memory_order_relaxed);
  if (tail == g_qHead.load(std::memory_order_acquire))
    return false;
  *out = g_q[tail];
  g_qTail.store((tail + 1u) % kQCap, std::memory_order_release);
  return true;
}

bool SeenEip(uintptr_t eip) {
  for (int i = 0; i < g_seenN; ++i) {
    if (g_seenEip[i] == eip)
      return true;
  }
  return false;
}

void RememberEip(uintptr_t eip) {
  if (g_seenN >= kMaxUnique)
    return;
  g_seenEip[g_seenN++] = eip;
}

DWORD AccessOnly(DWORD prot) {
  // Strip modifier bits; keep PAGE_READONLY / READWRITE / EXECUTE_* etc.
  return prot & (PAGE_NOACCESS | PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                 PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                 PAGE_EXECUTE_WRITECOPY);
}

void ClearGuardUnlocked() {
  g_armed.store(false, std::memory_order_release);
  const uintptr_t page = g_page.exchange(0, std::memory_order_acq_rel);
  g_watchLo.store(0, std::memory_order_release);
  g_watchHi.store(0, std::memory_order_release);
  if (!page)
    return;
  DWORD tmp = 0;
  const DWORD prot = AccessOnly(g_savedProt);
  VirtualProtect(reinterpret_cast<void*>(page), 0x1000, prot ? prot : PAGE_READWRITE, &tmp);
}

LONG CALLBACK IkReadVeh(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
    return EXCEPTION_CONTINUE_SEARCH;
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_GUARD_PAGE)
    return EXCEPTION_CONTINUE_SEARCH;

  const uintptr_t page = g_page.load(std::memory_order_acquire);
  if (!page)
    return EXCEPTION_CONTINUE_SEARCH;

  const uintptr_t addr = static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
  if ((addr & ~static_cast<uintptr_t>(0xFFFu)) != page)
    return EXCEPTION_CONTINUE_SEARCH;

  // This fault consumed PAGE_GUARD on our page — always continue execution.
  const bool armed = g_armed.load(std::memory_order_acquire);
  g_pageFaults.fetch_add(1, std::memory_order_relaxed);  // diagnostic only

  if (armed) {
    const uintptr_t lo = g_watchLo.load(std::memory_order_acquire);
    const uintptr_t hi = g_watchHi.load(std::memory_order_acquire);
    if (lo && addr >= lo && addr < hi) {
      // GUARD_PAGE: info[0] bit0 = write (1) vs read (0).
      const uint32_t isWrite =
          (ep->ExceptionRecord->NumberParameters >= 1)
              ? static_cast<uint32_t>(ep->ExceptionRecord->ExceptionInformation[0] & 1u)
              : 0u;
      const uint32_t hits = g_c20Hits.fetch_add(1, std::memory_order_relaxed) + 1;
      PushHit(static_cast<uintptr_t>(ep->ContextRecord->Eip),
              static_cast<uint32_t>(addr - lo), isWrite);

      // Disarm only on C20-hit volume — never on raw pageFaults.
      if (hits >= kC20HitCap) {
        g_armed.store(false, std::memory_order_release);
        g_doneFlag.store(2, std::memory_order_release);
        g_dead.store(true, std::memory_order_release);
        // Leave page unguarded (guard already cleared by OS).
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }

    // Re-arm while still watching. Ped page is hot; non-C20 faults are expected.
    DWORD tmp = 0;
    const DWORD base = AccessOnly(g_savedProt);
    VirtualProtect(reinterpret_cast<void*>(page), 0x1000,
                   (base ? base : PAGE_READWRITE) | PAGE_GUARD, &tmp);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

bool EnsureVeh() {
  if (g_veh)
    return true;
  g_veh = AddVectoredExceptionHandler(1, IkReadVeh);
  if (!g_veh) {
    g_dead.store(true);
    Log("IkRead: AddVectoredExceptionHandler FAIL — probe OFF");
    return false;
  }
  Log("IkRead: VEH installed — PAGE_GUARD on ped+C20 page while aim held. LOG ONLY. kill: ikread=0");
  return true;
}

bool ArmGuard(void* ped) {
  if (!ped || g_dead.load())
    return false;
  if (!EnsureVeh())
    return false;

  const uintptr_t watch = reinterpret_cast<uintptr_t>(ped) + kOffC20;
  const uintptr_t page = watch & ~static_cast<uintptr_t>(0xFFFu);

  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<void*>(watch), &mbi, sizeof(mbi))) {
    Log("IkRead: VirtualQuery FAIL @%p — probe OFF", reinterpret_cast<void*>(watch));
    g_dead.store(true);
    return false;
  }
  if (mbi.State != MEM_COMMIT) {
    Log("IkRead: page not committed @%p — probe OFF", reinterpret_cast<void*>(watch));
    g_dead.store(true);
    return false;
  }

  // If already armed on same page/watch, nothing to do.
  if (g_armed.load(std::memory_order_acquire) && g_page.load() == page &&
      g_watchLo.load() == watch)
    return true;

  ClearGuardUnlocked();

  g_savedProt = AccessOnly(mbi.Protect);
  if (!g_savedProt)
    g_savedProt = PAGE_READWRITE;

  // Publish watch range before arming so VEH can filter the first hit.
  g_page.store(page, std::memory_order_release);
  g_watchLo.store(watch, std::memory_order_release);
  g_watchHi.store(watch + kWatchBytes, std::memory_order_release);

  DWORD tmp = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(page), 0x1000, g_savedProt | PAGE_GUARD, &tmp)) {
    g_page.store(0, std::memory_order_release);
    g_watchLo.store(0, std::memory_order_release);
    g_watchHi.store(0, std::memory_order_release);
    Log("IkRead: VirtualProtect PAGE_GUARD FAIL err=%lu @%p — probe OFF", GetLastError(),
        reinterpret_cast<void*>(page));
    g_dead.store(true);
    return false;
  }

  g_armed.store(true, std::memory_order_release);
  return true;
}

void FlushHits(DWORD now) {
  static DWORD s_lastFlush = 0;
  static DWORD s_lastStatus = 0;
  static bool s_capLogged = false;

  Hit h{};
  int drained = 0;
  while (PopHit(&h) && drained < 64) {
    ++drained;
    if (SeenEip(h.eip))
      continue;
    if (g_seenN >= kMaxUnique) {
      if (!s_capLogged) {
        s_capLogged = true;
        Log("IkRead: unique EIP cap %d reached — further EIPs ignored", kMaxUnique);
      }
      continue;
    }
    RememberEip(h.eip);

    uintptr_t exeLo = 0, exeHi = 0;
    GetExeRange(&exeLo, &exeHi);
    const char* rw = h.isWrite ? "WRITE" : "READ";

    if (exeLo && h.eip >= exeLo && h.eip < exeHi) {
      Log("IkRead: %s eip=0x%08X GTAIV+0x%X addr=ped+C20+0x%X unique=%d/%d", rw,
          static_cast<unsigned>(h.eip), static_cast<unsigned>(h.eip - exeLo), h.offC20, g_seenN,
          kMaxUnique);
    } else {
      char modPath[MAX_PATH]{};
      HMODULE mod = nullptr;
      const char* modName = "?";
      uintptr_t modBase = 0;
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(h.eip), &mod) &&
          mod) {
        modBase = reinterpret_cast<uintptr_t>(mod);
        if (GetModuleFileNameA(mod, modPath, MAX_PATH)) {
          const char* slash = strrchr(modPath, '\\');
          modName = slash ? slash + 1 : modPath;
        }
      }
      if (modBase) {
        Log("IkRead: %s eip=0x%08X %s+0x%X addr=ped+C20+0x%X unique=%d/%d", rw,
            static_cast<unsigned>(h.eip), modName, static_cast<unsigned>(h.eip - modBase),
            h.offC20, g_seenN, kMaxUnique);
      } else {
        Log("IkRead: %s eip=0x%08X addr=ped+C20+0x%X unique=%d/%d", rw,
            static_cast<unsigned>(h.eip), h.offC20, g_seenN, kMaxUnique);
      }
    }

    // CE-known rare reader abs (may miss under ASLR — still log abs EIP above).
    const uintptr_t d =
        (h.eip > kCeReaderHint) ? (h.eip - kCeReaderHint) : (kCeReaderHint - h.eip);
    if (d <= kCeNearWindow) {
      Log("IkRead: CE-near eip=0x%08X (hint 0x%08X delta=%u) — rare reader region",
          static_cast<unsigned>(h.eip), static_cast<unsigned>(kCeReaderHint),
          static_cast<unsigned>(d));
    }
  }

  if (now - s_lastFlush >= kFlushMs)
    s_lastFlush = now;

  if (now - s_lastStatus >= 2000 && g_armed.load()) {
    s_lastStatus = now;
    Log("IkRead: status armed=1 pageFaults=%u(c20Hits=%u) unique=%d/%d (storm=c20/unique only)",
        g_pageFaults.load(), g_c20Hits.load(), g_seenN, kMaxUnique);
  }
}

}  // namespace

void TickIkRead(bool aimHeld) {
  if (!IsIkReadEnabled())
    return;

  static bool s_banner = false;
  if (!s_banner) {
    s_banner = true;
    Log("IkRead: enabled — VEH+PAGE_GUARD on CPed+0xC20 page while aim held; log only "
        "[C20,C20+12) READ/WRITE EIPs. Disarm: aim release / %d unique / %u c20Hits. "
        "NOT pageFault storm. kill: ikread=0 (ikray=0 ikaim=0).",
        kMaxUnique, kC20HitCap);
  }

  const uint32_t done = g_doneFlag.exchange(0);
  if (done == 2) {
    ClearGuardUnlocked();
    Log("IkRead: c20Hit cap (>%u C20-range hits) — probe OFF. Unique EIPs kept above. "
        "kill: ikread=0",
        kC20HitCap);
  } else if (done == 1) {
    ClearGuardUnlocked();
    Log("IkRead: unique EIP cap %d — probe OFF. kill: ikread=0", kMaxUnique);
  }

  if (g_dead.load()) {
    // Drain any late hits once, then idle.
    FlushHits(GetTickCount());
    return;
  }

  if (!aimHeld) {
    if (g_armed.load()) {
      ClearGuardUnlocked();
      Log("IkRead: guard cleared (aim released)");
    }
    FlushHits(GetTickCount());
    return;
  }

  if (!EnsureFindPlayerPed() || !g_FindPlayerPed)
    return;

  void* ped = nullptr;
  __try {
    ped = g_FindPlayerPed(0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ClearGuardUnlocked();
    g_dead.store(true);
    Log("IkRead: EXCEPTION FindPlayerPed — probe OFF");
    return;
  }
  if (!ped)
    return;

  if (!ArmGuard(ped))
    return;

  FlushHits(GetTickCount());

  // Disarm after N unique C20 EIPs (Tick-side; VEH never tracks unique list).
  if (g_seenN >= kMaxUnique && g_armed.load()) {
    ClearGuardUnlocked();
    g_dead.store(true);
    g_doneFlag.store(0);  // already logging here
    Log("IkRead: unique EIP cap %d reached — probe OFF. kill: ikread=0", kMaxUnique);
  }
}

}  // namespace asi
