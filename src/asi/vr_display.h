#pragma once

#include <cstdint>

#include <openvr.h>

struct IDirect3DDevice9;

namespace gtaiv_xr_bridge {
struct PoseBridge;
}

namespace asi {

struct GameFovPublishReceipt {
  uint32_t generation = 0u;
  float tanHalfHorizontal = 0.f;
  float tanHalfVertical = 0.f;
  bool trueFov = false;
};

bool InstallVrDisplayHooks();
void LogVrDisplayInfo();
float GetVrHorizontalFovDegrees();

// L4D2 cover-crop (only if we rendered cover FOV — unused for Mode 13).
bool GetEyeSubmitBounds(vr::EVREye eye, vr::VRTextureBounds_t* out);
bool GetCoverFovTangents(float* tanHalfHoriz, float* tanHalfVert);

// OpenXR input adapter. It fills a backend-isolated display-frustum cache;
// provider getters fail closed until this cache is ready.
bool UpdateVrDisplayFromOpenXr(
    const gtaiv_xr_bridge::PoseBridge& pose);
void InvalidateVrDisplayFromOpenXr();
bool IsVrDisplayFromOpenXr();

void UpdateGameFovFromDevice(IDirect3DDevice9* device);

// Mode 36: publish TRUE engine FOV from CCam+0x60 (after fovadd write).
// Rage ignores D3DTS_PROJECTION (Mode 15 dead) — canvas must NOT trust GetTransform.
// ccamDeg = live CCam FOV degrees; aspectWH = backbuffer W/H (0 = last known / 16:9).
// Conversion: Mode 16 probe — CCam 45° → rendered V ≈ 58.7° (factor 58.7/45).
void PublishGameFovFromCCamDegrees(float ccamDeg, float aspectWH = 0.f);
// Proven Mode16 CCam->raster conversion (45 CCam degrees -> 58.7 vertical
// raster degrees). Pure/query-free; aspectWH must be the captured backbuffer.
bool ComputeGameFovTangentsFromCCamDegrees(
    float ccamDeg,
    float aspectWH,
    float* tanHalfHorizontal,
    float* tanHalfVertical);
// Query-free tangent commit. The caller already owns validated display
// geometry (direct OpenXR uses PoseBridge::eyeFovs); this function never
// consults OpenVR or any runtime API.
bool PublishGameFovTangents(
    float tanHalfHorizontal,
    float tanHalfVertical,
    GameFovPublishReceipt* receipt = nullptr);
// Fail-closed validation for a receipt used to authorize a captured pair.
bool IsGameFovPublishReceiptCurrent(
    const GameFovPublishReceipt& receipt);
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

}  // namespace asi
