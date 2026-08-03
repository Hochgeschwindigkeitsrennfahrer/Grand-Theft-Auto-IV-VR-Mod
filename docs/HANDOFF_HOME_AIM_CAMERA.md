# HOME HANDOFF — Stereo Probe + Camera Attach + Decoupled Aiming (prepared offline 2026-07-29)

For the at-home agent (Grok 4.5) with **game + SteamVR + Reverb G2 + Cheat Engine**.
Prepared on a work PC with none of those — **nothing below is headset-verified**,
including the new scaffolding compile (no VS on the work PC).

**Branch note (2026-07-29 home):** motion-control work runs on
`feature/motion-controls` with stereo baseline **Mode 243** (241 stereo +
late-latch; `prebuilt/mode243/`). Kill: `241` / `203` / `0`. Do not mix
HeadAnchor / blur experiments from `feature/first-person-controller` into
motion-control builds. Track A (Mode 220 camera) and Track C (230+ TrueStereo)
stay reference-only unless explicitly scheduled; Track B (decoupled aim) is the
active goal.

Read first: `AGENTS.md`, `docs/CURRENT-STATE.md`, `docs/CONSTRAINTS.md`, then the
three analysis docs this handoff is built on:
- **Track A:** `docs/CAMERA_ATTACH_REVIEW.md` (attach map + diagnosis + Mode 220 design)
- **Track B:** `docs/DECOUPLED_AIM_PLAN.md` (aim plans A-D + CE recipes + checklist)
- **Track C:** `docs/STEREO_TRUETICK_REVIEW.md` (why near objects double + Mode 230-234)

---

## 1. Executive summary

**Prepared offline (done):**
- Full map of the current camera↔ped attach with file:line cites; mechanistic
  diagnosis of the walk jump (root-XY band filter limit cycle), turn drift
  (world-space offset filtering), and model spazz (SET_DRAW pulse thrash) —
  `CAMERA_ATTACH_REVIEW.md` §1-2.
- A concrete redesign ("Mode 220 HeadAnchor") with exact math, files to touch,
  kill switches, and a 5-step headset checklist — `CAMERA_ATTACH_REVIEW.md` §4.
- Decoupled-aim foundation: Holydh's GTA SA pattern extracted; **three IV-specific
  seams ranked** (natives > IK target > aim-cam hook > instruction hook);
  step-by-step Cheat Engine recipes; missing-address checklist —
  `DECOUPLED_AIM_PLAN.md`.
- **Compile-ready scaffolding** (`src/asi/aim_decouple.h/.cpp`, wired into
  `openvr_mono.cpp` + `build-asi.ps1`): controller pose cache from the existing
  WaitGetPoses array, GTA-axis conversion, probe logging at `aimmode=1`.
  Default `aimmode=0` = fully inert.
- **Stereo diagnosis + Mode 230 probe** (`STEREO_TRUETICK_REVIEW.md`): the reason
  near objects double is that the canvas is told the game image spans an angle it
  does not span (guessed `kEng`, then under-publish shrink), multiplied by a
  baseline that is 15-30 % wider than the real IPD. `src/asi/stereo_calib.h/.cpp`
  measures the real projection from the existing `SetVertexShaderConstantF` hook —
  read-only, inert outside Mode 230.

**Needs headset / CE at home:**
- FIRST: `.\scripts\build-asi.ps1` must pass (scaffolding was never compiled).
- Mode 230 probe run — one number (`errH/errV`) decides the whole stereo redesign
  (Session 0, 10 minutes, no code).
- Mode 220 implementation + walk/stairs/crouch verification (Session 1).
- CE finds: aim forward vector, aim-cam CopyMat AOB, CE CPedIK offset, native
  handler availability (Sessions 2+, recipes in `DECOUPLED_AIM_PLAN.md` §5).

---

## 2. Track A — what to change and how to test

Diagnosis recap (details + cites in `CAMERA_ATTACH_REVIEW.md` §2):
1. Walk jump = root-XY smoothing treats walking translation (3-4 cm/frame) as
   "bob", lags, then lurches when the error leaves the band
   (`cam_matrix.cpp:1340-1359`). **Fix: follow root 1:1; filter only
   bone-minus-root, in ped-LOCAL space.**
2. Turn drift/snap = bone offset filtered in world space (`:1313-1337`).
3. Model spazz = periodic SET_DRAW re-hides, potentially flipping between L and
   R passes (`ped_hide.cpp:106,125-144`, called per-CopyMat on non-216 modes at
   `cam_matrix.cpp:935`). **Fix: state machine, hide on transitions only.**
4. Fallback eye not pair-frozen (`cam_matrix.cpp:670-686`) → inter-eye shimmer
   at doors/enter/exit.

Build order at home (ONE change per build, headset test between each):
1. Build unchanged repo → confirms scaffolding compiles + Mode 216 unchanged.
2. Mode 220 anchor math (new branch in `SampleDeferredHeadBoneEye`) — test with
   the 5-step checklist (`CAMERA_ATTACH_REVIEW.md` §4.7). Kill: stereo `216`,
   hard `204`.
