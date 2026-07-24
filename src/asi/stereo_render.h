#pragma once

#include "../../thirdparty/dxvk/d3d9_vk_interop.h"

namespace asi {

bool InstallStereoRenderHooks();

// Create/resize eye RTs when mode >= 4.
void StereoRenderOnDevice(IDirect3DDevice9* device);

// If mode >= 4 and RTs ready, Submit L/R from eye textures. Returns true if handled.
bool StereoTrySubmitEyes(IDirect3DDevice9* device, ID3D9VkInteropDevice* interop);

// True while same-frame L/R dual pass is running (projection inject gate).
bool StereoInDualPass();

// Broader than dual: Left natural PhaseA..Draw + Right dual (mode 7/8).
bool StereoProjWindowActive();

// Mode 24: true only during the RIGHT pass of the exec-view dual; outputs the
// build camera position (left eye) and the world-space L->R eye delta for the
// SetVertexShaderConstantF view-translate patch. Same-thread (render) only.
bool StereoVsGetPatchParams(float cam3[3], float delta3[3]);

}  // namespace asi
