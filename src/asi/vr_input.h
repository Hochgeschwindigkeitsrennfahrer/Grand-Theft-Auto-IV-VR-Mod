#pragma once

#include <openvr.h>
#include <cstdint>

namespace asi {

// OpenVR IVRInput (action manifest) layer.
//
// Why: the legacy path (IVRSystem::GetControllerState + raw grip -Z + a 55 deg
// "tip pitch" constant) is deprecated by SteamVR, reports G2 face buttons
// unreliably (see VrPad "Face B rarely reports 0x2000"), and has no real tip
// pose. IVRInput gives /user/hand/*/pose/tip directly and lets SteamVR handle
// per-controller rebinding, so no magic angle is needed.
//
// Config: gtaiv_dxvk_vr.vrinput
//   0 = OFF (default) — legacy GetControllerState path, unchanged behavior
//   1 = ON  — action manifest; legacy reads are bypassed by VrPad/AimDecouple
// Kill: write 0 or delete the file.
//
// Manifest files must sit next to the ASI:
//   gtaiv_dxvk_vr_actions.json
//   gtaiv_dxvk_vr_bindings_hpmotioncontroller.json
// Templates live in config/vr_actions/.
bool IsVrInputEnabled();

// Load the manifest and resolve handles. Safe to call repeatedly; runs once.
// Returns false if disabled, if the manifest is missing, or if SteamVR refused.
bool VrInputInit();

// Once per frame, right after WaitGetPoses. No-op unless active.
void VrInputUpdate();

// True once the manifest loaded AND handles resolved AND a pose was seen.
bool VrInputActive();

// Tip ("aim") pose in raw OpenVR standing space. hand: 0 = right, 1 = left.
bool VrInputGetTipPose(int hand, vr::HmdMatrix34_t* out);

// Grip pose (the raw device pose the legacy path used).
bool VrInputGetGripPose(int hand, vr::HmdMatrix34_t* out);

struct VrInputHand {
  float stickX = 0.f, stickY = 0.f;
  float trigger = 0.f;   // 0..1
  float grip = 0.f;      // 0..1
  bool aim = false;      // grip click / squeeze past threshold
  bool fire = false;     // trigger past threshold
  bool primary = false;  // A / X
  bool secondary = false;// B / Y
  bool menu = false;
  bool stickClick = false;
  bool valid = false;
};

bool VrInputGetHand(int hand, VrInputHand* out);

// Rising edge of the "vr menu" chord (both menu buttons, or the bound action).
// Used by menu_bridge to cycle the render profile from inside the headset.
bool VrInputMenuChordPressed();

}  // namespace asi
