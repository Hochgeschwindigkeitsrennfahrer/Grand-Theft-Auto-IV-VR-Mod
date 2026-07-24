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

}  // namespace asi
