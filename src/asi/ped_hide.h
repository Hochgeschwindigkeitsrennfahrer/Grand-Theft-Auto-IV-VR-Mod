#pragma once

namespace asi {

// Hide player head/hair/teeth/face for FP VR.
// SetDrawPlayerComponent alone only clears drawable+0x588; CE draws via +0x600.
// ped_hide.cpp clears both masks every frame. Kill: gtaiv_dxvk_vr.pedhide = 0.
// Mode 120 path is untouched.
void UpdatePedHeadHide();

}  // namespace asi
