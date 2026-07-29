# Camera ↔ Character Attach Review (offline, 2026-07-29)

Written on the work PC — **no headset / no game files**. Nothing here is headset-verified.
Read together with `docs/CURRENT-STATE.md` (Mode 216 symptoms) and
`docs/HANDOFF_HOME_AIM_CAMERA.md` (session plan).

Symptoms (user, headset, Mode 216): camera AND world shake while walking; character
model jumps / fades / spazzes; stairs + crouch OK (stick works); vehicles want the
eye between the eyes (no back offset).

---

## 1. How the attach works today (exact map)

### 1.1 Hook layer

| What | Where |
|---|---|
| 4 CopyMat call-site hooks (onfoot front/behind, vehicle front/behind) | `src/asi/cam_matrix.cpp:1435-1438` (`InstallCamMatrixHooks`) |
| Every CopyMat → `ApplyHmdToCam(mat)` | `src/asi/cam_matrix.cpp:1230-1249` (`HookCopyMat`) |
| Late re-apply after Rage cam post-process (FusionFix FOV CALL chain) | `src/asi/cam_matrix.cpp:1744-1803` (`HookCamFovSite` → `RefreshLiveCamForStereoEye`) |
| Dual-eye render pair (Mode 200/201/203/204/216 family) | `src/asi/stereo_render.cpp:2265-2335` (`HookDrawWalk`) → `RunMode200ParentDualGuarded` `src/asi/stereo_render.cpp:2562-2599` |
| `RefreshLiveCamForStereoEye` re-applies HMD cam to ALL tracked mats per eye | `src/asi/cam_matrix.cpp:1497-1521` |

### 1.2 Eye position source (Mode 216 path)

1. **Bone sample** — once per frame, from `HookDrawWalk` *before* the dual flag:
   `src/asi/stereo_render.cpp:2298-2299` → `SampleDeferredHeadBoneEye`
   (`src/asi/cam_matrix.cpp:1278-1409`).
2. Bone getter: `TryGetPedHeadBoneEye` (`cam_matrix.cpp:430-544`) via CE thiscall
   `ResolvePedGetBonePos` (`cam_matrix.cpp:371-404`, AOB + RVA 0x5E7320 fallback,
   bone tag HEAD = `0x4B5` at `cam_matrix.cpp:357`).
3. **Smoothing** (all inside `SampleDeferredHeadBoneEye`):
   - bone-minus-root offset, adaptive alpha `kOffBobA=0.025 / kOffMoveA=0.80`,
     band `kOffBobT=0.08m / kOffMoveT=0.22m` — `cam_matrix.cpp:1318-1337`
   - **root XYZ smoothing** with `kRootBobA=0.03 / kRootMoveA=0.92`,
     band `0.08m / 0.20m` — `cam_matrix.cpp:1340-1359`
   - compose eye = smoothedRoot + smoothedOffset + 6cm up + 3cm fwd —
     `cam_matrix.cpp:1361-1371`
   - one PedHide pulse per sample — `cam_matrix.cpp:1379`
4. **Cam bake**: `ApplyHmdToCam` (`cam_matrix.cpp:718-1096`) reads only the cached
   `g_deferredWorldEye` (`TryGetDeferredHeadBoneEyeCached`, `cam_matrix.cpp:621-641`
   via `TryGetPedEyeCenterBase` `cam_matrix.cpp:657-716`), adds HMD orientation
   (forward from pose, right/up rebuilt against world-up), controller/vehicle yaw,
   ±IPD (`cam_matrix.cpp:988-1074`), then **overwrites the CCam matrix rows**
   (`cam_matrix.cpp:1079-1082`).
5. Fallback when bone missing: ped root + 0.65m height (`cam_matrix.cpp:670-686`) —
   **live per call**, not frozen for the L/R pair.

### 1.3 Data flow per frame (Mode 216)

```
EndScene (openvr_mono.cpp:128 TryMonoSubmit)
  └ WaitGetPoses → UpdateHmdPose (hmd_pose.cpp:23)
Game thread draw:
  HookDrawWalk (stereo_render.cpp:2265)
    ├ SampleDeferredHeadBoneEye          ← 1 bone read + smoothing + PedHide pulse
    ├ [optional] BeginDualHmdPoseLatch   ← one HMD pose for the pair (Mode 203+)
    ├ L: SetStereoEye(L) → RefreshLiveCamForStereoEye → ApplyHmdToCam → DrawScene → canvas L
    ├ R: SetStereoEye(R) → same → canvas R
    └ back to L
  HookCamFovSite (cam_matrix.cpp:1652) → late RefreshLiveCamForStereoEye (again!)
EndScene → StereoTrySubmitEyes (stereo_render.cpp:~7700) → OpenVR Submit L/R
```

