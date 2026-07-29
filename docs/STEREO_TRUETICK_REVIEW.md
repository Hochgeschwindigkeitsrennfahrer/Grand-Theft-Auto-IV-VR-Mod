# Stereo / Same-Tick Review + TrueStereo Redesign (offline, 2026-07-29)

Written on the work PC — **no headset / no game / no Visual Studio**. Nothing here is
headset-verified and the scaffolding in `src/asi/stereo_calib.*` was never compiled.

Read together with `docs/CURRENT-STATE.md`, `docs/CAMERA_ATTACH_REVIEW.md` (Track A,
camera anchor) and `docs/PLAN_NEXT.md` (rejected experiments).

Scope: the stereo **image** path of the Mode 200/203/204/216 parent dual — geometry,
disparity, submit. The walk/run "jumping" is a camera-anchor problem and stays in
`CAMERA_ATTACH_REVIEW.md` (Mode 220). The two are independent; do not fix them in
one build.

---

## 1. How stereo works today (exact map)

### 1.1 The same-tick dual

| What | Where |
|---|---|
| Parent draw-shell hook (204 seam `@0x4DE020`, 203 seam `@0x4D8BF0`) | `src/asi/stereo_render.cpp:2268` (`HookDrawWalk`) |
| L/R dual body | `src/asi/stereo_render.cpp:2565-2602` (`RunMode200ParentDualGuarded`) |
| HMD pose latched for the pair | `src/asi/hmd_pose.cpp:37-49` (`BeginDualHmdPoseLatch`) |
| Per-eye camera bake | `src/asi/cam_matrix.cpp:718` (`ApplyHmdToCam`), re-applied per eye by `RefreshLiveCamForStereoEye` (`cam_matrix.cpp:1497`) |
| Per-eye capture into the angle canvas | `src/asi/stereo_render.cpp:1092` (`CopySurfToEyeCanvas`) |
| Submit | `stereo_render.cpp:7676` (`StereoTrySubmitEyes`), nullptr bounds on the canvas |

Per parent-dual pair:

```
HookDrawWalk (one game tick, logic frozen)
  ├ BeginDualHmdPoseLatch          one HMD pose for BOTH eyes
  ├ SetStereoEye(Left)  → RefreshLiveCamForStereoEye → origDrawWalk → CopyBbToEyeCanvas(L)
  ├ SetStereoEye(Right) → RefreshLiveCamForStereoEye → origDrawWalk → CopyBbToEyeCanvas(R)
  └ SetStereoEye(Left)  → RefreshLiveCamForStereoEye
EndScene → StereoTrySubmitEyes → IVRCompositor::Submit(L), Submit(R)
```

**Same-tick is solved.** The engine renders the world twice from two real camera
matrices while the simulation does not advance. That is the L4D2VR / BotW model and it
is why 203 was the first build that felt like a place instead of a screen. Everything
below is about what still sits *on top* of that correct foundation.

### 1.2 Angular mapping (the canvas)

Rage ignores `D3DTS_PROJECTION` (Mode 15, proven dead), so the per-eye **asymmetric**
frustum cannot be set on the projection. Instead the game renders one symmetric
frustum and `CopySurfToEyeCanvas` places that image at its correct angular position
inside each eye's frustum:

```
overlap  = [max(-gameH, l) .. min(gameH, r)] x [max(-gameV, t) .. min(gameV, b)]
src rect = that overlap mapped back into backbuffer pixels     (crop when game is wider)
dst rect = that overlap mapped into the eye canvas             (inset when game is narrower)
Submit(canvas, bounds = nullptr)
```

`l/r/t/b` are the true per-eye tangents from `IVRSystem::GetProjectionRaw`
(`src/asi/vr_display.cpp:63-83`). `gameH/gameV` are the **claimed** half-tangents of
the game image. The construction is right. Its accuracy is entirely the accuracy of
`gameH/gameV`.

### 1.3 Where `gameH/gameV` come from (Mode 154-216, incl. 203/204/216)

1. `WantsFpAbsoluteFov` writes `fpfov` (110 deg for Mode 186+) into `CCam+0x60`.
2. `PublishGameFovUnderPublishFit` (`vr_display.cpp:295-354`) converts those degrees
   into tangents and then deliberately shrinks them to fit the HMD cover frustum:
   - `vDeg = ccamDeg * kEng` with **`kEng = 58.7f / 45.f`** (`vr_display.cpp:238`)
   - `trueV = tan(vDeg/2)`, `trueH = trueV * bbAspect`
   - `scale = min(coverH/trueH, coverV/trueV)`, capped at 1, times `overscan`
   - published `gameTan = true * scale`

