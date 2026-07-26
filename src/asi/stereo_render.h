#pragma once

#include "../../thirdparty/dxvk/d3d9_vk_interop.h"

namespace asi {

bool InstallStereoRenderHooks();

// Create/resize eye RTs when mode >= 4.
void StereoRenderOnDevice(IDirect3DDevice9* device);

// Graphics options / resolution change: release POOL_DEFAULT eye RTs before Reset.
void StereoNotifyDeviceLost();

// If mode >= 4 and RTs ready, Submit L/R from eye textures. Returns true if handled.
bool StereoTrySubmitEyes(IDirect3DDevice9* device, ID3D9VkInteropDevice* interop);

// True while same-frame L/R dual pass is running (projection inject gate).
bool StereoInDualPass();

// Mode74: true only when sparse dual is runtime-armed (gate OPEN; not HARD OFF).
bool StereoMode74DualEnabled();

// Mode74: upload fn 0x2A1E10 bak/±IPD/restore armed (owns draw-path sep; CamMatrix skips IPD).
bool StereoMode74UploadFnArmed();

// Broader than dual: Left natural PhaseA..Draw + Right dual (mode 7/8).
bool StereoProjWindowActive();

// Mode 24: true only during the RIGHT pass of the exec-view dual; outputs the
// build camera position (left eye) and the world-space L->R eye delta for the
// SetVertexShaderConstantF view-translate patch. Same-thread (render) only.
bool StereoVsGetPatchParams(float cam3[3], float delta3[3]);

// Mode 31: SetVSConstF feeds stack RVAs while Discover is armed (stereo_proj).
bool StereoMode31WantsDiscover();
void StereoMode31OnVsConst(void* retAddr);

// Mode 32/33: VsParent hist must be collected at HookSetVSConstF stack depth
// (same as Mode 26 NoteVsRet) — pass SP from the hook, do not scan from a callee.
bool StereoMode32WantsDiscover();
void StereoMode32CollectVsParents(void* retAddr, void* hookSpWords);
bool StereoMode33WantsDiscover();
void StereoMode33CollectVsParents(void* retAddr, void* hookSpWords);
bool StereoMode34WantsDiscover();
void StereoMode34CollectVsRetCallers(void* retAddr, void* hookSpWords);
// Mode 41: read-only replay-thread chain map. No game-function hooks.
bool StereoMode41WantsTrace();
void StereoMode41CollectVsRetChain(void* retAddr, void* hookSpWords);

}  // namespace asi
