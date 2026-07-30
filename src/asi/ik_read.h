#pragma once

namespace asi {

// Session 6 Plan B — find who READS CPed+0xC20 (aim OUTPUT slot; freeze MISS).
// While aim held: VEH + PAGE_GUARD on the page containing ped+0xC20.
// On EXCEPTION_GUARD_PAGE, if fault addr in [ped+C20, ped+C20+12), record unique
// READ/WRITE EIP (ExceptionInformation[0]). Re-arm. Cap unique EIPs.
// Disarm: aim release, unique-EIP cap, or C20-hit cap — NEVER raw pageFault storm
// (ped page is hot; pageFaults is diagnostic only).
//
// Gate: gtaiv_dxvk_vr.ikread = 1 (default off / missing) AND aim held.
// LOG ONLY — never writes game data. Does not touch ikaim / C20 values.
// Kill: delete gtaiv_dxvk_vr.ikread or write 0.
//
// Noise cut: set ikray=0 ikaim=0 while testing.
// Why not 2-level heap float3: ped float3 scans exhausted (0 MATCH); need READ EIPs.

void TickIkRead(bool aimHeld);

}  // namespace asi
