#pragma once

struct IDirect3DDevice9;

namespace asi {

// AOB-hook CE follow-cam CopyMat; hard-lock pos to player ped + HMD orient.
// Safe to call repeatedly; installs once.
bool InstallCamMatrixHooks();

// Allow Apply only after title/menu (set from OpenVR submit warmup).
void SetCamMatrixGameplayActive(bool active);

bool AreCamMatrixHooksInstalled();

// True when hooks installed AND gameplay gate open AND we own full HMD cam
// (false for Mode 140 — FirstPerson owns cam; HMD→mouse stays on).
bool IsCamMatrixOverrideEnabled();

// Hooks + gameplay armed (Mode 140 dual/submit still need this while override is off).
bool IsStereoRenderArmed();

void PollCamHotkeys();

// F9 view recenter — ped/veh heading baselines (not 6DoF translation).
void CamMatrixOnRecenter();

// F10 — zero seated 6DoF translation origin only (WorldScale lean anchor).
void CamMatrixOnSixDofReset();

// Re-apply HMD+IPD to all tracked CopyMat matrices (stereo pass switch).
void RefreshLiveCamForStereoEye();

// Mode 171: snapshot ped/vehicle eye ORIGIN once for a DrawScene L→R pair.
// Mode 178+: also freeze pre-IPD basis+center pos (only ±IPD differs).
void BeginStereoDualCamFreeze();
void EndStereoDualCamFreeze();

// Mode 234 SAME-STATE (§4.5): latch controller+vehicle yaw and ped eye-center for L/R.
void BeginTrueStereoSameStateLatch();
void EndTrueStereoSameStateLatch();

// Last position written by ApplyHmdToCam (for L/R delta logs).
bool GetLastStereoCamPos(float* x, float* y, float* z);

// World-space delta from LEFT to RIGHT eye (hmdRight * sep * worldScale).
bool GetStereoEyeRightDeltaWorld(float* dx, float* dy, float* dz);

// Push live Rage cam → D3DTS_VIEW (and log once). Safe no-op if no cam/device.
void PushLiveCamToD3D(IDirect3DDevice9* device);

// Mode193: after a DrawScene dual AV (door/interior), pause GetBonePos briefly.
void NotifyHeadBoneSoftSkip(unsigned ms, const char* why);

// Mode216: sample HEAD bone once outside DrawScene/CopyMat (EndScene). Cam bake
// only reads the cache — never calls GetBonePos mid-dual.
void SampleDeferredHeadBoneEye();

// Row-major 4x4 D3D LH view from live Rage cam. Returns false if no cam.
bool BuildLiveViewMatrix16(float* out16);

// Mode 35/36: chain-hook FusionFix FOV recompute CALL (CCam+0x60 after cam process).
// Mode 36 also publishes the post-ADD FOV to the angle-correct canvas.
// Safe to call repeatedly; installs once. Returns true if AOB+hook OK.
bool InstallFovRecomputeSiteHook();

}  // namespace asi

