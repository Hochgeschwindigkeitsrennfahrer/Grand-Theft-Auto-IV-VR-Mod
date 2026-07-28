#include "ped_hide.h"
#include "aob.h"
#include "log.h"
#include "native_invoke.h"
#include "stereo_config.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace asi {
namespace {

// C06alt FirstPerson: SET_DRAW_PLAYER_COMPONENT on ScriptHook NativeThread.
// Direct CE SetDraw from CopyMat = false green (eyes remain). Native hash path
// (Mode 150/151) aims for ScriptHook-equivalent timing via NativeInvoke.

constexpr uint32_t kSetDrawPlayerComponent = 0x3EFE3DC8;  // SET_DRAW_PLAYER_COMPONENT

using SetDrawPlayerComponent_t = void(__cdecl*)(int component, int visible);

SetDrawPlayerComponent_t g_setDraw = nullptr;
std::atomic<bool> g_resolved{false};
std::atomic<bool> g_dead{false};
std::atomic<uint32_t> g_calls{0};
std::atomic<uint32_t> g_ok{0};
std::atomic<bool> g_nativeLogged{false};

bool ResolveSetDraw() {
  if (g_resolved.load())
    return g_setDraw != nullptr;
  g_resolved.store(true);

  const uintptr_t p = FindPattern(
      nullptr,
      "6A 00 E8 ? ? ? ? 83 C4 04 85 C0 74 12 6A 00 E8 ? ? ? ? 8B 90 28 02 00 00");
  if (!p) {
    Log("PedHide: AOB MISS — set gtaiv_dxvk_vr.pedhide=0");
    return false;
  }
  g_setDraw = reinterpret_cast<SetDrawPlayerComponent_t>(p);
  Log("PedHide: SetDraw @ %p (CE helper; prefer Mode150/151 native path for eyes)",
      reinterpret_cast<void*>(p));
  return true;
}

void HideOneHelper(int component) {
  if (!g_setDraw || g_dead.load())
    return;
  __try {
    g_setDraw(component, 0);
    ++g_ok;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("PedHide: EXCEPTION on component %d — DISABLED", component);
  }
}

void HideOneNative(int component) {
  __try {
    InvokeNativeVoid_I32_I32(kSetDrawPlayerComponent, component, 0);
    ++g_ok;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_dead.store(true);
    Log("PedHide: NATIVE EXCEPTION on component %d — DISABLED", component);
  }
}

}  // namespace

void UpdatePedHeadHide() {
  const bool nativeMode = WantsNativePedHide(GetStereoMode());
  if (!nativeMode && !IsPedHideEnabled()) {
    static std::atomic<bool> s_offLogged{false};
    if (!s_offLogged.exchange(true))
      Log("PedHide: OFF (gtaiv_dxvk_vr.pedhide=0; Mode150/151 force native)");
    return;
  }
  if (g_dead.load())
    return;

  const uint32_t n = ++g_calls;
  if (n > 1 && (n % 30) != 0)
    return;

  if (nativeMode) {
    if (!InitNativeInvoke()) {
      if (!g_nativeLogged.exchange(true))
        Log("PedHide: native path waiting (InitNativeInvoke not ready yet)");
      return;
    }
    if (!g_nativeLogged.exchange(true))
      Log("PedHide: NATIVE SET_DRAW_PLAYER_COMPONENT path ON (Mode150/151)");
    HideOneNative(0);
    HideOneNative(7);
    HideOneNative(8);
    HideOneNative(9);
    HideOneNative(10);
  } else {
    if (!ResolveSetDraw())
      return;
    HideOneHelper(0);
    HideOneHelper(7);
    HideOneHelper(8);
    HideOneHelper(9);
    HideOneHelper(10);
  }

  if (n == 1 || (n % 300) == 0) {
    Log("PedHide: #%u ok=%u dead=%d native=%d", n, g_ok.load(), g_dead.load() ? 1 : 0,
        nativeMode ? 1 : 0);
  }
}

}  // namespace asi
