#pragma once

#include <cstdint>

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

// Mode 36: publish TRUE engine FOV from CCam+0x60 (after fovadd write).
// Rage ignores D3DTS_PROJECTION (Mode 15 dead) — canvas must NOT trust GetTransform.
// ccamDeg = live CCam FOV degrees; aspectWH = backbuffer W/H (0 = last known / 16:9).
// Conversion: Mode 16 probe — CCam 45° → rendered V ≈ 58.7° (factor 58.7/45).
void PublishGameFovFromCCamDegrees(float ccamDeg, float aspectWH = 0.f);
bool IsGameFovFromCCamActive();
// Monotonic counter bumped when gameTan changes from CCam (StereoCanvas re-logs).
uint32_t GetGameFovPublishGeneration();

// Last backbuffer aspect (W/H); updated by UpdateGameFovFromDevice / EndScene.
void SetBackbufferAspect(float aspectWH);
float GetBackbufferAspect();

// Mode 13: game BB aspect + optional center-square crop (Luke Ross style) + HMD inset.
// Does NOT render at SteamVR recommended res — Submit uses the game backbuffer as-is.
bool GetNativeFovInsetBounds(vr::EVREye eye, vr::VRTextureBounds_t* out);

// Mode 14: raw per-eye HMD frustum tangents (GetProjectionRaw, cached).
// left/top are negative, right/bottom positive.
bool GetEyeRawProjection(vr::EVREye eye, float* left, float* right, float* top, float* bottom);

// Build / adapt a D3D-style row-major projection for one HMD eye (asymmetric frustum).
// If template16 is non-null and looks like a projection, keep its Z row (near/far) and
// only replace FOV / off-axis terms from GetProjectionRaw. Otherwise use zn/zf defaults.
bool BuildHmdEyeProjection(vr::EVREye eye, float* out16, const float* template16 = nullptr,
                           float zn = 0.15f, float zf = 2500.f);

// Game half-FOV tangents (horizontal/vertical). Sources, in priority order:
// pair latch (Mode 37), CCam publish (Mode 36/37), gtaiv_dxvk_vr.fov override,
// D3D probe, 70° fallback. Canvas zoom DISABLED — claimed-FOV warp on head move.
void GetGameFovTangents(float* tanHalfH, float* tanHalfV);

// Mode 37: freeze gameTan for one L→R pair so canvas rects match (less temporal jump).
void LatchGameFovForPair();
void ClearGameFovPairLatch();
// True when a Mode 37 pair latch is active (CopySurf uses these, not size calc).
bool GetLatchedGameFovTangents(float* tanHalfH, float* tanHalfV);

}  // namespace asi
