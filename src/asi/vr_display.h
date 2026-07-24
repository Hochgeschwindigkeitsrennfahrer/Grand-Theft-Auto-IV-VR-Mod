#pragma once

#include <openvr.h>

struct IDirect3DDevice9;

namespace asi {

bool InstallVrDisplayHooks();
void LogVrDisplayInfo();
float GetVrHorizontalFovDegrees();

// L4D2 cover-crop (only if we rendered cover FOV — unused for Mode 13).
bool GetEyeSubmitBounds(vr::EVREye eye, vr::VRTextureBounds_t* out);
bool GetCoverFovTangents(float* tanHalfHoriz, float* tanHalfVert);

void UpdateGameFovFromDevice(IDirect3DDevice9* device);

// Mode 13: game BB aspect + optional center-square crop (Luke Ross style) + HMD inset.
// Does NOT render at SteamVR recommended res — Submit uses the game backbuffer as-is.
bool GetNativeFovInsetBounds(vr::EVREye eye, vr::VRTextureBounds_t* out);

// Mode 14: raw per-eye HMD frustum tangents (GetProjectionRaw, cached).
// left/top are negative, right/bottom positive.
bool GetEyeRawProjection(vr::EVREye eye, float* left, float* right, float* top, float* bottom);

// Game half-FOV tangents (horizontal/vertical). Sources, in priority order:
// gtaiv_dxvk_vr.fov (horiz deg 30..150), D3D probe, 70° fallback.
// Canvas zoom (gtaiv_dxvk_vr.zoom) is DISABLED — claimed-FOV warp on head move.
void GetGameFovTangents(float* tanHalfH, float* tanHalfV);

}  // namespace asi
