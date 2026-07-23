#pragma once

#include <cstdint>

namespace asi {

// File: gtaiv_dxvk_vr.stereo — digit / "10"-"13". Kill-switch: 0
enum class StereoMode : int {
  Off = 0,
  Probe = 1,
  DualSameCam = 2,
  DualIpd = 3,
  DualRtSubmit = 4,       // temporal L/R (legacy)
  PhaseProbe = 5,
  SameFrameDual = 6,
  GBufferRtDual = 7,      // same-frame RT dual (original Mode 7 — not exec-dual)
  FusionSwap = 8,
  VtableProbe = 9,
  ExecuteDual = 10,
  BuildExecDual = 11,     // BLACKSCREEN — do not use
  D3dCamDual = 12,
  // L4D2VR/BotW-oriented fusion: temporal dual eye + cover FOV + TextureBounds,
  // IPD/VRScale like L4D2 (defaults: scale=100%, sep≈HMD IPD).
  StereoFusion = 13,
};

StereoMode GetStereoMode();
void ReloadStereoMode();

float GetStereoSepMeters();
void PollIpdScaleHotkey();
float GetStereoIpdScale();

// VRScale (L4D2VR): HIGHER → more IPD + 6DoF in game units → world feels SMALLER.
// File: gtaiv_dxvk_vr.scale (percent, 100 = 1.0)
float GetWorldScale();
void PollWorldScaleHotkey();

// True for Mode 4 and Mode 13 (frame-alternating eye capture + Submit).
bool IsTemporalStereoMode(StereoMode mode);

}  // namespace asi