### 1.4 Ped visibility (PedHide)

`UpdatePedHeadHide` (`src/asi/ped_hide.cpp:92-151`):
- throttle `(n % 30) != 0 → return` (`ped_hide.cpp:106`), n = **call count**, not frames
- hides components 0,7,8,9,10 **every pulse**, even when already hidden
  (`ped_hide.cpp:125-129` native path / `140-144` helper path)
- non-216 modes still call it from every `ApplyHmdToCam` (`cam_matrix.cpp:935`)

### 1.5 Body heading writers (potential fighters)

- `vr_move.cpp:121-144 ApplyPedHeading` — hard-rewrites the **whole ped matrix**
  (right/up/at rows) + heading floats each frame when active.
- `look_move.cpp:114-117 ApplyPedHeadingSoft` — heading floats only (Mode 147+).
- Both are skipped in free-move / look-move-owning modes
  (`vr_move.cpp:196-203`), but the hard path still exists for older modes.

---

## 2. Diagnosis — why walk still shakes and the model spazzes

### 2.1 PRIMARY SUSPECT: root-XY smoothing creates a lag→lurch limit cycle

`cam_matrix.cpp:1340-1359`. Walking moves the ped root ~2.5 m/s ≈ **3-4 cm per
frame** at 60-90 fps. That per-sample delta sits INSIDE the "bob" band
(`kRootBobT = 0.08m`), so root XY is smoothed with alpha **0.03** — the camera
anchor follows only ~3% of real forward motion per frame. The error accumulates
until it crosses toward `kRootMoveT = 0.20m`, alpha ramps to 0.92, the camera
**lurches forward**, the error collapses back into the bob band, the anchor stalls
again. Result: rhythmic world jump exactly while walking — matching "stairs stick:
yes / walk: everything jumping" in `CURRENT-STATE.md`.

Key design error: the adaptive band treats *translation speed* as *bob amplitude*.
Bob is an oscillation in **bone-minus-root**; root translation while walking is
real player motion and must be followed 1:1. Smoothing the absolute root position
by error magnitude cannot distinguish "walking steadily" from "standing with sway".

### 2.2 Bone offset filtered in WORLD space — turning corrupts the filter

`g_deferredSmoothOff` is bone-minus-root in **world** coordinates
(`cam_matrix.cpp:1313-1337`). When the ped turns 180°, the true local offset
(head is slightly forward of root) flips sign in world space; the low-alpha filter
takes dozens of frames to converge → the eye drifts sideways during/after turns
and then snaps. Filtering must happen in **ped-local** space (rotate by inverse
heading before filtering, rotate back after).

### 2.3 Model spazz: SET_DRAW re-applied periodically + mid-frame

- The hide pulse repeats every ~30 calls forever (`ped_hide.cpp:106,125-129`).
  If the game re-evaluates component draw per frame, each pulse can land at a
  different point in the frame (CopyMat on non-216 / sample time on 216); on the
  dual path a state flip between the L pass and the R pass shows the head in one
  eye only → perceived flicker/spazz/fade.
- Non-216 modes call `UpdatePedHeadHide()` from **every** `ApplyHmdToCam`
  (`cam_matrix.cpp:935`) — with 4 tracked mats × 2 eyes × late refresh this is
  ~10+ calls/frame; the `%30` throttle then fires roughly every 3 frames.
- Additional fade source (not our code): Rage hides/fades the player model when
  the render camera sits inside its bounding volume — our eye is inside the skull.
  PedHide only removes comps 0,7-10; torso may still LOD-flicker. C06alt handles
  the same problem with SET_DRAW on the script thread once, not periodically.

### 2.4 Height-fallback path is not pair-frozen

When the bone cache is invalid, `TryGetPedEyeCenterBase` reads the **live** ped
matrix per call (`cam_matrix.cpp:670-686`). Inside a dual pair the L and R passes
can then see different ped positions (game thread continues) → inter-eye vertical
disparity for those frames → stereo shimmer during exactly the transitions where
the bone is unreliable (doors, enter/exit, after load).

