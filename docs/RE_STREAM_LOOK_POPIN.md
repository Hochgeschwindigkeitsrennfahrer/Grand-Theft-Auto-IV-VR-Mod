# RE notes — streaming / cull vs HMD look (2026-07-27 overnight)

Goal: explain texture **pop-in on windows** in Roman’s apartment, worst when **looking with HMD**.

## What we already know (clean Mode 120/121)

- 3D path = **DrawScene×2** everyN + HOLD (not BuildRootA×2 — BuildRoot dual was worse for stream).
- Phase order: **PhaseA → PhaseC → DrawScene** (game thread).
- `BuildRootA @ 0x8F7F00` walks ~13 render phases; dualing that whole root is forbidden on clean path.
- WaitGetPoses stalls **20–200ms** observed → AppFPS half-lock / hitch (not RAM leak).
- FusionFix FOV not primary; higher **fovadd** = more StretchRect crop zoom-IN.

## Ranked next RE targets (not hooked tonight)

1. **Streamer uses gameplay / TheCamera position, not HMD render cam**  
   Classic GTA streamers request models from **camera/player position** (see GTAMods Resource Streaming; VC `AddModelsToRequestList(TheCamera.GetPosition())`).  
   If IV still keys off collision/gameplay cam while we only override **render** CopyMat to HMD, turning the head looks at sectors the streamer has not prioritized → window TXD pop-in.  
   **Future:** find CE streamer focus write; optionally bias request list toward HMD forward (read-only probe first).

2. **DrawScene dual tearing stream budget** — L+R same tick while yaw changes. **Mode 122 LOOKHOLD tests this.**

3. **WaitGetPoses stall (≥16–200ms)** — compositor hitch. Offline Mode121: **3248** stall samples, avg **21.2ms**, max **212ms**. **Mode 122 POSEHOLD.**

4. **Occlusion / portal / room system around interiors** — dual frustum thrash.

5. **Mip / texture budget under dual StretchRect**.

6. **BuildRootA×2** — **forbidden** on clean path.

## Safe probes later (COUNT/read-only first)

- Log DrawScene gap vs HMD yaw rate (already partially in perf.csv).
- Optional: read-only peek of streamer “focus” position if AOB found (do not write).
- Never enable BuildRootA×2 on clean LKG path.

## UEVR parallel (reference only)

Praydog: per-eye **view + GetProjectionRaw projection** same tick (Native Stereo / Synchronized Sequential).  
UEVR also has orientation-LOD bugs in some UE titles — symptom class similar, engine different.
