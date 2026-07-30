#pragma once

namespace asi {

// Render-profile switching without editing files by hand (docs/MENU_INTEGRATION.md).
//
// Three input routes, all optional, all off unless configured:
//   1. FusionFix pause-menu row. FusionFix exposes every PREF_* (its own custom
//      ones included) through the GET_NUMBER_OF_INSTANCES_OF_STREAMED_SCRIPT
//      native — see FusionFix source/settings.ixx. We poll that by name, so a
//      real menu row needs no hook on our side.
//        gtaiv_dxvk_vr.menupref = PREF_VRMODE   (name of the row to follow)
//   2. VR controller: right menu button (VrMenu action) cycles the profile.
//   3. Keyboard F4 cycles the profile (F5..F11 are already taken).
//
// Profile list comes from gtaiv_dxvk_vr.menumap, one "value=stereoMode" per
// line. Default: 0=243 (TrueStereo late-latch), 1=120 (AER/temporal), 2=0 (off).
// Switching writes gtaiv_dxvk_vr.stereo and calls ReloadStereoMode(); families
// that need a fresh hook install still want a restart — the log says which.
//
// Kill: delete gtaiv_dxvk_vr.menupref and gtaiv_dxvk_vr.menumap.

// Game-thread only (same sites as TickAimFromGameThread). Rate-limited inside.
void TickMenuBridge();

// Current profile index, or -1 when the bridge is inactive.
int GetMenuProfileIndex();

// Read a FusionFix preference by name. Returns defValue when FusionFix or the
// native is unavailable. Game-thread only.
int FusionFixGetPref(const char* prefName, int defValue);

}  // namespace asi
