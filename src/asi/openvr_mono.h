#pragma once

struct IDirect3DDevice9;

namespace asi {

void TryMonoSubmit(IDirect3DDevice9* device);
void OpenVrShutdown();

// Call from ASI worker thread (not D3D thread).
bool InitOpenVrEarly();

// True once VR is initialised and past warmup — Mode 271 submits from the end
// of the draw walk instead of from EndScene, so it needs to know when that is
// safe.
bool VrSubmitReady();

// Mode 271: the draw walk drives the VR frame, not EndScene.
// Canonical OpenVR order is WaitGetPoses -> render -> Submit. EndScene fires
// INSIDE origDrawWalk, so driving the frame from there can never achieve that.
// Called at the START of the AER walk instead, this gives WaitGetPoses ->
// render the eye with that fresh pose -> Submit at the walk's end.
bool VrBeginFrameFromDual();
// EndScenes since the walk last drove a VR frame. TryMonoSubmit falls back to
// the legacy EndScene path when this grows, so menus/loading never starve the
// compositor.
unsigned VrEndScenesSinceDualFrame();

}  // namespace asi