### 1.4 Disparity

`cam_matrix.cpp:1019-1028`:

```
ws   = GetWorldScale()      (Mode 187+: multiplies the stereo baseline too)
half = 0.5 * GetStereoSepMeters() * GetStereoScale() * ws
eye += hmdRight * (±half) + forward * (-eyeZ * ws)
```

`GetStereoScale()` default is **1.15** (`stereo_config.cpp:26`), the F7 presets push it
to 1.25-1.30 (`stereo_config.cpp:48-59`), and Mode 162+ enters at preset 9 = 130.

---

## 2. Diagnosis — why near objects will not fuse

### 2.1 PRIMARY: the angular scale of the game image is guessed, not measured

`kEng = 58.7/45 = 1.3044` was derived once from a Mode 16 probe at CCam 45 deg
(`vr_display.cpp:237`). It is applied linearly to every CCam value. At the Mode 186+
setting `fpfov = 110` that predicts **143.5 deg vertical** and `tanV = 3.04`. No Rage
build renders that; the engine clamps or the relation is not linear that far out. So
the canvas is told the image spans an angle it does not span.

The constant is duplicated in three places — whoever implements 231 must handle all
of them: `vr_display.cpp:238` (`PublishGameFovFromCCamDegrees`), `vr_display.cpp:304`
(`PublishGameFovUnderPublishFit`) and `stereo_config.cpp:2248`
(`TryApplyCoverMatchedFovAdd`, Mode 120s family only).

Consequence: every world direction lands at the wrong angle in the headset — by the
same factor in both eyes. A **uniform angular magnification `m`** does not just make
the world look wrong-sized. It multiplies every retinal disparity by `m` as well.
Fusing a point at distance `d` then requires the vergence of a point at `d/m`. The
horizon is unaffected (zero disparity times anything is still zero), which is exactly
the reported symptom: **far fuses, near doubles**.

### 2.2 The FOV is then falsified a second time, on purpose

`PublishGameFovUnderPublishFit` shrinks the published tangents by `min(cover/true)` so
the canvas fills the frustum without black bars (`vr_display.cpp:318-336`), plus an
`overscan` factor (`GetUnderPublishOverscan`, 1.0 for the 161+ family). That is a
deliberate second angular error stacked on the first. It buys cosmetics (no bars) and
pays with fusion.

### 2.3 The baseline is not the IPD

With `stereoscale` at 1.15-1.30 the rendered eye separation is 15-30 % wider than the
user's real IPD. Wider baseline = proportionally more disparity at every distance,
i.e. it multiplies with §2.1/§2.2 instead of cancelling them.

### 2.4 What Modes 210-215 actually did

- **210/211 toe-in** (`cam_matrix.cpp:1038-1073`) rotates the two view axes toward a
  convergence point. Real stereo cameras must stay **parallel**; the vergence is the
  user's job. Toe-in adds vertical disparity that grows toward the image corners —
  it can never fully fuse, which is exactly what the headset reported.
- **212/213/214 IPD 1 / 0.5 / 0.25 cm** shrink the baseline until `baseline x m` lands
  back inside the fusion budget. It works, and it deletes the stereo effect: at
  0.25 cm the depth cue is 4 % of a human's. "Getting there, wants a bit more" is the
  signature of compensating a scale error with a broken baseline.

Both are symptom treatments for §2.1. Remove the scale error and neither is needed.

### 2.5 Same tick is not the same state

The two `origDrawWalk` calls share the tick but not every engine side effect:

- **Auto-exposure / HDR adaptation** updates per rendered frame. Pass 2 sees a
  different exposure than pass 1 → the two eyes differ in brightness → binocular
  luminance rivalry, which suppresses fusion on its own and reads as flicker.
  Own evidence: Mode 191 "whole-screen+UI flicker when looking up/out, gone looking
  down (post/exposure rivalry hypothesis)" (`stereo_config.h:526`); Mode 204 "phone
  washout" (`:577`); Mode 206 "no near-bounce, no phone washout" (`:583`).
- Particle/effect ticks, occlusion-query results and any frame-indexed jitter can
  differ the same way.

### 2.6 Not everything is latched for the pair

