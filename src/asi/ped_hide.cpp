#include "ped_hide.h"
#include "aob.h"
#include "log.h"
#include "stereo_config.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace asi {
namespace {

// Inspiration FirstPerson.asi (C06alt) hides head via ScriptHook NativeThread +
// SET_DRAW_PLAYER_COMPONENT. Calling that native through NativeContext from
// EndScene/CopyMat CRASHED CE (2026-07-23).
//
// CE internal implementation (no script TLS): unique AOB → cdecl
//   void SetDrawPlayerComponent(int component, int visible)
// which toggles bits on the player drawable (1<<id at drawable+0x588).
// Same effect as the Inspiration native, without ScriptHook / script thread.
//
// Components (gtamods): Head=0 Hair=7 Teeth=9 Face=10.

using SetDrawPlayerComponent_t = void(__cdecl*)(int component, int visible);

SetDrawPlayerComponent_t g_setDraw = nullptr;
std::atomic<bool> g_resolved{false};
std::atomic<bool> g_dead{false};
std::atomic<uint32_t> g_calls{0};
std::atomic<uint32_t> g_ok{0};

bool ResolveSetDraw() {
  if (g_resolved.load())
    return g_setDraw != nullptr;
  g_resolved.store(true);

  // Unique in GTAIV.exe CE 1.2.0.59 (file RVA 0x7B2910 / VA 0xBB2910 @ ImageBase 400000).
  const uintptr_t p = FindPattern(
      nullptr,
      "6A 00 E8 ? ? ? ? 83 C4 04 85 C0 74 12 6A 00 E8 ? ? ? ? 8B 90 28 02 00 00");
  if (!p) {
    Log("PedHide: AOB MISS — keep eyeForward; set gtaiv_dxvk_vr.pedhide=0 to silence");
    return false;
  }
  g_setDraw = reinterpret_cast<SetDrawPlayerComponent_t>(p);
  Log("PedHide: SetDrawPlayerComponent @ %p (Inspiration-style, NO natives/ScriptHook)",
      reinterpret_cast<void*>(p));
  return true;
}

void HideOne(int component) {
  if (!g_setDraw || g_dead.load())
    return;
  __try {
    g_setDraw(component, 0);
    ++g_ok;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("PedHide: EXCEPTION on component %d — DISABLED (use eyeForward; pedhide=0)", component);
  }
}

}  // namespace

void UpdatePedHeadHide() {
  if (!IsPedHideEnabled()) {
    static std::atomic<bool> s_offLogged{false};
    if (!s_offLogged.exchange(true))
      Log("PedHide: OFF (gtaiv_dxvk_vr.pedhide=0) — eyeForward only");
    return;
  }
  if (g_dead.load())
    return;
  if (!ResolveSetDraw())
    return;

  // Game may restore draw flags; refresh ~2×/sec while FP cam is live.
  const uint32_t n = ++g_calls;
  if (n > 1 && (n % 30) != 0)
    return;

  HideOne(0);   // head
  HideOne(7);   // hair
  HideOne(9);   // teeth
  HideOne(10);  // face

  if (n == 1 || (n % 300) == 0) {
    Log("PedHide: head/hair/teeth/face off via SetDraw# %u ok=%u dead=%d (kill: pedhide=0)",
        n, g_ok.load(), g_dead.load() ? 1 : 0);
  }
}

}  // namespace asi
