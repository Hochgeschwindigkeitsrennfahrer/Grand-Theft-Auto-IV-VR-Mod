#pragma once

namespace asi {

// Head-directed locomotion + right-stick camera yaw. Call each VR frame after poses.
void UpdateVrMoveAndStick();

// Extra yaw (radians) from controller — applied on top of HMD in cam matrix path.
float GetControllerYawOffset();
void ResetControllerYawOffset();

}  // namespace asi