Only the HMD pose is frozen (`hmd_pose.cpp:37`). Re-read per `ApplyHmdToCam` call, i.e.
possibly different between L and R:

- controller yaw offset and vehicle yaw offset (`cam_matrix.cpp:718-1096`)
- the height-fallback eye when the bone cache is invalid (`cam_matrix.cpp:670-686`)

Any of these changing mid-pair is an inter-eye rotation or translation the brain reads
as world shake. See `CAMERA_ATTACH_REVIEW.md` §2.4/2.5.

### 2.7 Two avoidable resampling / rounding errors

`CopySurfToEyeCanvas` computes its rectangles with `static_cast<LONG>` truncation
(`stereo_render.cpp:1173-1182`), independently per eye, so L and R can land up to one
pixel apart in opposite directions. It then `StretchRect`s with `D3DTEXF_LINEAR` into a
canvas that the compositor resamples again for lens distortion. Small compared to
§2.1, but pure loss.

### 2.8 What is NOT the problem

- The dual seam. 204 `@0x4DE020` is stable, ~1 entry/ES, no crashes.
- The canvas *construction* (the overlap math is correct).
- The temporal path — there is none any more in 200+.
- The walk jumping. That is the root-XY band filter, `CAMERA_ATTACH_REVIEW.md` §2.1.

---

## 3. How other mods handle the same wall

| Mod | Per-eye projection | Lesson |
|---|---|---|
| **L4D2VR** | Engine accepts a per-eye asymmetric frustum directly | Not available to us — Rage ignores `D3DTS_PROJECTION`. |
| **UEVR** | Renders wide, crops per eye when it cannot own the projection | The crop path is legitimate and exact **if** the source FOV is known exactly. |
| **Luke Ross R.E.A.L.** | Wide symmetric render, per-eye sub-rect at submit | Same shape as our Mode 232/233 target. |
| **Halo MCC VR** | True per-eye same-frame, engine FOV as the primary display knob | FOV first, then stereo, then HUD — our order too. |

Common denominator: nobody guesses the source FOV. They either own the projection or
they measure it. We do neither today.

---

## 4. Redesign — "TrueStereo", Mode 230-234

Principle: **measure instead of guess, render wider than the eye sees, crop exactly per
eye, use the real IPD, keep both passes in the same state.**

All five inherit the proven 204 seam (`IsOursFpSameTickParentDualTry2`), the square
render path, `fpfov`, native ped hide and the atomic eye-pair promote. 203/204/216 and
`CopyBbToEyeCanvas` stay byte-identical — every step is a new mode number.

Status: **230 implemented (read-only). 231-234 are design only** and are remapped to
Mode 204 by the config parser until someone implements them (`stereo_config.cpp`
`ReloadStereoMode`).

```
230 PROBE   measure the real projection            (read-only, no behavior change)
231 EXACT   publish measured tangents, IPD = HMD   (bars allowed)
232 COVER   raise engine FOV until game >= eye     (bars gone, honestly)
233 DIRECT  drop the canvas, float TextureBounds   (no resample, no rounding)
234 STATE   freeze exposure + latch the whole pair (kill luminance rivalry)
```

### 4.1 Mode 230 PROBE — measure the true projection

`HookSetVSConstF` (`src/asi/stereo_proj.cpp:191`) already sees every vertex-shader
constant upload and already contains a proven detector for "4x4 block that maps the
build camera position onto the view axis" (`:233-261`), in both row-vector and
column-vector layout. Such a block is the view or the view-projection matrix. Mode 25
confirmed the layout live: row-vector, view at `c4`, viewProj at `c8`.

**Do not invert matrices and do not assume a convention.** Probe the matched block with
three world points built from the camera we wrote ourselves
(`BuildLiveViewMatrix16` + `GetLastStereoCamPos`), using the same multiply the detector
matched with:

```
d  = 2 m
Pc = c + f*d              (straight ahead)
Pr = c + f*d + r*d        (x_view = z_view = d  ->  tan = 1)
Pu = c + f*d + u*d

q  = transform(P)                       (layout 1: row-vector; layout 2: column-vector)
viewProj  <=>  ||q_c.w| - d| < 0.25*d   (a pure view matrix gives q.w = 1, rejected)
ndc       = q.xy / q.w

sH = ndcR.x - ndcC.x                    (= 1/tanH, signed)
sV = ndcU.y - ndcC.y
tanH = 1 / |sH|                         (half-tangent, horizontal)
tanV = 1 / |sV|
centerTanH = -ndcC.x / sH               (frustum asymmetry; must be ~0)
centerTanV = -ndcC.y / sV
```

