#pragma once

#include <d3d9.h>

// In-game settings overlay (docs/menu-integration/).
//
// This is the flat-testable half of docs/MENU_INTEGRATION.md: instead of adding
// a row to GTA IV's own pause menu — which needs a FusionFix preference-table
// patch and a headset to judge — the mod draws its own panel into the frame.
// One draw serves both outputs, because it lands in the backbuffer before the
// VR path copies it, so the panel is visible on the monitor with no headset
// connected AND inside both eyes when SteamVR is running.
//
// Keys (see kToggleKey and friends in vr_menu.cpp):
//   F3            open / close
//   Up/Down       or Num8/Num2   move selection
//   Left/Right    or Num4/Num6   change value  (hold Shift for x5 steps)
//   Enter         or Num5        run the selected action
//
// Every change is applied live and written to the same gtaiv_dxvk_vr.* file the
// matching F5..F11 hotkey writes, so a setting survives a restart and the menu
// can never disagree with the hotkeys.
//
// Kill: put a single 0 in gtaiv_dxvk_vr.menu next to the ASI. The overlay then
// never allocates, never polls keys and never touches device state.

namespace asi {

// Call from the D3D9 EndScene hook BEFORE the VR submit path reads the
// backbuffer. Cheap when the menu is closed (one key poll, no device calls).
void VrMenuOnEndScene(IDirect3DDevice9* dev);

bool VrMenuIsOpen();

// Opens or closes the overlay without a key press. Used by the offline render
// test (tools/menu_preview) and by any future controller binding.
void VrMenuSetOpen(bool open);

}  // namespace asi
