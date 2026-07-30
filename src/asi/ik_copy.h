#pragma once

namespace asi {

// Session 6 Plan B — find CPed float3s that match / move with aim OUTPUT at +0xC20,
// then follow heap-looking pointers (+0xB80..+0xC40 / +0xA00..+0xE00) and SEH-scan
// each object (+0x70 first, then 0..0x120) for float3 near C20 (HEAP MATCH).
// Hardened: skip weak/zero C20 refs; reject float-ish / EXE pseudo-ptrs.
// Caps: 32 ptrs/tick, 12 logs.
//
// Gate: gtaiv_dxvk_vr.ikcopy = 1 (default off / missing) AND aim held (LT/grip).
// LOG ONLY — never writes. Does not touch ikaim / ikwriter / C20.
// Kill: delete gtaiv_dxvk_vr.ikcopy or write 0.
//
// aimHeld: pass IsVrPadAimHeld() (or grip OR). Sampling only runs while true.

void TickIkCopy(bool aimHeld);

}  // namespace asi
