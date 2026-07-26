#pragma once

struct IDirect3DDevice9;

namespace asi {

// Optional light hooks (VS const only while proj window / dual is active).
void InstallStereoProjHooks(IDirect3DDevice9* device);

bool StereoProjShouldInject();

// Poll device constants / D3DTS_PROJECTION once per eye (no hot path).
void StereoProjApplyForCurrentEye(IDirect3DDevice9* device);

// Mode 24: total view/viewProj constant blocks translated in right passes.
unsigned StereoVsTranslateCount();
// ALL SetVertexShaderConstantF calls seen (hook aliveness).
unsigned StereoVsTotalCalls();
// Calls seen during right passes (constant-flow diagnosis).
unsigned StereoVsRightPassCalls();
// Log all unique uploader return addresses with counts (find the real draw path).
void StereoVsDumpRets();

}  // namespace asi
