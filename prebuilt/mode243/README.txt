Mode 243 CHECKPOINT (2026-07-29) — 241 stereo + late-latch Submit
================================================================
Mode203 @0x4D8BF0 + cam-right IPD flip + shared Submit_TextureWithPose
(one late HMD pose stamped on L+R). ASI_BUILD_ID 20260729-210905.

Headset wins vs Mode 241:
- Real fused 3D stereo (same class as 241 checkpoint)
- UI/text noticeably better (likely less motion ghosting; not a HUD overlay fix)
- Head feel improved vs raw 241; some blur remains with SteamVR Motion Smoothing OFF

Kill/restore: copy this folder into GTAIV (ASI + stereo=243 + buildid).
Related: Mode 241 = stereo without late-latch; Mode 242 = 204 seam (snappy, flatter).

Next (blur): stamp dual RENDER pose at Submit instead of late-now sample (Mode 244).
