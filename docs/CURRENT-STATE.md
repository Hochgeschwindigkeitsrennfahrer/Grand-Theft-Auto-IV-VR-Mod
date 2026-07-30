# CURRENT-STATE - gtaiv-dxvk-vr

## 2026-07-31 (home) — STOPPED / pushed to GitHub

Session 6 Plan B paused. Safe defaults: **`ikaim=0`**, all ik* probes **0**, PAGE_GUARD/`ikread` abandoned (crash).  
Candidate recorded: `ikoffs` hint **`D58 90`** (write sticks, **arms MISS** — not IK driver).  
Branch: `feature/motion-controls`.

### Next session
Hunt what actually drives arm aim (not D58+90 / not C20). Keep `ikread=0`.

---
## 2026-07-31 (home) - ikaim D58+90 MISS (write sticks, arms ignore)

**buildId `20260731-003920`** — tested; **MISS**.

### Log verdict
- `ASI_BUILD_ID 20260731-003920` confirmed.
- **IkAim wrote: 85** lines via `CPed+0xD58 -> +0x90`. **0 EXCEPTION**, **0 IK OFF**.
- **IkPin after ikaim on:** d min=0.000 avg˜0.04 max˜1.04 (one spike) — **d near 0 = write STICKS**.
- AimProbe while aimed: **ctrl=1** (67 samples), grip=1 (12) — controller aim active.
- Headset: **nothing moving** — arms ignore stuck vecAim.

### Meaning
- Memory write every-frame **works** and is not out-written (unlike CE freeze).
- `D58?+0x90` is **not** the live arm/IK driver (or wrong semantic: tip origin vs far aim). Treat as **write-not-driving-arms**.

### Disk NOW (SAFE)
- `ikaim=0` (GameDir + home-dropin). **ikoffs kept** (`D58 90`) but disabled via ikaim=0.
- `ikpin` can stay for read-only; no further D58 writes until next approach.

### Next approach
- Stop writing D58+90. Find what **drives** arms: sibling `+0xA0`, other IkRay HEAP/L2 hits, or bone/anim path — LOG-ONLY first.
- User: **Quick Restart** now (GTAIV still running with old ikaim=1 in memory).

---
## 2026-07-31 (home) - Plan B: IkAim write D58?+90 (every frame) — DONE / MISS

**buildId `20260731-003920`** — see verdict above.

### ONE change (that build)
- `ikoffs=D58 90`, `ikaim=1`; TickIkAim write ComputeAimOrigin every frame while aim held.
- ikpin=1 for d= compare.

### Result
- Writes OK + d˜0 stick; arms MISS. Killed: ikaim=0.

---
## 2026-07-31 (home) - CE freeze D58?+90 FAILED (out-written)

**buildId `20260731-002523`**. CE freeze lost race; values change while Active.

### IkPin result (still useful)
- PASS: d ~0.2–1.2 vs ComputeAimOrigin, `src=ctrl`.
- Live sample: `D58->0DDB4840` ? float3 @ **`0DDB48D0`** (+90).

---
## 2026-07-31 (home) — Plan B: IkPin D58/+90 (LOG ONLY)

**buildId `20260731-002523`** (deployed, running)

### Candidate (from `20260731-001945`)
- IkRay HEAP MATCH: CPed+0xD58 ? heapP +0x90 float3 dRef˜0.25, Z˜18.17 ˜ ctrl world origin.
- Sibling +0xA0 similar. L2 via D58+0x20?+0x30.
- ADDRESS_SHEET 4a=`D58` 4b=`90` — **UNVERIFIED freeze** (no ikoffs / no ikaim writes).

### ONE change
- Kill switch `gtaiv_dxvk_vr.ikpin=1` (default off): while aim held, hardened read D58?P, float3 @ +0x90, compare to ComputeAimOrigin, log `IkPin: D58->%p +90=(…) ref=(…) d=…` (~2 Hz). Sibling +0xA0 once. SEH. LOG ONLY.
- `ikray=0` `ikread=0` `ikaim=0`. No PAGE_GUARD.

### Disk
- `ikpin=1` `ikray=0` `ikread=0` `ikaim=0` `aimmode=3`

### Test (LT+VR tip aim ~15s)
1. SteamVR on — already Quick Restarted after deploy.
2. Open `GTAIV\gtaiv_dxvk_vr.log` — confirm `ASI_BUILD_ID 20260731-002523`.
3. Confirm AimProbe `ctrl=1` and **no** `IkRead:` / `IkRay:` / PAGE_GUARD.
4. On foot, hold LT/grip ~15s — expect stable `IkPin:` lines with **small d** (˜0.25 or less).
5. Kill: `gtaiv_dxvk_vr.ikpin=0`, restart.

