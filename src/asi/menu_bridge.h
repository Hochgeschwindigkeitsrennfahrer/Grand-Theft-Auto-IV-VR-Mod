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

// Follows a row in GTA IV's OWN pause menu (Pause > Controls).
//
// docs/MENU_INTEGRATION.md routes this through the chained
// GET_NUMBER_OF_INSTANCES_OF_STREAMED_SCRIPT native. We do not: FusionFix also
// persists its menu prefs to plugins\GTAIV.EFLC.FusionFix.cfg, and it writes
// that file the moment the row is toggled, with the game still running
// (measured 2026-07-31). Reading a file needs no script native, so this is safe
// from the EndScene thread and — unlike the native route — works on flat, where
// the CopyMat game-thread hooks never arm because they wait on 360 VR submits.
//
//   gtaiv_dxvk_vr.menukey   cfg key to follow, e.g. "LightSyncRGB".
//                           Missing = this path is off entirely.
//
// The key's value is matched against the `value=` column of
// gtaiv_dxvk_vr.menumap, so a two-state On/Off row picks between the profiles
// numbered 0 and 1. The first reading is only recorded, never applied, so
// gtaiv_dxvk_vr.stereo still decides the mode at startup.
//
// Install the row with scripts/install-vr-menu-row.ps1.
void MenuBridgeTickPrefFile();

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
