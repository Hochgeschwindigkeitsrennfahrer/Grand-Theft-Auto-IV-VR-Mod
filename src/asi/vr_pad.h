#pragma once

#include <openvr.h>
#include <cstdint>

namespace asi {

// OpenVR → synthetic Xbox pad. Config: gtaiv_dxvk_vr.vrpad 1/0 (default 1).
// Official G2 / SteamVR hpmotioncontroller (see SteamVR resources + OpenXR HP profile):
//   /input/joystick (Axis Joystick, NOT trackpad Axis0)
//   /input/trigger  /input/squeeze(grip)  /input/a /b /x /y  /input/menu
//   Windows/System button is NOT menu — leave unmapped (SteamVR overlay).
// Game mapping:
//   L stick=move  R stick=look
//   R grip=aim (Xbox LT + TASK_AIM)  R trigger=shoot (Xbox RT)
//   Menu=Start  A/B/X/Y=face  L grip=LB cover
bool IsVrPadEnabled();
void UpdateVrPadFromPoses(const vr::TrackedDevicePose_t* poses, uint32_t poseCount);
bool InstallVrPadHooks();
bool IsVrPadAimHeld();  // right grip
bool IsVrPadShootHeld();  // right trigger
float GetVrPadRightStickX();

}  // namespace asi
