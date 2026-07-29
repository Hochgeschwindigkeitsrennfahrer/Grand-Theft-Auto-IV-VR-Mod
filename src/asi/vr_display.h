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
// Mode 153: lock canvas gameTan to HMD cover FOV (under-publish vs wide CCam).
// Returns true when cover tangents were applied.
bool PublishGameFovFromCover();
// Mode 154/155: under-publish with BB aspect preserved — scale true CCam tangents
// so they fit inside cover (fill≈100% on limiting axis). Avoids 153 vertical stretch.
// overscan>1 (Mode 157+): multiply fit scale to shrink bars (mild edge crop).
bool PublishGameFovUnderPublishFit(float ccamDeg, float aspectWH = 0.f,
                                   float overscan = 1.f);
bool IsGameFovFromCCamActive();
// Monotonic counter bumped when gameTan changes from CCam (StereoCanvas re-logs).
uint32_t GetGameFovPublishGeneration();

// Mode 231+: publish half-tangents measured from the live viewProj (StereoCalib).
// Bypasses kEng + under-publish fit. Bars allowed when game < eye cover.
void PublishGameFovMeasured(float tanHalfH, float tanHalfV);

// Last backbuffer aspect (W/H); updated by UpdateGameFovFromDevice / EndScene.
void SetBackbufferAspect(float aspectWH);
float GetBackbufferAspect();

// Mode 13: game BB aspect + optional center-square crop (Luke Ross style) + HMD inset.
// Does NOT render at SteamVR recommended res — Submit uses the game backbuffer as-is.
bool GetNativeFovInsetBounds(vr::EVREye eye, vr::VRTextureBounds_t* out);

// Mode 14: raw per-eye HMD frustum tangents (GetProjectionRaw, cached).
// left/top are negative, right/bottom positive.
bool GetEyeRawProjection(vr::EVREye eye, float* left, float* right, float* top, float* bottom);

// Game half-FOV tangents (horizontal/vertical). Sources, in priority order:
// pair latch (Mode 37), CCam publish (Mode 36/37), gtaiv_dxvk_vr.fov override,
// D3D probe, 70° fallback. Canvas zoom DISABLED — claimed-FOV warp on head move.
void GetGameFovTangents(float* tanHalfH, float* tanHalfV);

// Mode 37: freeze gameTan for one L→R pair so canvas rects match (less temporal jump).
void LatchGameFovForPair();
void ClearGameFovPairLatch();
// True when a Mode 37 pair latch is active (CopySurf uses these, not size calc).
bool GetLatchedGameFovTangents(float* tanHalfH, float* tanHalfV);

// Mode 233 DIRECT (§4.4): float UV bounds for Submit on a 1:1 BB eye texture.
// Returns false → fall back to Mode 232 canvas path (meas under cover / bad bounds).
bool TryGetTrueStereoDirectBounds(vr::EVREye eye, vr::VRTextureBounds_t* out);
// True when Mode 233 should use 1:1 BB copy (not canvas StretchRect).
bool TrueStereoDirectPathOk();

}  // namespace asi
