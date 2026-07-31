#pragma once

namespace asi {

// Render-profile model shared by the overlay menu and the F4 hotkey.
// See docs/menu-integration/ and docs/MENU_INTEGRATION.md.
//
// A "profile" is just a named stereo mode. The list is data, not code:
//
//   gtaiv_dxvk_vr.menumap   one "value=stereoMode" per line, e.g.
//                             0=243
//                             1=53
//                             2=0
//                           Missing file = built-in default (243 / 53 / 0).
//
// Selecting a profile writes gtaiv_dxvk_vr.stereo and calls ReloadStereoMode().
// Families that arm their hooks at startup still want a restart — the log says
// which. `value` is the FusionFix preference value that would select this
// profile; it is kept so the list stays compatible with the pause-menu bridge
// described in docs/MENU_INTEGRATION.md section 2, which lives on the branch
// that has a game-thread tick (natives must not be invoked from EndScene).
//
// Kill: delete gtaiv_dxvk_vr.menumap to fall back to the built-in list.

// Reads the profile files. Idempotent; safe from any thread.
void MenuBridgeInit();

// F4 cycles to the next profile. Render-thread safe: touches no game natives.
void MenuBridgeTickHotkey();

int MenuBridgeProfileCount();
// Stereo mode number for a profile, or -1 when idx is out of range.
int MenuBridgeProfileMode(int idx);
// "243", "53", "0 (flat)" — stable pointer, valid for the process lifetime.
const char* MenuBridgeProfileLabel(int idx);
// Index of the profile matching the live stereo mode, or -1 when none does.
int MenuBridgeCurrentProfile();
// Applies profile `idx`. `why` is echoed into the log line.
void MenuBridgeApplyProfile(int idx, const char* why);

}  // namespace asi
