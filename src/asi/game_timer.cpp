#include "game_timer.h"

#include "aob.h"
#include "log.h"

#include <atomic>
#include <cstdint>
#include <windows.h>

namespace asi {
namespace {

std::atomic<float*> g_timeStep{nullptr};
std::atomic<float*> g_timeScale1{nullptr};
std::atomic<int32_t*> g_timeMs{nullptr};
std::atomic<uint8_t*> g_codePause{nullptr};
std::atomic<bool> g_resolveTried{false};
std::atomic<bool> g_freezeActive{false};

float g_savedStep = 0.f;
float g_savedScale1 = 0.f;
int32_t g_savedMs = 0;
uint8_t g_savedCodePause = 0;
bool g_haveStep = false;
bool g_haveScale1 = false;
bool g_haveMs = false;
bool g_haveCodePause = false;

bool PointerInMainModule(const void* p) {
  HMODULE mod = GetModuleHandleW(nullptr);
  if (!mod || !p)
    return false;
  const auto base = reinterpret_cast<uintptr_t>(mod);
  const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;
  const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return false;
  const auto addr = reinterpret_cast<uintptr_t>(p);
  return addr >= base && addr < base + nt->OptionalHeader.SizeOfImage;
}

template <typename T>
T* ResolveRip(const char* pat, int ptrOff, const char* name) {
  const uintptr_t site = FindPattern(nullptr, pat);
  if (!site)
    return nullptr;
  T* p = *reinterpret_cast<T**>(site + ptrOff);
  if (!PointerInMainModule(p)) {
    Log("GameTimer: reject %s %p (outside GTAIV.exe)", name, reinterpret_cast<void*>(p));
    return nullptr;
  }
  Log("GameTimer: %s @ %p", name, reinterpret_cast<void*>(p));
  return p;
}

void TryResolveAll() {
  // FusionFix comvars.ixx CTimer (CE + legacy patterns).
  static const char* kStepPats[] = {
      "F3 0F 10 05 ? ? ? ? F3 0F 59 05 ? ? ? ? 8B 43 20 53",
      "F3 0F 10 05 ? ? ? ? F3 0F 59 44 24 ? 83 C4 04 83 7C 24",
  };
  for (const char* pat : kStepPats) {
    if (float* p = ResolveRip<float>(pat, 4, "ms_fTimeStep")) {
      g_timeStep.store(p);
      break;
    }
  }

  static const char* kScalePats[] = {
      "F3 0F 10 05 ? ? ? ? F3 0F 10 0D ? ? ? ? 0F 2F C8 F3 0F 11 44 24",
      "F3 0F 10 05 ? ? ? ? 0F 2F C8 77 ? F3 0F 10 05",
  };
  for (const char* pat : kScalePats) {
    if (float* p = ResolveRip<float>(pat, 4, "fTimeScale1")) {
      g_timeScale1.store(p);
      break;
    }
  }

  static const char* kMsPats[] = {
      "A1 ? ? ? ? A3 ? ? ? ? EB 3A",
      "A1 ? ? ? ? 39 05 ? ? ? ? 76 1F",
  };
  for (const char* pat : kMsPats) {
    if (int32_t* p = ResolveRip<int32_t>(pat, 1, "m_snTimeInMilliseconds")) {
      g_timeMs.store(p);
      break;
    }
  }

  // "0A 05 pause1  0A 05 pause2" — UserPause at +2, CodePause at +8 (CE).
  static const char* kPausePats[] = {
      "0A 05 ? ? ? ? 0A 05 ? ? ? ? 75 38",
      "0A 05 ? ? ? ? 0A 05",
  };
  for (const char* pat : kPausePats) {
    const uintptr_t site = FindPattern(nullptr, pat);
    if (!site)
      continue;
    uint8_t* user = *reinterpret_cast<uint8_t**>(site + 2);
    uint8_t* code = *reinterpret_cast<uint8_t**>(site + 8);
    if (!PointerInMainModule(user) || !PointerInMainModule(code)) {
      Log("GameTimer: reject pause flags user=%p code=%p", reinterpret_cast<void*>(user),
          reinterpret_cast<void*>(code));
      continue;
    }
    g_codePause.store(code);
    Log("GameTimer: m_CodePause @ %p (UserPause @ %p)", reinterpret_cast<void*>(code),
        reinterpret_cast<void*>(user));
    break;
  }

  if (!g_timeStep.load())
    Log("GameTimer: FAILED ms_fTimeStep — Mode189 step freeze no-op");
  if (!g_timeMs.load())
    Log("GameTimer: FAILED m_snTimeInMilliseconds — Mode189 ms pin no-op");
  if (!g_codePause.load())
    Log("GameTimer: FAILED m_CodePause — Mode189 code-pause no-op");
}

}  // namespace

bool ResolveGameTimer() {
  if (g_resolveTried.load())
    return g_timeStep.load() != nullptr || g_timeMs.load() != nullptr ||
           g_codePause.load() != nullptr;
  g_resolveTried.store(true);
  TryResolveAll();
  return g_timeStep.load() != nullptr || g_timeMs.load() != nullptr ||
         g_codePause.load() != nullptr;
}

void BeginDualTimeFreeze() {
  if (g_freezeActive.load())
    return;
  ResolveGameTimer();

  g_haveStep = false;
  g_haveScale1 = false;
  g_haveMs = false;
  g_haveCodePause = false;

  if (float* p = g_timeStep.load()) {
    if (PointerInMainModule(p)) {
      g_savedStep = *p;
      *p = 0.f;
      g_haveStep = true;
    }
  }
  if (float* p = g_timeScale1.load()) {
    if (PointerInMainModule(p)) {
      g_savedScale1 = *p;
      // Keep scale; zero step is the main lever. Do not zero scale1 (can desync audio).
      g_haveScale1 = true;
    }
  }
  if (int32_t* p = g_timeMs.load()) {
    if (PointerInMainModule(p)) {
      g_savedMs = *p;
      g_haveMs = true;
    }
  }
  if (uint8_t* p = g_codePause.load()) {
    if (PointerInMainModule(p)) {
      g_savedCodePause = *p;
      *p = 1;
      g_haveCodePause = true;
    }
  }

  if (!g_haveStep && !g_haveMs && !g_haveCodePause)
    return;

  g_freezeActive.store(true);
  static bool s_once = false;
  if (!s_once) {
    s_once = true;
    Log("Mode189: same-tick freeze armed step=%d(%.5f→0) msPin=%d(%d) codePause=%d(%u→1)",
        g_haveStep ? 1 : 0, g_savedStep, g_haveMs ? 1 : 0, g_savedMs,
        g_haveCodePause ? 1 : 0, static_cast<unsigned>(g_savedCodePause));
  }
}

void RepinDualTimeMs() {
  if (!g_freezeActive.load() || !g_haveMs)
    return;
  if (int32_t* p = g_timeMs.load()) {
    if (PointerInMainModule(p))
      *p = g_savedMs;
  }
}

void EndDualTimeFreeze() {
  if (!g_freezeActive.load())
    return;

  // Restore step + CodePause. Leave ms at pinned value (equals saved if pause held).
  if (g_haveMs) {
    if (int32_t* p = g_timeMs.load()) {
      if (PointerInMainModule(p))
        *p = g_savedMs;
    }
  }
  if (g_haveStep) {
    if (float* p = g_timeStep.load()) {
      if (PointerInMainModule(p))
        *p = g_savedStep;
    }
  }
  if (g_haveCodePause) {
    if (uint8_t* p = g_codePause.load()) {
      if (PointerInMainModule(p))
        *p = g_savedCodePause;
    }
  }

  g_freezeActive.store(false);
  g_haveStep = false;
  g_haveScale1 = false;
  g_haveMs = false;
  g_haveCodePause = false;
}

bool IsDualTimeFreezeActive() {
  return g_freezeActive.load();
}

}  // namespace asi
