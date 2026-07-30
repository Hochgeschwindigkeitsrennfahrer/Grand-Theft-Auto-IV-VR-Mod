#pragma once

namespace asi {

// Plan B — steer engine IK via CPedIKManager::vecAimTarget (docs/DECOUPLED_AIM_PLAN.md §5.4).
//
// Completely OFF until gtaiv_dxvk_vr.ikaim = 1 AND gtaiv_dxvk_vr.ikoffs contains
// verified CE offsets. Without that file this module never touches game memory.
//
// File format (ASCII, next to the ASI), one line:
//   <ikManagerOffHex> <vecAimTargetOffHex>
// Example (1.0.7 HINT ONLY — will crash CE if blindly used):
//   BC0 70
//
// Kill: delete gtaiv_dxvk_vr.ikaim / write 0, or delete gtaiv_dxvk_vr.ikoffs.
//
// Also logs the raw CPed* once when ikprobe=1 (for the CE Structure Dissect session).
//
// ikwriter=1 (default off): AOB-scan mov [esi+70],eax + log-only ped float3 hunt
// (~2 Hz whenever ped is valid — no VR pose / grip gate). Never writes. Kill: ikwriter=0.
//
// ikesi=1 (default off): MinHook the Session6 nearHint mov [esi+70],eax site and
// log-only dump writer ESI (~2 Hz). Never writes game memory. Kill: ikesi=0.

// Game-thread only. Writes aim point while grip/aim is held; no-ops otherwise.
void TickIkAim(float aimX, float aimY, float aimZ, bool aiming);

// Optional: log FindPlayerPed() pointer for CE. Gate: gtaiv_dxvk_vr.ikprobe = 1.
void TickIkProbe();

// Optional: hunt aim-target writer (Session 6 CE 89 46 70). Gate: ikwriter=1.
// gripHeld/ctrlValid are log annotations only — sampling does not require VR pose.
void TickIkWriter(bool gripHeld, bool ctrlValid);

// Optional: install/log ESI at Session6 writer site. Gate: ikesi=1. LOG ONLY.
void TickIkEsi();

}  // namespace asi