---
## 2026-07-31 (home) — Plan B: IkRay ref = game-world origin (not far ray)

**buildId `20260731-001945`** (deployed; produced D58/+90 HEAP MATCH).

### Problem (recheck `20260731-001224`)
- aimmode=3 OK, src=ctrl=67, AimProbe ctrl/grip solid.
- 0 MATCH / HEAP / L2.
- IkRay `ref` Z˜42 while AimProbe cam world Z˜18 — ref was **cam + fwd×25** (far hit), not game-world tip near C20/cam.

### ONE change (that build)
- `ComputeAimOrigin`: `cam + (ctrlTrk - hmdTrk)` after OvrToGta (same space as stereo cam / C20).
- `ComputeAimPoint`: origin = `ComputeAimOrigin`, then + fwd × ray (FIRE path).
- IkRay match ref = **world ORIGIN** (`ComputeAimOrigin`), not far ray hit.
- LOG-ONLY nearest each tick (~1–2 Hz): `IkRay: nearest ped d=… at +0x…` and `nearest L1/L2 d=…`.
- `ikread` stays **0**. No PAGE_GUARD. `ikaim=0`.

### Disk
- `ikray=1` `ikread=0` `ikaim=0` `aimmode=3`

---
## 2026-07-31 (home) — Plan B: fix IkRay src=ctrl (pose latch)

**buildId `20260731-001001`** (pose latch fixed; world-origin fixed in build above).

### Problem (build `20260730-235646`)
- L2 scan ran while aiming (`VrPad aimGrip=1 LT=255`) but **40× `src=cam`**, **0× `src=ctrl`**, **0 MATCH**.
- Earlier `20260730-233358` had solid `src=ctrl` + AimProbe `ctrl=1`.

### Root cause
- Disk `aimmode=3` but `GetAimMode()` clamped max **2** ? silent **0**.
- `AimDecoupleOnPoses` early-returned when `aimmode==0` ? **no controller pose latch** ? `ComputeAimPoint` fell back to HMD ? `src=cam`.

### ONE change (that build)
- Always latch OpenVR controller pose in `AimDecoupleOnPoses` (independent of aimmode). Plan A natives still only when `aimmode==2`.
- AimProbe also runs when `ikray=1` (confirm `ctrl=1`).
- `ikread` stays **0**. No PAGE_GUARD. `ikaim=0`.

---
## 2026-07-30 (home) — Plan B: IkRay L2 heap scan (PAGE_GUARD abandoned)

**buildId `20260730-235646`** — L2 empty / cam-only (regression above).

### PAGE_GUARD abandoned
- IkRead PAGE_GUARD crashed 3× on aim (`20260730-234605`); **0 useful EIPs**.
- `ikread` stays **0**. IkRead / PAGE_GUARD **not in this ASI build**.
- `ikaim=0`.

### That build — L2 (log-only)
- Extended `ikray=1`: level-1 hardened CPed ptrs + **level-2** `P+0..+0x100 ? Q` float3 scan.
- Result: L2 empty; all refs `src=cam` (pose latch dead) — fixed later.

---
## 2026-07-30 (home) — IkRead PAGE_GUARD CRASH on aim (killed)

**buildId `20260730-234605`** — IkRead VEH+PAGE_GUARD while aim held.

### Crash
- User: **3 crashes on aim** with IkRead PAGE_GUARD.
- **Useful READ/WRITE EIPs: none**. Crash before any C20 hit logged.

### Kill
- Disk: `gtaiv_dxvk_vr.ikread=0`; `ikaim=0` kept.

---
## 2026-07-30 (home) — Motion: one bullet + G2 tip aim

**buildId `20260730-010155`**

### Headset feedback on `20260730-004820` (3D ComputeAimPoint)
1. **Two bullets** — one from stock gun, one from controller ray.
2. **Controller bullet ignores reload** / continuous fire.
3. **Awkward aim** — pitch OK, but must hunt hits; can shoot behind.
4. **Two freezes** after continuous shooting.
5. Pitch itself works.

### That build — one change (aim-ray correctness)
**A) One projectile** — only `FIRE_PED_WEAPON`; no LMB / FIRE_SINGLE; RT=0 when aimmode=2.
**B) G2 tip** — pitch grip -Z toward +Y by 55°.

Still hard-off: ped bone WRITE, weapfollow CREATE_OBJECT.
