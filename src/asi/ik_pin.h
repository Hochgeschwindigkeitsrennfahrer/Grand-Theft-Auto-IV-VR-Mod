#pragma once

namespace asi {

// Plan B pin — LOG ONLY. Candidate from IkRay HEAP MATCH:
//   CPed+0xD58 → heapP, float3 at P+0x90 ≈ ComputeAimOrigin (ctrl world).
// While aim held: hardened ptr read + float3 @ +0x90 vs ctrl/cam origin.
// Sibling +0xA0 logged once. ~2 Hz. SEH. NO writes. No PAGE_GUARD.
//
// Gate: gtaiv_dxvk_vr.ikpin = 1 (default off) AND aim held (LT/grip).
// Kill: write 0 or delete gtaiv_dxvk_vr.ikpin.
// Keep ikray=0 ikread=0 ikaim=0 while testing.

bool IsIkPinEnabled();
void TickIkPin(bool aimHeld);

}  // namespace asi