Signs cancel in every derived value, so a right-handed or flipped-Y engine
convention changes nothing but the logged `sign=` field.

This is convention-free, needs no inverse, and is exact to float precision.

Logged once per ~120 pairs, per eye (`src/asi/stereo_calib.cpp`):

```
StereoCalib: #3 eye=L layout=row reg=8 measTan=(1.2340,1.2340) pubTan=(0.9870,0.9870)
  errH=+25.0% errV=+25.0% center=(+0.0010,-0.0020) cover=(1.1500,1.1800)
  coverFill=107%h/105%v ccamFov=110.0 kEngWouldSay=(5.4100,3.0400) sign=(+1,-1)
```

`errH/errV` is the whole point: how far the published canvas angle is from the angle
Rage really renders. `center` must be ~0 (a symmetric engine frustum); `coverFill`
says whether Mode 232 has to raise the FOV at all.

Companion lines: `StereoCalibHmd:` once (per-eye `GetProjectionRaw`, cover tangents,
`GetEyeToHeadTransform` translation plus cant angle — if the eye frames are rotated, a
pure-translation IPD is wrong), `StereoCalibRaw:` once (the 16 floats of the matched
block, for archaeology), and — if nothing matches — an explicit failure line naming
`uploads`, `axisMatches` and `camBasisFails` so a silent probe cannot be mistaken for
a clean result.

Optional, off by default (`gtaiv_dxvk_vr.caliblum` = 1): downsample both eye textures
to 8x8, read back, log mean luminance L vs R. This quantifies §2.5 before anyone
spends a build on it.

```
StereoCalibLuma: L=118.4 R=131.7 delta=+11.2%
```

**Read-only. No game function hooked, no write, no draw duplicated beyond the existing
204 dual. Kill: `204`.**

Headset run: 60 s, stand still 10 s, walk 10 s, look up at the sky, look down at the
ground, enter a car. Then read `errH/errV` — that single number decides everything that
follows.

### 4.2 Mode 231 EXACT — correct geometry, bars allowed

One behavior change: **stop lying about angles and disparity.**

- Publish the measured tangents from 230 as `gameTan`; bypass
  `PublishGameFovUnderPublishFit` and `PublishGameFovFromCCamDegrees` for 231+.
  Keep the existing stability/publish gate so a zoom (sniper, cutscene) still updates.
- Fall back to the old `kEng` path only while no measurement exists yet (first seconds
  after load), and log clearly which source is live.
- `GetStereoScale()` returns 1.00 for 231+ (do not delete the file, ignore it).
- `sep` = HMD IPD from `ReadHmdIpdMeters()`; write it once on mode enter.
- Toe-in off (`IsOursFpSameTickParentDual203ToeIn` must be false for 231+).
- `worldscale` keeps its 6DoF-lean job only; it must not multiply the baseline
  (`IsOursFpTrueWorldScale` false for 231+).

Expect black borders wherever the game renders narrower than the eye sees. **That is
the honest state and it is fixed in 232, not by falsifying the angle.**

Headset checklist:
1. Hold a hand / lean toward a wall at ~40 cm — must fuse, single image.
2. Dashboard and steering wheel in a car — must fuse.
3. Street 5-50 m — depth must feel graded, not like a flat backdrop.
4. Horizon — no vertical offset between the eyes (close one eye, then the other:
   objects must not jump up/down, only sideways).
5. Look up / down 60 deg — no growing double image toward the corners.

Kill: `204`. Hard kill: `203`, then `0`.

### 4.3 Mode 232 COVER — full field without lying

Raise the engine FOV (`fpfov`) in steps until the **measured** tangents reach the cover
tangents, i.e. `measTan >= coverTan` on both axes. From that point
`CopySurfToEyeCanvas` only ever crops the source (the `txLo/txHi` branch already does
this) and the destination is the full canvas — no bars, and the crop is per eye
asymmetric, which *is* the missing asymmetric frustum.

Because 230 measures live, it does not matter where Rage clamps: raise `fpfov`, read
`measTan`, stop when covered. If the engine clamps below what the G2 needs, accept a
few degrees of peripheral bar. **Correct geometry beats a full field.**

