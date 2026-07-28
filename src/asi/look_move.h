#pragma once

namespace asi {

// Clean look-move: HMD yaw → ped heading (GTA atan2(-fx, fy)).
// File gtaiv_dxvk_vr.lookmove: 1=on, 0=off. Mode 120 forces ON.
// pedCoupled stays OFF for pure HMD cam — this module owns body facing only.
bool IsLookMoveEnabled();

// Call each VR frame after WaitGetPoses (replaces bare UpdateVrMoveAndStick).
// Always updates right-stick yaw offset; forces ped heading when look-move on.
void UpdateLookMove();

}  // namespace asi
