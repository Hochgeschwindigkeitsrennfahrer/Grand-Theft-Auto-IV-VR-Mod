#pragma once

namespace asi {

// Plan B — LOG-ONLY hunt for CPed / heap float3s near ctrl/cam WORLD ORIGIN
// (ComputeAimOrigin = cam+(ctrlTrk−hmdTrk)). NOT far ray hit. NOT C20 (aim OUTPUT).
// No writes. No PAGE_GUARD.
//
// Level-1: hardened ptrs from CPed → float3 on P (prefer +0x70).
// Level-2: for each P, hardened Q at P+0..+0x100 → float3 on Q (prefer +0x70).
//
// Gate: gtaiv_dxvk_vr.ikray = 1 (default off) AND aim held (LT/grip).
// Kill: write 0 or delete gtaiv_dxvk_vr.ikray.
// Keep ikread=0 ikaim=0 (PAGE_GUARD abandoned — crash, 0 EIPs).

bool IsIkRayEnabled();
void TickIkRay(bool aimHeld);

}  // namespace asi
