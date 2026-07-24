#pragma once

struct IDirect3DDevice9;

namespace asi {

// AOB-hook CE follow-cam CopyMat; hard-lock pos to player ped + HMD orient.
// Safe to call repeatedly; installs once.
bool InstallCamMatrixHooks();

// Allow Apply only after title/menu (set from OpenVR submit warmup).
void SetCamMatrixGameplayActive(bool active);

bool AreCamMatrixHooksInstalled();

// True when hooks installed AND gameplay gate open.
bool IsCamMatrixOverrideEnabled();

void PollCamHotkeys();

// F9 SteamVR recenter — resets seated 6DoF baseline only.
void CamMatrixOnRecenter();

// Re-apply HMD+IPD to all tracked CopyMat matrices (stereo pass switch).
void RefreshLiveCamForStereoEye();

// Last position written by ApplyHmdToCam (for L/R delta logs).
bool GetLastStereoCamPos(float* x, float* y, float* z);

// World-space delta from LEFT to RIGHT eye (hmdRight * sep * worldScale).
bool GetStereoEyeRightDeltaWorld(float* dx, float* dy, float* dz);

// Push live Rage cam → D3DTS_VIEW (and log once). Safe no-op if no cam/device.
void PushLiveCamToD3D(IDirect3DDevice9* device);

// Row-major 4x4 D3D LH view from live Rage cam. Returns false if no cam.
bool BuildLiveViewMatrix16(float* out16);

// Mode 35: chain-hook FusionFix FOV recompute CALL (CCam+0x60 after cam process).
// Safe to call repeatedly; installs once. Returns true if AOB+hook OK.
bool InstallFovRecomputeSiteHook();

}  // namespace asi

