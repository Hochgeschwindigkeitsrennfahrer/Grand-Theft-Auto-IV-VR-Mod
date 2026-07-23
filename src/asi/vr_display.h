#pragma once

#include <openvr.h>

namespace asi {

// FOV call-site hook — currently pass-through only (HMD FOV write froze CE).
bool InstallVrDisplayHooks();

// Log recommended RT size / FOV / IPD once (safe anytime after VR_Init).
void LogVrDisplayInfo();

// Horizontal FOV in degrees from HMD projection (0 if unavailable).
float GetVrHorizontalFovDegrees();

// L4D2VR-style submit bounds: shared cover FOV render → per-eye UV crop.
// Returns false if OpenVR unavailable (caller should pass nullptr bounds).
bool GetEyeSubmitBounds(vr::EVREye eye, vr::VRTextureBounds_t* out);

// Symmetric cover frustum tangents (max of both eyes) — for game projection widen.
bool GetCoverFovTangents(float* tanHalfHoriz, float* tanHalfVert);

}  // namespace asi
