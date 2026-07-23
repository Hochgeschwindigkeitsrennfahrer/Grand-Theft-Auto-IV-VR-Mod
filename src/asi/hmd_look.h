#pragma once

#include <cstdint>

namespace vr {
struct TrackedDevicePose_t;
}

namespace asi {

void ApplyHmdMouseLook(const vr::TrackedDevicePose_t* poses, uint32_t poseCount);

// SteamVR seated zero + mouse-look baseline. Safe to call every frame (edge-triggered F9).
void PollRecenterHotkey();

}  // namespace asi
