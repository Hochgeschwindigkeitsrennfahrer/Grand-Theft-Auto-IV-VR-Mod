#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace asi {

struct StereoDrawPatchAudit {
  bool verified = false;
  uint32_t leftDraws = 0;
  uint32_t rightDraws = 0;
  uint32_t eligibleDraws = 0;
  uint32_t patchedDraws = 0;
  uint32_t factorOnlyDraws = 0;
  uint32_t auxiliaryWvpDraws = 0;
  uint32_t passthroughDraws = 0;
  uint32_t trustedCameraDraws = 0;
  uint32_t failures = 0;
  uint64_t leftSequenceHash = 0;
  uint64_t rightSequenceHash = 0;
};

inline bool IsVerifiedStereoDrawPatchAudit(
    const StereoDrawPatchAudit& audit) {
  return audit.leftDraws != 0u &&
         audit.leftDraws == audit.rightDraws &&
         audit.leftSequenceHash == audit.rightSequenceHash &&
         audit.eligibleDraws != 0u &&
         audit.eligibleDraws == audit.patchedDraws &&
         audit.trustedCameraDraws != 0u &&
         audit.failures == 0u;
}

// Mode 54 only. Hooks the six D3D9 draw entry points on the real GTA device.
// Hooks remain inert outside an explicitly bracketed pair and render thread.
bool InstallStereoDrawPatchHooks(IDirect3DDevice9* device);
bool StereoDrawPatchHooksReady();

bool BeginStereoDrawPatchPair(
    IDirect3DDevice9* device,
    const float rightEyeDeltaWorld[3]);
bool BeginStereoDrawPatchRightPass();
StereoDrawPatchAudit EndStereoDrawPatchPair();
void CancelStereoDrawPatchPair();

}  // namespace asi
