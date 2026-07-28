#pragma once

#include <cstdint>

namespace asi {

// Mode 120–136 clean dual: synced DrawScene×2 + HOLD/hitchcut (Praydog sequential).
// Never BuildRootA×2. Never AER temporal as the 3D path. No FPS HUD.
// Mode 120: dualn default 2 (file). Mode 121: force 3. Mode 122: force 4 + look/pose HOLD.
// Mode 126: dualn 2 + pose HOLD + live BB both eyes (not stale HOLD).
// Mode 127: same LIVELOOK as 126 + tighter POSEHOLD + dualn=3 + hitch 32.
// Mode 128: Mode127 fill/tight POSEHOLD/dualn=3 — NO look-rate LIVELOOK; pose → HOLD last stereo.
// Mode 129: look → force same-tick dual; calm dualn=3 HOLD; pose → mono BB ≥200ms hyst.
// Mode 130: look → identical BB both eyes ≥350ms hyst; calm dualn=3 HOLD; pose extends mono.
// Mode 131: Mode130 monolook + pitch-stable + 600ms + BB every 3 + calm POSEHOLD — FAILED (HMD starve).
// Mode 132: Mode131 pitch-stable monolook but BB→L+R EVERY EndScene; calm dualn=3 HOLD.
// Mode 133: FAILED — same-tex L+R Submit (extreme jump / fusion gone). Prefer 134.
// Mode 134: exact Mode 132 look path (BB→L AND BB→R every; 132 pitch/hyst). FAILED HOLD freeze.
// Mode 135: ALWAYS-FRESH — everyN=1 dual; look→BB×2; POSEHOLD→fresh monolook; never bare HOLD.

// True after a dual or HOLD tick this frame — EndScene must NOT TemporalCapture.
bool StereoDualHoldActive();
void StereoDualClearHold();
void StereoDualMarkHold();

// Mode 126/127: skip dual → EndScene copies live BB to both eyes (fresh mono, no snap).
// Mode 129: pose-budget mono with ≥200ms hysteresis (same EndScene BB→both path).
// Mode 130: look/pose mono with ≥350ms hysteresis (same EndScene BB→both path).
// Mode 131: look mono with ≥600ms hyst (EndScene throttles BB refresh — FAILED).
// Mode 132/134: same look mono ≥600ms hyst; EndScene StretchRect BB→L+R every mono frame.
// Mode 133: FAILED — EndScene ONE StretchRect BB→L; Submit L for both.
// Mode 135: look (low thresh) OR pose → EndScene BB→L+R every; never bare HOLD.
bool StereoDualLiveLookActive();
void StereoDualClearLiveLook();
void StereoDualMarkLiveLook();

// Soft hitch gap (ms) — skip dual, keep last L/R. Mode 122/126: 40ms. Mode 127–135: 32ms.
// Mode 168: 40ms hitch → live BB (not stale HOLD).
uint32_t StereoDualHitchMs();

// Dual every N DrawScene calls. Mode 120: file gtaiv_dxvk_vr.dualn (2..4), default 2.
// Mode 168: forced 1 (flicker-stable).
// Mode 121/127–134: forced 3. Mode 122: forced 4. Mode 126: file/default 2.
// Mode 135: forced 1 (always same-tick dual; no off-tick HOLD).
uint32_t StereoDualEveryN();

// Streaming gate EndScenes before first dual.
uint32_t StereoDualGateNeed();

// Mode 122/125: true → skip dual this tick (look-rate or pose-budget stale HOLD).
bool StereoDualShouldLookHold();

// Mode 126/127: true → skip dual; EndScene should live-BB both eyes (not stale HOLD).
bool StereoDualShouldLiveLook();

// Mode 128: true → skip dual; HOLD last stereo (pose budget only; no LIVELOOK mono).
// Mode 131/132/133/134 calm: same (pose stall → HOLD last stereo; look path uses mono instead).
// Mode 135: NEVER (POSEHOLD → AlwaysFreshMono instead).
bool StereoDualShouldPoseHoldStereo();

// Mode 129: true → skip dual; EndScene mono BB both eyes (pose budget + ≥200ms hyst).
bool StereoDualShouldPoseMonoHyst();

// Mode 129: true → force same-tick DrawScene×2 this frame (look rate; skip everyN HOLD).
bool StereoDualShouldForceLookDual();

// Mode 130: true → skip dual; EndScene identical BB→both eyes (look and/or pose + ≥350ms hyst).
bool StereoDualShouldMonoLookHyst();

// Mode 131/132/133/134: true → skip dual; EndScene mono BB (look sustained + ≥600ms hyst).
// Does NOT enter mono on pose alone (calm pose → PoseHoldStereo). Pitch thresh > yaw.
// Mode 131 EndScene throttles BB/3; Mode 132/134 BB→L+R every; Mode 133 BB→L + Submit L both (FAILED).
bool StereoDualShouldMonoLookPitchHyst();

// Mode 135: look (low thresh) OR pose budget → fresh BB→both; never bare HOLD.
bool StereoDualShouldAlwaysFreshMono();

// Mode 122/125/126/127/128/129/130/131/132/133/134/135: feed last WaitGetPoses duration (ms).
void StereoDualNotePoseMs(double poseMs);

}  // namespace asi
