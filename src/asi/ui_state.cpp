#include "ui_state.h"

#include "aob.h"
#include "log.h"
#include "stereo_config.h"
#include "../bridge/gtaiv_xr_frame_bridge.h"

#include <windows.h>
#include <psapi.h>

#include <cstdint>

namespace asi
{
namespace
{
volatile uint8_t* g_menuActive = nullptr;
volatile uint8_t* g_loadscreenShown = nullptr;
volatile uint8_t* g_phoneShowing = nullptr;
bool g_installed = false;
uint32_t g_lastLoggedReasons = UINT32_MAX;

bool inMainModule(const void* address)
{
    HMODULE module = GetModuleHandleW(nullptr);
    MODULEINFO information {};
    if (!module
        || !address
        || !GetModuleInformation(
            GetCurrentProcess(),
            module,
            &information,
            sizeof(information)))
    {
        return false;
    }
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    const uintptr_t base =
        reinterpret_cast<uintptr_t>(information.lpBaseOfDll);
    return value >= base
        && value < base + static_cast<uintptr_t>(information.SizeOfImage);
}

bool readableByte(const volatile uint8_t* address)
{
    if (!inMainModule(const_cast<const uint8_t*>(address)))
        return false;
    MEMORY_BASIC_INFORMATION information {};
    if (VirtualQuery(
            const_cast<const uint8_t*>(address),
            &information,
            sizeof(information))
        != sizeof(information))
    {
        return false;
    }
    const DWORD blocked = PAGE_NOACCESS | PAGE_GUARD;
    return information.State == MEM_COMMIT
        && (information.Protect & blocked) == 0u;
}

volatile uint8_t* resolveAbsoluteByte(
    const char* primary,
    const char* fallback,
    const char* label)
{
    uintptr_t site = FindPattern(nullptr, primary);
    const char* selected = primary;
    if (!site && fallback)
    {
        site = FindPattern(nullptr, fallback);
        selected = fallback;
    }
    if (!site)
    {
        Log("UiState: %s signature missing", label);
        return nullptr;
    }
    if (FindPatternN(nullptr, selected, 1) != 0u)
    {
        Log("UiState: %s signature is not unique", label);
        return nullptr;
    }

    // These x86 instructions carry a 32-bit absolute address at byte +2.
    const uint32_t rawAddress =
        *reinterpret_cast<const uint32_t*>(site + 2u);
    auto* result =
        reinterpret_cast<volatile uint8_t*>(
            static_cast<uintptr_t>(rawAddress));
    if (!readableByte(result))
    {
        Log("UiState: %s state pointer rejected", label);
        return nullptr;
    }
    return result;
}
}

bool InstallUiStateProbe()
{
    if (g_installed)
        return true;
    static_assert(sizeof(void*) == 4u, "GTA UI state probe must remain x86");

    // The byte signatures are independently verified against GTA IV CE.
    // FusionFix is used only as a public cross-reference for what each
    // read-only global represents; no FusionFix implementation is copied.
    g_menuActive = resolveAbsoluteByte(
        "80 3D ? ? ? ? ? 74 4B E8 ? ? ? ? 84 C0",
        "80 3D ? ? ? ? ? C6 05 ? ? ? ? ? 74 ? "
        "C6 05 ? ? ? ? ? E8",
        "pause/map");
    g_loadscreenShown = resolveAbsoluteByte(
        "80 3D ? ? ? ? ? 53 56 8A FA",
        "80 3D ? ? ? ? ? 53 8A 5C 24 1C",
        "loading");
    g_phoneShowing = resolveAbsoluteByte(
        "C6 05 ? ? ? ? ? E8 ? ? ? ? 6A 00 E8 ? ? ? ? 8B 80",
        "88 1D ? ? ? ? 88 1D ? ? ? ? E8 ? ? ? ? 6A 00",
        "phone");

    g_installed =
        g_menuActive && g_loadscreenShown && g_phoneShowing;
    if (!g_installed)
    {
        g_menuActive = nullptr;
        g_loadscreenShown = nullptr;
        g_phoneShowing = nullptr;
        Log(
            "UiState: required UI probes incomplete; OpenXR menu "
            "presentation will not arm");
        return false;
    }

    Log(
        "UiState: pause/map, loading, and phone probes ready "
        "(read-only)");
    return true;
}

UiPresentationState GetUiPresentationState()
{
    UiPresentationState state {};
    if (!g_installed)
        return state;

    if (*g_menuActive != 0u)
        state.reasonFlags |= gtaiv_xr_bridge::UiReasonPauseOrMap;
    if (*g_loadscreenShown != 0u)
        state.reasonFlags |= gtaiv_xr_bridge::UiReasonLoading;
    if (*g_phoneShowing != 0u)
        state.reasonFlags |= gtaiv_xr_bridge::UiReasonPhone;

    if (state.reasonFlags != g_lastLoggedReasons)
    {
        Log(
            "UiState: presentation=%s reasons=0x%X",
            state ? "stationary local-space quad"
                  : GetStereoMode() == StereoMode::OpenXrImmersiveMono
                      ? "world immersive mono"
                      : "world stereo",
            state.reasonFlags);
        g_lastLoggedReasons = state.reasonFlags;
    }
    return state;
}
}
