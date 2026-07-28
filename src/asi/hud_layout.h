#pragma once

namespace asi {

// Mode 166: VR-inset HUD via common/data/hud.dat (illustration: corners → inward).
// Call Ensure on enter 166; RestoreHudDatStock when leaving 166 / kill to 162.
void UpdateHudLayoutForVr();
void EnsureHudOffDefaults();
void RestoreHudDatStock();

}  // namespace asi
