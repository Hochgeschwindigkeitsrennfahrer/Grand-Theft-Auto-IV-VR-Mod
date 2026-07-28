#pragma once

#include "../bridge/gtaiv_xr_pose_bridge.h"
#include "../../thirdparty/dxvk/d3d9_vk_interop.h"

#include <cstdint>

namespace asi {

struct OpenXrStereoPair {
  IDirect3DTexture9* eyes[2] = {nullptr, nullptr};
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t contentWidth = 0;
  uint32_t contentHeight = 0;
  D3DFORMAT format = D3DFMT_UNKNOWN;
  uint64_t pairId = 0;
  uint64_t sourceFrameId[2] = {};
  uint64_t poseSequence[2] = {};
  int64_t renderedDisplayTime[2] = {};
  bool sameSimulationTick = false;
  bool poseStamped = false;
  bool verifiedWvpStereo = false;
  bool verifiedDrawSceneStereo = false;
  // True only for the explicit direct-OpenXR temporal mode: the two
  // non-aliased images came from an ordered L->R pair of complete native GTA
  // frames. This is deliberately separate from every same-tick proof bit.
  bool verifiedTemporalStereo = false;
  bool uiQuad = false;
  bool immersiveMono = false;
};

bool InstallStereoRenderHooks();

// Create/resize eye RTs when mode >= 4.
void StereoRenderOnDevice(IDirect3DDevice9* device);
// Mode 57: bind the ExecRoot receipt to the exact successful D3D scene.
// Called only by the already-installed IDirect3DDevice9::BeginScene hook.
void StereoMode57BeginScene(IDirect3DDevice9* device);
// Release every D3DPOOL_DEFAULT capture resource before/after device Reset.
void StereoNotifyDeviceLost();
// Pose/FOV that GTA used to render the frame currently at EndScene.
// Null invalidates capture after host loss and before the first owned frame.
void StereoSetOpenXrRenderPose(
    const gtaiv_xr_bridge::PoseBridge* pose);

// If mode >= 4 and RTs ready, Submit L/R from eye textures. Returns true if handled.
bool StereoTrySubmitEyes(IDirect3DDevice9* device, ID3D9VkInteropDevice* interop);

// Acquires AddRef'd L/R textures from the most recently completed capture
// transaction. The caller must release with StereoReleaseOpenXrPair.
bool StereoAcquireOpenXrPair(OpenXrStereoPair* output);
void StereoReleaseOpenXrPair(OpenXrStereoPair* pair);

// True while same-frame L/R dual pass is running (projection inject gate).
bool StereoInDualPass();

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
