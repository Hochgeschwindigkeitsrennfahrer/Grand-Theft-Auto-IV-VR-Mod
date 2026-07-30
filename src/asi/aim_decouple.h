#pragma once

#include <openvr.h>

namespace asi {

// Decoupled aiming (docs/DECOUPLED_AIM_PLAN.md). OFF by default.
// gtaiv_dxvk_vr.aimmode:
//   0 = off (default)
//   1 = probe log + one-time AimNative resolve
//   2 = grip LT + trigger FIRE_PED_WEAPON (on foot); TASK_AIM off (no VR hands)
//   3 = Session 3 controller-origin ray + once-per-frame natives
// Kill: write 0 or delete file. Stereo baseline: Mode 243 (do not change here).
int GetAimMode();

// 0 = right hand (default), 1 = left (gtaiv_dxvk_vr.aimhand).
int GetAimHand();

// Cache controller pose + grip from the SAME WaitGetPoses array as the HMD.
void AimDecoupleOnPoses(const vr::TrackedDevicePose_t* poses, uint32_t poseCount);

// EndScene-safe: probe log only (no native calls).
void UpdateAimDecouple();

// Game-thread only (CopyMat / PedHide sites): Plan A native aim when aimmode=2.
void TickAimFromGameThread();

bool GetControllerAimPose(float* posXyz, float* fwdXyz, float* upXyz);
bool ResolveWeaponOrHandMatrix(float* outMat16RowMajor);
bool ComputeAimForward(float* fwdXyz);
// Game-world ray origin: cam+(ctrlTrk-hmdTrk) when controller latched, else cam.
// Same space as GetLastStereoCamPos / C20 — NOT raw OpenVR tracking meters.
// *fromCtrl=1 when controller world estimate was used.
bool ComputeAimOrigin(float* ox, float* oy, float* oz, bool* fromCtrl = nullptr);
// World aim point = ComputeAimOrigin + fwd * ray (same as FIRE). Optional:
// *fromCtrl=1 when controller pose supplied origin and/or forward.
bool ComputeAimPoint(float* ax, float* ay, float* az, bool* fromCtrl = nullptr);
bool ApplyAimOverride();

}  // namespace asi