### 2.5 Multiple cam rewrites per frame with different data epochs

`ApplyHmdToCam` runs per tracked mat per eye pass, PLUS the late CCam refresh at
the FOV site (`cam_matrix.cpp:1744-1803`). Only the HMD pose is latched for the
pair (Mode 203+, `hmd_pose.cpp:37-49`); the controller-yaw offset, vehicle-yaw
offset and (in fallback) the ped eye are re-read each call. Any of these changing
between L and R shows up as inter-eye rotation — perceived as shake that "is in
the world".

### 2.6 Heading writers can still fight the walk animation

If the active mode uses the hard `ApplyPedHeading` (`vr_move.cpp:121-144`), the
ped matrix is snapped to yaw-only every frame — the walk anim's natural root
rotation/roll is destroyed each frame and re-applied by the next anim tick →
visible model "vibration" while moving. The soft path (heading floats only,
`look_move.cpp:114-117`) is the right one for FP; verify at home which one Mode
216 actually engages.

---

## 3. How serious VR mods attach the camera (comparison)

| Mod | Attach model | Lesson for us |
|---|---|---|
| **BotW BetterVR** (playbook L14-18) | Game head anchor sampled ONCE per stereo pair; HMD delta on top; body follows via game logic; render 2× per tick | Anchor = low-rate stable sample + HMD delta. Never let per-eye passes resample the body. |
| **L4D2VR** (`GetViewOriginLeft/Right`, playbook L15) | Engine gives ONE view origin per tick; VR adds eye offsets; the body/anim never feeds the render pose directly | The render eye derives from ONE per-tick origin. Anim bob enters only if the engine's origin has it — and Source's origin is already smoothed by the engine. |
| **HL2VR / VRIK-style** (playbook L16) | HMD OWNS the camera 1:1 (no filtering of head motion!); the BODY is IK-dragged under the head; arms via controller IK targets | Long-term correct model: never filter the HMD; filter only the *game anchor* the HMD rides on. Arms/hands = IK targets (GTA IV has `CPedIKManager` — see DECOUPLED_AIM_PLAN §4). |
| **Luke Ross R.E.A.L.** (INSPIRATION_NOTES L73-99) | Game cam ownership fights resolved at render time; pitch clamps overridden; body hidden near cam | Same problem class we have; he also anchors to a stable head point and fixes the view during draw. |

Common denominator: **the HMD pose is applied raw; only the anchor point under it
is stabilized, and it is stabilized ONCE per tick in body-local space.**
Our Mode 216 already went half-way (deferred sample, pair freeze); the remaining
bugs are the root-XY band filter (§2.1), world-space offset filtering (§2.2) and
visibility thrash (§2.3).

---

## 4. Proposed redesign — "Mode 220: HeadAnchor" (design, NOT headset-verified)

One behavior change per build at home; each sub-step is its own build.

### 4.1 Anchor math (replaces the two band filters)

```
per frame (still sampled once, same call site stereo_render.cpp:2298):
  root, heading  = live ped root + heading            (RAW — no smoothing)
  boneLocal      = RotZ(-heading) * (boneWorld - root) (ped-local offset)
  boneLocalSm    = EMA(boneLocalSm, boneLocal, aOff)   (ONE fixed alpha ~0.10)
  anchor         = root + RotZ(heading) * boneLocalSm
  anchor.z       = root.z + springZ(boneLocalSm.z)     (critically damped, ~5 Hz)
  eye            = anchor + up*0.06 + fwd*0.03         (keep Mode 216 offsets)
```

- Root XY is followed **1:1** (kills §2.1 lurch; walking translation is real).
- Bob lives in `boneLocal.z` (+ a little XY) and is filtered with ONE constant
  alpha — no adaptive band, so no limit cycle. Stairs/crouch work because root.z
  moves raw and the spring only shapes the residual bone offset.