3. PedHide state machine — separate build; watch for "model spazz better?".
4. Fallback pair cache — separate build; test doors + car enter/exit.

---

## 3. Track B — plan + first session content

Ranked plans (full detail `DECOUPLED_AIM_PLAN.md` §2):
- **Plan A (start here): natives.** `TASK_AIM_GUN_AT_COORD` /
  `TASK_SHOOT_AT_COORD` / `FIRE_SINGLE_BULLET` aimed at
  `controllerPos + controllerFwd × 25 m`. Zero memory writes; our native-invoke
  path is proven (PedHide). First step is only a LOG of
  `GetNativeHandlerByName("TASK_AIM_GUN_AT_COORD")` etc. — if handlers resolve
  on CE, the whole feature likely ships through natives.
- **Plan B: CPedIK vecAimTarget write** (CE offset unknown; 1.0.7 hint CPed+0xBC0→+0x70).
- **Plan C: hook the aim-cam CopyMat site** (5th sibling of our 4 hooks) — also
  fixes "aiming switches to third person" (`HANDOFF_GROK.md` §4.6) regardless of
  aiming work.
- **Plan D (last resort): shoot-function arg hook.**

CE recipes for the user (non-programmer, follow literally):
`DECOUPLED_AIM_PLAN.md` §5.1-5.4. Address checklist to fill: §6 table.

---

## 3b. Track C — stereo geometry (new)

Diagnosis recap (cites in `STEREO_TRUETICK_REVIEW.md` §2):
1. Same-tick is **already solved** — `HookDrawWalk` renders the world twice per
   frozen tick. Do not "fix" it again.
2. `gameTan` is guessed: `kEng = 58.7/45` at `fpfov 110` would mean 143 deg
   vertical, which the engine cannot render. Wrong angular scale multiplies every
   disparity → horizon fuses, near doubles.
3. `PublishGameFovUnderPublishFit` then shrinks the tangents on purpose so no black
   bars appear — a second angular error, paid for with fusion.
4. `stereoscale` 1.15-1.30 makes the baseline wider than the user's IPD.
5. Modes 210-215 (toe-in, IPD 0.25 cm) were compensating 2-4 by destroying the
   stereo base. Do not go further down that road.

Build order (ONE change per build):
1. Mode **230** — measure only, no behavior change. Read `StereoCalib:`.
2. Mode **231** — measured tangents + IPD = HMD + no toe-in. Bars allowed.
3. Mode **232** — raise `fpfov` until measured >= cover; bars gone honestly.
4. Mode **233** — drop the canvas, float `VRTextureBounds`.
5. Mode **234** — exposure freeze + full pair latch (only if the luma probe shows
   an L/R difference).

Kill chain: `231` → `204` → `203` → `0`. 231-234 currently remap to 204 in
`ReloadStereoMode` — delete that remap in the build that implements them.

---

## 4. Ordered home sessions (one behavior change per build)

### Session 0 — stereo probe (Track C, 10 minutes, no code)
1. `.\scripts\build-asi.ps1` on the unchanged repo. If `stereo_calib` or
   `aim_decouple` fail to compile, fix signatures only.
2. `gtaiv_dxvk_vr.stereo` = `230`. This is Mode 204 behavior plus logging —
   if it feels different from 204, something is wrong; kill to `204`.
3. Play 60 s: stand, walk, look up at the sky, look down, enter a car.
4. Read the log:
   - `StereoCalib: ... measTan=(..) pubTan=(..) errH=.. errV=..` — the answer.
     `errH/errV` near 0 % means the canvas angle is right and the fusion problem is
     elsewhere (baseline / exposure). A large error (tens of percent) confirms the
     diagnosis and 231 is the next build.
   - `StereoCalibHmd:` — check `cant` is ~0 and the HMD IPD value looks sane.
   - `StereoCalib: NO viewProj block matched ...` — the probe found nothing; report
     `uploads`, `axisMatches`, `camBasisFails` before changing anything.
5. Optional second run with `gtaiv_dxvk_vr.caliblum` = `1` for
   `StereoCalibLuma:` (auto-exposure rivalry, feeds Mode 234). Costs a GPU
   readback; delete the file afterwards.
6. Write the measured numbers into `docs/CURRENT-STATE.md`. Then decide: Track C
   231, or Track A Session 1 first. **Not both in one build.**

### Session 1 — camera stability (Track A)
1. `.\scripts\build-asi.ps1` on the unchanged repo. If `aim_decouple` fails to
   compile, fix signatures only (logic is intentionally trivial); do not touch
   other files.
2. Implement Mode 220 (`CAMERA_ATTACH_REVIEW.md` §4.1, §4.6 steps 1-2).
3. Headset checklist §4.7. Update `CURRENT-STATE.md`. Kill: `216`/`204`.
4. If walk is fixed: PedHide state machine as the next build (§4.3).

