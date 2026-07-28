#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace asi {

struct StereoCamBuildReceipt {
  uint32_t applyGeneration = 0u;
  uint32_t applyCount = 0u;
  uint32_t writerThreadId = 0u;
  bool rightEye = false;
  bool eyeOffsetApplied = false;
  bool firstPersonAnchor = false;
  float position[3] = {};
  float preEyePosition[3] = {};
  float eyeOffset[3] = {};
  float localEyeOffset[3] = {};
  // GTA camera basis before the stereo eye offset: right, forward, up.
  float basis[9] = {};
};

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

// Monotonic count of successful ApplyHmdToCam writes.
uint32_t GetStereoCamApplyGeneration();

// Mode 171: snapshot ped/vehicle eye ORIGIN once for a DrawScene L→R pair.
// Mode 178+: also freeze pre-IPD basis+center pos (only ±IPD differs).
void BeginStereoDualCamFreeze();
void EndStereoDualCamFreeze();

// Last position written by ApplyHmdToCam (for L/R delta logs).
bool GetLastStereoCamPos(float* x, float* y, float* z);

// Game-thread BuildRootA receipt. Every successful ApplyHmdToCam on the
// calling thread is accumulated while the scope is active.
void BeginStereoCamBuildReceipt(bool rightEye);
bool EndStereoCamBuildReceipt(StereoCamBuildReceipt* receipt);

// World-space delta from LEFT to RIGHT eye (hmdRight * sep * worldScale).
bool GetStereoEyeRightDeltaWorld(float* dx, float* dy, float* dz);

// Push live Rage cam → D3DTS_VIEW (and log once). Safe no-op if no cam/device.
void PushLiveCamToD3D(IDirect3DDevice9* device);

// Mode193: after a DrawScene dual AV (door/interior), pause GetBonePos briefly.
void NotifyHeadBoneSoftSkip(unsigned ms, const char* why);

// Row-major 4x4 D3D LH view from live Rage cam. Returns false if no cam.
bool BuildLiveViewMatrix16(float* out16);

// Mode 35/36: chain-hook FusionFix FOV recompute CALL (CCam+0x60 after cam process).
// Mode 36 also publishes the post-ADD FOV to the angle-correct canvas.
// Safe to call repeatedly; installs once. Returns true if AOB+hook OK.
bool InstallFovRecomputeSiteHook();

}  // namespace asi

