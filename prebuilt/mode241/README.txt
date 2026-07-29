Mode 241 CHECKPOINT (2026-07-29) — 100% 3D stereo VR
=====================================================
Mode203 parent dual @0x4D8BF0 + pose latch + cam-right IPD with L/R flip
(no TrueStereo measure/Direct/publish — no gameTan FOV spazz).

Headset: real fused stereo (near + far). Stick yaw pivots correctly.

Known issues (next track — do not lose this checkpoint):
- Head look feels heavy / ghosting — dual cost + pose latch vs head motion
- UI / text not fused (screen-space HUD still mono-wrong parallax)

Kill/restore: copy this ASI + stereo=241 into GTAIV folder.
Related: Mode 242 = same IPD flip on Mode204 seam (@0x4DE020).