### Session 2 — find the aim data (Track B, mostly CE + logs)
1. Set `gtaiv_dxvk_vr.aimmode` = `1`. Confirm `AimProbe:` lines show a valid
   controller pose on the G2 (proves the new pose path).
2. Add the one-time native handler resolution log (Plan A feasibility).
3. CE: aim forward vector hunt (`DECOUPLED_AIM_PLAN.md` §5.1) + freeze test.
4. CE: aim-cam CopyMat writer while aiming (§5.3) → new AOB → add
   `HookOneCall("aim_cam", …)` next to `cam_matrix.cpp:1435-1438` as its own
   build (this alone fixes aim→third-person).
5. Fill the §6 checklist table with verified addresses (date + game version).

### Session 3 — weapon/hand follows controller
1. Plan A first: while grip held, `TASK_AIM_GUN_AT_COORD` at the controller ray
   point every ~150 ms (`aimmode=2` gate). Niko's arms track via engine IK.
2. If task natives misbehave: Plan B (IK vecAimTarget write, CE offset from
   Session 2) or Plan C matrix override.
3. Visual weapon-at-controller (bone/entity matrix) only AFTER shooting works —
   checklist #6/#7.

### Session 4 — shoot along the weapon
1. Trigger → `TASK_SHOOT_AT_COORD` (or `FIRE_SINGLE_BULLET` from the controller
   muzzle for full decouple). On-foot only; vehicles excluded (Holydh lesson).
2. Verify bullet impacts follow the controller, not the head. HUD/radar sanity:
   when NOT aiming, no override (radar must not track the gun).
3. Tune: aim point distance (10-40 m), hold-vs-toggle grip, left-hand config
   (`gtaiv_dxvk_vr.aimhand`).

Every session ends with: update `docs/CURRENT-STATE.md`, one paragraph per build,
kill values written down BEFORE testing.

---

## 5. Paste prompt for the home agent

```text
Read AGENTS.md, docs/CURRENT-STATE.md, docs/HANDOFF_HOME_AIM_CAMERA.md,
docs/CAMERA_ATTACH_REVIEW.md, docs/DECOUPLED_AIM_PLAN.md,
docs/STEREO_TRUETICK_REVIEW.md.
Project: gtaiv-dxvk-vr — GTA IV CE VR, stock DXVK 3.0.2 d3d9.dll + gtaiv_dxvk_vr.asi
+ OpenVR/SteamVR, Win32/x86, Reverb G2. I am not a programmer — give exact
click/command steps and tell me which log lines to check.
Baselines to protect: Mode 14 geometry canvas, Mode 243 fused stereo + late-latch
(kill 241 / 203 / 0; prebuilt/mode243), no rejected FOV/canvas-zoom tricks, no
forbidden replay RVAs (PLAN_NEXT.md). Branch: feature/motion-controls.
Today's goal: Session <N> from HANDOFF_HOME_AIM_CAMERA.md §4 — <one line>.
One behavior change per build; deploy via .\scripts\build-deploy-run.ps1; after
each headset test update docs/CURRENT-STATE.md. Keep gtaiv_dxvk_vr.stereo=243.
I have Cheat Engine and can follow the recipes in DECOUPLED_AIM_PLAN.md §5.
```

---

## 6. Risks / do-not-touch list

- **Mode 14 geometry-canvas fusion** (`stereo_render.cpp CopyBbToEyeCanvas`) —
  never modify; any experiment = NEW mode number.
- Rejected forever: canvas zoom / claimed-FOV warp, `D3DTS_PROJECTION`
  SetTransform tricks (Mode 15), square commandline without engine FOV, Mode 46
  raw-basis late CCam write, FusionFix FieldOfView for size.
- Forbidden replay RVAs: `0x2C6AC`, `0x37BD0`, `0x1BF010`, `0x4DDAD0`, indirect
  edge `0x309D0` — not aim-related, no shortcuts.
- No natives from raw EndScene context — use the proven PedHide invoke path.
- Win32/x86 only; OpenVR only (no OpenXR this milestone); singleplayer offline;
  never ship/describe online use.
- No HL2VR closed binaries; techniques only.
- Do not hard-code any address from `DECOUPLED_AIM_PLAN.md` §6 until CE-verified
  at home; label every verified value with date + game build.
- Work-PC scaffolding (`aim_decouple.*`, `stereo_calib.*`) was NOT compiled —
  first home build verifies. `aimmode` default 0 and the Mode 230 gate keep both
  inert even if the logic is wrong.
- Do not combine Track A (Mode 220 camera anchor) and Track C (Mode 230+ stereo)
  in one build — they fix different symptoms and would confuse the headset report.
- Track C dead ends, do not retry: toe-in (210/211), micro-IPD (212-215), canvas
  zoom, `D3DTS_PROJECTION`, UV bounds on the remapped canvas (Mode 180).