Angular resolution is the cost: cropping a 1440x1440 square render to the central
~100 deg throws pixels away. Compensate in `commandline.txt` (square, e.g. 2048x2048
or higher — quality-first budget allows 45 FPS with reprojection). Log the effective
pixels-per-degree in the same `StereoCalib` line so the trade is visible.

Headset checklist: same five as 231, plus "no bars" and a sharpness A/B against 231.

Kill: `231`.

### 4.4 Mode 233 DIRECT — remove the resample and the rounding

Once the game covers the eye frustum, the canvas is dead weight:

- Copy the backbuffer 1:1 into the eye texture (same size, no scaling, no filter).
- Submit with `vr::VRTextureBounds_t` computed in float from measured tangents:
  `uMin = 0.5 + 0.5 * l / measTanH`, `uMax = 0.5 + 0.5 * r / measTanH`,
  `vMin = 0.5 - 0.5 * b / measTanV`, `vMax = 0.5 - 0.5 * t / measTanV`
  (v flipped, same convention as `EnsureBoundsCache`, `vr_display.cpp:86-93`).

No `StretchRect` filter, no integer rounding, one less full-frame copy per eye.

This is **not** the failed Mode 180: that put UV bounds on top of the already-remapped
canvas (double mapping = warp). Here the bounds *are* the mapping and the source is the
untouched backbuffer.

Guard: if `measTan < coverTan` on either axis (engine clamped, or a cutscene narrowed
the FOV), fall back to the 232 canvas path for that frame — bounds outside 0..1 would
sample garbage.

Kill: `232`.

### 4.5 Mode 234 SAME-STATE — one state for both eyes

Only start this after the 230 luminance numbers exist.

1. **Exposure.** If `StereoCalibLuma` shows a systematic L/R difference, the adaptation
   runs between the passes. Options in order of preference:
   a. Find the adaptation lerp / delta-time input (Cheat Engine: float that settles
      after a bright-to-dark transition) and force it to 0 for the duration of the
      pair.
   b. Snapshot and restore the adaptation state around the second pass.
   c. Last resort: slow the adaptation globally (visible gameplay change — only if
      a and b fail).
2. **Pair latch.** Extend the pair freeze from "HMD pose only" to everything the camera
   bake reads: controller yaw, vehicle yaw, and the height-fallback eye
   (`cam_matrix.cpp:670-686`). Same pattern as `g_deferredWorldEye`: first computed
   value of the pair wins, second pass reuses it.

Headset checklist: look up at bright sky then down at dark asphalt while standing
still — no eye-to-eye brightness pumping; car enter/exit and doorways — no shimmer.

Kill: `233`.

---

## 5. Do NOT do

- No canvas zoom / claimed-FOV warp (rejected 2026-07-24).
- No `D3DTS_PROJECTION` / `SetTransform` projection widen — Mode 15 proved Rage
  ignores it.
- No toe-in, no `stereoscale` above 100 and no baseline shrink as a fusion fix. Those
  are the compromises this redesign exists to remove.
- No forbidden replay RVAs: `0x2C6AC`, `0x37BD0`, `0x1BF010`, `0x4DDAD0`, indirect edge
  `0x309D0`.
- No modification of `CopyBbToEyeCanvas` / Mode 14 geometry or of the 203/204/216
  constants — new mode number for every experiment.
- No filtering of the HMD pose. Filter only the ped anchor (Track A).
- Do not mix this with Mode 220 (camera anchor) in one build.

---

## 6. Files

| File | Role |
|---|---|
| `src/asi/stereo_calib.h/.cpp` | Mode 230 read-only probe (NOT COMPILED — work PC) |
| `src/asi/stereo_proj.cpp` | one gated call into the probe from `HookSetVSConstF` |
| `src/asi/stereo_render.cpp` | one gated call per pair (`StereoCalibOnEndScene`) + optional luma sample |
| `src/asi/stereo_config.h/.cpp` | modes 230-234, family predicates, 231-234 remap to 204 |
| `scripts/build-asi.ps1` | new source file |

## 7. Order at home

1. `.\scripts\build-asi.ps1` on the unchanged repo. If `stereo_calib` fails to compile,
   fix signatures only — the logic is deliberately trivial and self-contained.
2. `gtaiv_dxvk_vr.stereo` = `230`, 60 s run, read the `StereoCalib:` lines.
   This one number (`errH/errV`) tells you whether §2.1 is really the main error.
3. Only then implement 231 (§4.2) as its own build.
4. Update `docs/CURRENT-STATE.md` after every headset test, kill value written down
   before the test.
