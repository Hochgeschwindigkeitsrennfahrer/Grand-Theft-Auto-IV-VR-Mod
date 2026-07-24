#pragma once

namespace asi {

// Hide player head/hair/teeth/face for FP VR.
// Inspiration FirstPerson.asi uses SET_DRAW_PLAYER_COMPONENT on a ScriptHook
// NativeThread. We call the same CE helper directly (no script TLS) — see
// ped_hide.cpp. Kill: gtaiv_dxvk_vr.pedhide = 0.
void UpdatePedHeadHide();

}  // namespace asi