- Local-space filtering fixes §2.2 (turns don't corrupt the filter).
- Keep the existing freeze: sample once → both eyes read the same cached eye.

### 4.2 Pair coherence for the fallback path

In `TryGetPedEyeCenterBase` height-fallback (`cam_matrix.cpp:670-686`): when
`StereoInParentDualWalk()` is true, cache the first computed fallback eye and
reuse it for the R pass (same pattern as `g_deferredWorldEye`). Small, isolated.

### 4.3 PedHide: state machine instead of pulses

`ped_hide.cpp`: remember desired state + last applied state per component; call
SET_DRAW **only on transition** (arm → hide once; disarm/kill → show once); add a
slow keep-alive (once per 5 s, outside the dual window — e.g. from
`SampleDeferredHeadBoneEye`, never from `ApplyHmdToCam`). Remove the
`cam_matrix.cpp:935` per-CopyMat call for all modes ≥216.

### 4.4 Heading policy

Mode 220 uses ONLY the soft heading write (`look_move.cpp:114-117`) or none
(free-move). The hard matrix rewrite (`vr_move.cpp:125-144`) stays for legacy
modes but is excluded from 220 by predicate.

### 4.5 Mode number / kill switch

- New `StereoMode` entry **220** (`OursFpHeadAnchor`), predicates mirror 216
  (`IsOursFpDeferredHeadBone` true for 220 as well, plus new
  `IsOursFpHeadAnchor` gating the new filter path).
- Config file stays `gtaiv_dxvk_vr.stereo`; **kill = write `216`** (previous
  live), hard kill `204`, disaster `0`.
- Every constant (`aOff`, spring frequency) read once from optional files
  (`gtaiv_dxvk_vr.anchoralpha`, `gtaiv_dxvk_vr.anchorspring`) with safe defaults
  so headset tuning needs no rebuild.

### 4.6 Files to touch at home (in order)

1. `src/asi/stereo_config.h/.cpp` — add mode 220 enum value + predicates + parser
   max-mode bump (search `ParseModeFile max`).
2. `src/asi/cam_matrix.cpp` — new local-space filter inside
   `SampleDeferredHeadBoneEye` (branch on `IsOursFpHeadAnchor`), leave the 216
   path byte-identical.
3. `src/asi/ped_hide.cpp` — state-tracked SET_DRAW (§4.3) — SEPARATE build.
4. `src/asi/cam_matrix.cpp:670-686` — fallback pair cache (§4.2) — SEPARATE build.
5. `docs/CURRENT-STATE.md` after each headset test.

### 4.7 Headset test checklist (5 steps, repeat per build)

1. **Walk/run** straight 30 s on flat ground — world must stay planted; no
   rhythmic lurch. (This is the §2.1 verification.)
2. **Stairs** up + down (the Rotterdam Tower steps or any brownstone stoop) —
   camera must track height without floating or popping.
3. **Crouch/stand** 5× — height change ≤0.5 s latency, no overshoot.
4. **Turn in place** 2×360° + strafe circle around a parked car — no sideways eye
   drift, no post-turn snap (§2.2 verification).
5. **Vehicle**: enter, drive a block, exit — eye stays between the eyes, no stuck
   offsets, model no flicker during enter/exit.
Log lines to check: `CamMatrix: deferred HEAD`, `PedHide:` (must be RARE now),
`StereoSubmit: L=1 R=1`.

---

## 5. Do NOT do (Track A)

- No canvas-zoom / claimed-FOV tricks (rejected 2026-07-24, `PLAN_NEXT.md`).
- No `D3DTS_PROJECTION` / SetTransform projection changes (Mode 15 dead).
- No Mode 46-style raw OpenVR basis into the late CCam path (froze; `PLAN_NEXT.md` §top).
- No new smoothing of the **HMD** pose — filter only the ped anchor.
- Do not touch Mode 14 geometry-canvas code (`CopyBbToEyeCanvas`) or the Mode 216
  constants in place — new mode number, new branch.
- No forbidden replay RVAs (`0x2C6AC`, `0x37BD0`, `0x1BF010`, `0x4DDAD0`, edge `0x309D0`).

---

## 6. Optional future: VRIK-style arms (design only)

Once decoupled aim works (Track B), GTA IV's own IK is the cheap path to
"hands follow controllers": `CPedIKManager` (CPed+0xBC0, IV-Network reversing —
UNVERIFIED for CE) has an aim-target vector the engine solves arms toward. Feed it
the controller ray hit point instead of the camera ray; the game animates the
arms. Full VRIK (custom bone posing per frame) stays out of scope — Rage
re-solves anims per tick and would fight us; the IK-target route uses the engine
solver instead of fighting it. See `DECOUPLED_AIM_PLAN.md` §4.
