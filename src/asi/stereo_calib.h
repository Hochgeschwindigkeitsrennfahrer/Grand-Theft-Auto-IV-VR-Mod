#pragma once

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace asi {

// Mode 230 (TrueStereoCalibProbe) — READ-ONLY measurement of the projection Rage
// actually renders with. Mode 231 Exact also measures and PUBLISHES those tangents
// as gameTan (docs/STEREO_TRUETICK_REVIEW.md §4.1–4.2). 203-seam retry.
//
// Every entry point is a no-op unless Mode 230/231 is active. Nothing here hooks a
// game function or duplicates a draw.

// Cheap gate for the hot SetVertexShaderConstantF path (one atomic load).
bool StereoCalibWantsVsProbe();

// True after Mode 231 has published at least one measured gameTan (else kEng fallback).
bool StereoCalibHasMeasurement();

// Called from HookSetVSConstF with the untouched upload. Read-only of the upload;
// Mode 231 may publish gameTan from the measurement.
void StereoCalibOnVsConst(unsigned startRegister, const float* data, unsigned vec4Count);

// Called once per parent-dual EndScene: throttle + arm the next sample.
void StereoCalibOnEndScene();

// Optional (gtaiv_dxvk_vr.caliblum = 1, default off): mean luminance of both eye
// textures, to quantify the auto-exposure rivalry between the L and R pass.
// Costs a GPU readback — throttled, probe/exact modes only.
void StereoCalibSampleEyeLuma(IDirect3DDevice9* device, IDirect3DTexture9* texLeft,
                              IDirect3DTexture9* texRight);

}  // namespace asi
