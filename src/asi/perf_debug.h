#pragma once

#include <cstdint>

namespace asi {

// Lightweight sampled diagnostics (Mode 120 stutter / RAM probe).
// Toggle: gtaiv_dxvk_vr.debug next to EXE — "1" or absent = ON; "0" = OFF.
// Files (sampled ~1–2s, NOT per-frame):
//   gtaiv_dxvk_vr.perf.csv
//   gtaiv_dxvk_vr.mem.log
//   gtaiv_dxvk_vr.vr.log  (rare compositor/pose errors only)
// No FPS HUD / ColorFill.

void PerfDebugInit();
bool PerfDebugEnabled();

// Call once per WaitGetPoses.
void PerfDebugOnPoseWait(double poseMs, int poseErr);

// Call once per successful TryMonoSubmit frame (after Submit).
// hold=1 means Mode120 used last L/R (off-tick / hitch-gap); dual=1 means DrawScene×2 ran.
void PerfDebugOnPresent(bool stereoSubmitted, bool dualTick, bool holdTick, uint32_t drawGapMs);

// Rare VR/compositor notes → .vr.log only.
void PerfDebugVrNote(const char* fmt, ...);

// Mode120 DrawScene path: record dual vs HOLD vs hitch-skip (cheap atomics).
void PerfDebugNoteDrawScene(bool didDual, bool hold, uint32_t gapMs);

}  // namespace asi
