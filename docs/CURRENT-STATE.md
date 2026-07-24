# CURRENT-STATE — gtaiv-dxvk-vr

**As of:** 2026-07-23 ~23:45  
**Version:** **1.5.0** — Mode 7 spike (RT capture + HMD projection); default stereo **off**.

---

## Architecture (proven)

```
GTAIV.exe
  → d3d9.dll          = Stock DXVK 3.0.2 x32
  → FusionFix         = ASI loader + CE fixes (Graphics API = DX9 / our d3d9)
  → gtaiv_dxvk_vr.asi + openvr_api.dll
       → EndScene: WaitGetPoses → Mono-Submit L+R (same texture)
       → CopyMat hooks: FP cam on ped + HMD orientation + 6DoF (F9)
       → vr_move: walk in look direction + right-stick yaw (inverted sign fixed)
  → SteamVR → HP Reverb G2
```

| Path | Role |
|------|------|
| Game folder | `C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV` |
| Log | `...\GTAIV\gtaiv_dxvk_vr.log` |
| Stereo kill switch | `...\GTAIV\gtaiv_dxvk_vr.stereo` (single digit 0–8) |
| IPD scale | `...\GTAIV\gtaiv_dxvk_vr.ipd` (percent 1–100); **F8** cycles live |
| Build | `.\scripts\build-asi.ps1` → `out-asi\gtaiv_dxvk_vr.asi` |
| Deploy + start | `.\scripts\build-deploy-run.ps1` (Build→Kill→Deploy→Steam start) |
| Deploy only | `.\scripts\deploy-asi.ps1 -GameDir "<GTAIV>"` (`-Launch` starts the game) |

**Currently deployed:** `gtaiv_dxvk_vr.stereo` = **`14`** (GeometryCanvas — fusion confirmed).
Mode **15** built and ready to test (black-bar widen spike). Kill switch stays **`0`**.

---

## What works (keep)

| Feature | Status | Details |
|---------|--------|---------|
| Stock DXVK 3.0.2 + Interop | Done | `ID3D9VkInteropDevice` / Texture → OpenVR Vulkan submit |
| Same-thread WaitGetPoses + Submit | Done | Extra pose thread → `AlreadySubmitted=108` / freeze — **do not repeat** |
| Mono image in headset | Done | Same BB to L+R; `errL=0 errR=0` |
| FP cam on ped (`FindPlayerPed` + CopyMat) | Done | Armed after ~360 submits; 4/4 call sites |
| HMD orient + 6DoF relative F9 | Done | Only **F9** recenter (no look toggles) |
| Eye-Forward ~0.38 | Done | Head/hair partially "see-through" without natives |
| Locomotion + stick yaw | Done | Stick-yaw sign negated (L/R was swapped) |
| HMD info log | Done | G2: recommended ~2836×2772, FOVh~94°, IPD~60 mm |
| BuildRenderList mid-AOB | Done | Finds fn despite FusionFix prologue patch |

### Known good AOB (DrawScene::BuildRenderList)

- Mid-body (survives FusionFix SafetyHook):  
  `83 BF 38 09 00 00 FF 0F 84 ? ? ? ? 6A 00 6A 0C`
- Function start = mid − **13**
- File-offset reference (exe): mid ~`0x527EDE` → start ~`0x527ED1`  
  Runtime example: `@ 00EAD200` (ASLR)

---

## Stereo modes (`gtaiv_dxvk_vr.stereo`)

| Mode | Meaning | Result (tested) |
|------|---------|-----------------|
| **0** | Off (default) | Mono submit, best FPS (~90) |
| 1 | Probe: log BuildRenderList | OK |
| 2 | Dual BuildRenderList, same cam | Stable; **does not draw twice** — lists only |
| 3 | Dual + IPD in CopyMat | IPD had effect; no real dual draw |
| 4 | Temporal L/R: frame L / frame R, StretchRect → 2 RTs, submit | **Real parallax** (user: cup) — fusion incomplete; **FPS ~46** |
| 5 | Phase probe (log order) | Order: **PhaseA → PhaseC → DrawScene** |
| 6 | Same-frame PhaseA→C→Draw L then R + BB submit | Stable, submit L=R=1; **no fusion**; ~50 FPS |
| 7 | Dual BuildRenderList + RT copy + cam IPD | Cam matrix L/R correct (logged up to 200 cm) — **BB still does not change** |
| 8 | Mode 7 + L/R submit swapped | ~45 FPS / worse — do not use |

### Hard finding (2026-07-24)

- F8 sep 0→200 cm: log `camDist=200cm`, user sees **no** image difference → capture is not a real dual draw.
- `BuildRenderList` (PhaseA/C/DrawScene) ≠ RenderView. Building lists 2× + StretchRect = often **the same** image.
- Mode 4 (temporal) had real parallax — there a **full frame** is drawn with the cam.
- VSConstF hooks → ~45 FPS, fusion no → turned **off** again.

**Mode 9 result (VTable, 2026-07-24):**

| Phase | BuildRenderList slot | Execute candidate (next slot) |
|-------|----------------------|----------------------------------|
| PhaseA | `[7]=00928AC0` | `[9]=0053B750` (`[8]`=Stub) |
| PhaseC | `[8]=00D76950` | `[9]=0053E300` |
| DrawScene | `[8]=00ADD200` | `[9]=0049C250` |

Mode 9 FPS hole: AOB on every phase call — fixed.  
**Mode 10:** Execute-only dual → L≠R, F8 sep had no effect. Build+Exec from ExecA = freeze — forbidden.  
**Mode 11:** Build+Exec from PhaseA build → **black screen** — forbidden.  
**Mode 13 spike:** temporal stereo + **native-FOV inset** Submit bounds (no D3D cover widen).  
Cover-crop without cover render caused zoom; Mode **0** = daily ~90 FPS. Mode 11 dead. Kill **0**.  
**Mode 14 — CONFIRMED WORKING (headset test 2026-07-24): FUSION + CORRECT PROPORTIONS.**
Temporal stereo + **angle-correct per-eye canvas**. Root cause of all previous diplopia:
nullptr-bounds Submit stretched the 16:9 game image over each eye's full asymmetric G2 frustum
→ per-eye different angular error → double vision despite correct parallax (mono fused because
both eyes shared the identical error). Mode 14 places the game image at its TRUE angular position
inside a larger black canvas per eye (one StretchRect, no engine FOV write, no Submit UV tricks).
Black border is EXPECTED (game covers ~100% h / ~53% v of the frustum). User: image correct,
performance good. Optional `gtaiv_dxvk_vr.fov` (horizontal degrees 30–150) calibrates world size.
Log: `StereoRender: mode 14 GeometryCanvas`, `StereoCanvas: L/R canvas=… rect=…`, `mode=14`.

**Key measurement:** `gameTan=(1.000,0.562)` at 16:9 → tanH = tanV × aspect exactly →
**GTA IV keeps vertical FOV fixed**; square resolution would SHRINK the image, not fill the
frustum. Black-bar fix must raise the SOURCE vertical FOV.

**Mode 15 — TESTED DEAD (2026-07-24):** SetTransform projection widen produced vertical stretch
→ **Rage ignores `D3DTS_PROJECTION` for its shader passes.** Black-bar fix must change the
engine FOV itself.

**Modes 16/17 (built 2026-07-24):** two-stage engine-FOV plan, both render like Mode 14:
- **16 CamFovProbe** — READ-ONLY: samples floats at mat−0x80..+0xFC around the CopyMat camera
  matrix over 240/2400 frames, logs stable FOV-plausible candidates
  (`FovProbe: off=… DEG/RAD …`). Target value ≈ 58.7 deg vertical (or ≈1.02 rad) at 16:9.
  Cannot freeze (no writes).
- **17 CamFovWrite** — writes `value × scale` at the verified offset, on the game thread right
  after CopyMat (NOT the old blind CCam+0x60 write). Config `gtaiv_dxvk_vr.fovpatch` =
  `"<offsetBytes> <scalePercent>"`. Guards: only writes when the current value is FOV-plausible;
  clamps at 130 deg / 2.27 rad. Log: `FovPatch: #n off=… old -> new`. Canvas adapts
  automatically (reads live projection). Kill: delete file / stereo=14/0.

**Mode 16 probe RESULT (2026-07-24):** all cam mats show a stable, exact **45.000 at mat+0x50
(off=+80)** and mat+0xD0 (+208) — GTA IV's camera FOV field (default 45°). Other candidates were
noise (at.z, player height). Note: rendered vertical FOV is 58.7° = 45 × ~1.304 (engine multiplier),
so `scale` in fovpatch maps ~1:1 onto the render FOV ratio.

**Regression found & fixed (2026-07-24):** trusting `m[1][1]` in `UpdateGameFovFromDevice`
picked up a SQUARE side-pass projection (mirror/envmap) → gameTan flapped 0.562↔1.0 → no bars,
wrong proportions, RT-recreate stutter in Mode 16. Reverted to backbuffer-aspect derivation
(also correct under the Mode 17 FOV patch).

**Mode 17 test 1 (2026-07-24):** bars smaller, image better — offset +80 CONFIRMED as the
engine FOV. But multiplicative per-call writes compounded (45→58.5→76→…112 across call sites)
→ frame-to-frame FOV oscillation = jitter + FPS loss. **Fixed:** baseline captured once per cam,
absolute idempotent target write (`FovPatch: baseline mat=… base=45.000`).

**Mode 17 test 2 (2026-07-24):** jitter gone (idempotent write works), but objects jumped
left/right and FPS 30–50. Cause: at scale 130 the game renders tanH≈1.4 > eye frustum (~1.157)
— GTA IV couples horizontal = vertical × aspect — and `CopyBbToEyeCanvas` clamped only the DEST
rect → per-eye horizontal squeeze → alternating displacement in temporal stereo. **Fixed:**
proper overlap mapping (crop SOURCE where game FOV exceeds the eye frustum, inset DEST where it
is smaller). Log now shows `dst=… src=…`.

**Mode 17 test 3 (2026-07-24):** still jumping at `80 115`. Log proof: +80 is a DERIVED,
animated value — game rewrote our 51.75 to 63.49 next call, later back to 45 → per-frame FOV
oscillation (each temporal eye rendered with different FOV). **New approach:** patch **+208**
(the second constant-45.000 field = likely the base/desired FOV the game derives +80 from),
plus a 30-frame stability gate on gameTan updates in `vr_display.cpp` (canvas rect can never
flap per-frame again, no matter what the engine does).

**Mode 17 test 4 (2026-07-24) — FOV patch CLOSED for now:** offset `+208` accepts the write
(single `FovPatch: #1 45.000 -> 51.750`, game does not fight it) but is **inert** — gameTan
stayed (1.000, 0.562), rendering unchanged. So: **+80 = live FOV but recomputed per frame by
the game (oscillates against writes), +208 = unused field.** A working engine-FOV lever needs
to hook the code that RECOMPUTES +80 (found via the render-root RE below). `fovpatch` file
deleted.

**KEY DIAGNOSIS (2026-07-24):** test 4 was geometrically identical to the confirmed-good
Mode 14 run (same rects, stable tangents, no FOV change) — and the user STILL saw objects
jumping left/right. → **The jumping is the inherent temporal alternating-eye artifact**, not a
bug: at 30–50 game FPS each eye updates at 15–25 Hz, one frame apart; any motion (driving,
walking, head turns) displaces the two eyes' images against each other. The first Mode 14 test
was calm/standing, so it went unnoticed. Luke Ross needs high FPS + own motion compensation
for exactly this reason. Mitigation: more FPS (lower resolution/settings). **Real fix: render
both eyes in the SAME frame.**

**Mode 18 — RenderRootProbe RESULT (2026-07-24):** all phase-hook callers found in seconds
(GTA IV CE 1.2.0.59, main module PlayGTAIV.exe = byte-identical copy of GTAIV.exe, base
0x400000, no ASLR). Static PE analysis pinned the three caller functions — **all thiscall
(this in ecx), NO stack args (plain `C3` ret)** → hookable AND re-callable with our standard
`BuildRenderList_t` pattern:
- **0x8F73B0** (len 0x1C2, prologue `53 55 56 57 8B F1`) — exec dispatcher: calls vt[9]
  Execute on ALL phases from two sites (0x8F7435/0x8F744A); ExecPhaseA ~9×/frame,
  ExecPhaseC/DrawScene 1×/frame from here. Also contains the BuildPhaseA call site 0x8F7FB3…
  region.
- **0x8F7F00** (len 0x10A, prologue `56 57 8B F9`) — PhaseA build root (1×/frame).
- **0xA87680** (len 0x392, prologue `83 EC 20 53 56 57 8B F9`) — loop that BUILDS
  (site 0xA8784B) and EXECUTES (site 0xA876AB) PhaseC + DrawScene, 1×/frame each.
Other DrawScene-exec sites: 0xAD37F3 (1×/frame), 0xA7C7BA (~17×/frame subpasses), 0xA876AB.

**Mode 19 — RootDispatchProbe RESULT (2026-07-24):** per gameplay frame, exactly:
`ExecRoot 1× → BuildRootA 1× → BuildExecCD 13×` (order string `012222222222222`). So
**BuildRootA (0x8F7F00) is the frame render root**: it walks all 13 render phases via an
indirect per-phase call (one site, ret 0x8F7FB3 — PhaseA build and the shared 0xA87680
build+exec wrapper both return there). Menu/loading does not use this path (counters 0).
Caller chain: BuildRootA is invoked from a tiny wrapper 0x5C2BF0 that does
`mov ecx, 0x118D7F0` (global render-phase manager = `this`) before the call; ExecRoot is
called from the adjacent function 0x5C2380 (len 0x866).

**Mode 20 — SameFrameRootDual (built 2026-07-24) — THE fix candidate for temporal jumping:**
hook BuildRootA and run the ENTIRE 13-phase frame build+draw twice per frame:
Left eye → `CopyBbToEyeCanvas(L)` → Right eye → `CopyBbToEyeCanvas(R)`, eye switch via
`RefreshLiveCamForStereoEye()` between passes. Both eyes share one game tick → the temporal
L/R jumping is impossible by construction. Costs ~2× render time (expect roughly half FPS).
Non-reentrant (the dual-call happens at top level after a full walk), unlike dead mode 11.
Log: `StereoRender: mode 20 SAME-FRAME ROOT dual`, `StereoRootDual: #n haveL=1 haveR=1`.
Known open question: the Left capture happens mid-frame (before post-process/UI of the
native pass) — per-eye post-processing mismatch is possible; headset test will show.
Kill: `gtaiv_dxvk_vr.stereo` = 14 (stable daily) or 0.

**Mode 20 RESULTS (2026-07-24):** v1 (build-only dual): jumping GONE, ~50 FPS, but NO fusion
— Rage double-buffers draw lists, the second build overwrote the first → both captures showed
the same image. v2 (build+ExecRoot per eye) and v3 (build+FrameA+ExecRoot per eye): CRASH —
build (thread A) and exec (thread B) run on DIFFERENT threads; calling exec-side code from the
build hook is a cross-thread violation. SEH guard added (falls back to native mono).

**Mode 21 — ViewMatrixScan RESULT (2026-07-24, user moved/drove during scan — hits held):**
the render-phase manager (0x118D7F0) holds **two live camera WORLD matrices**; their position
rows are at **0x118D8C0 / 0x118D9C0** (exe RVAs 0xD8D8C0/0xD8D9C0, matrix base = pos − 0x30).
No inverse/view-translation copies anywhere → Rage derives the view matrix at draw time from
these. Thread IDs confirmed: BuildRootA thread ≠ ExecRoot thread.

**Mode 22 — ExecViewDual RESULT (2026-07-24): DEAD.** ExecRoot run twice on the render thread
with the manager matrices shifted between passes → AV stage 5 (2nd ExecRoot call, fault
0x6E75E3): **ExecRoot CONSUMES the frame's draw lists — a second root call is use-after-free.**
Left capture from the backbuffer inside ExecRoot context worked fine (that part is reusable).

**Mode 23 — ExecViewPhaseDual (headset test 2026-07-24): STABLE, jumping FIXED, fusion still
missing.** Per-phase Execute ×2 (mode 10's crash-free path) + manager-matrix shift between
passes, captures via current-RT → angle-correct canvas. 3300+ dual frames, zero exceptions,
`haveL=1 haveR=1`, 45 FPS (exactly half of the user's stable 90 → the second pass REALLY
renders). But user sees "two separate pictures": near objects double → both passes almost
certainly render from the SAME camera. **Conclusion: the view transform is baked into the
draw lists / shader constants at BUILD time; the manager matrices are bookkeeping copies —
shifting them at exec time changes nothing.**

**Mode 23 headset test (2026-07-24):** confirmed by user — no jumping (same-frame works!),
45 FPS (= half of stable 90 → second pass really renders), but NO fusion ("two separate
pictures" = near objects double → both passes render from the same camera, images land at
infinity in the per-eye canvases).

**Mode 24 — ExecViewConstDual (BUILT + DEPLOYED, stereo file already set to 24, NOT yet
tested):** mode 23 + the missing camera change, done at the D3D level: `SetVertexShaderConstantF`
(device vt[94]) is hooked; during the RIGHT pass every uploaded 4x4 constant block that maps
the build cam position onto the view axis (|x'|,|y'| ≈ 0 with cancellation tolerance, zero
matrix rejected via z'/w') is treated as view/viewProj and gets the world-space eye delta
pre-multiplied (both row-vector and column-vector conventions handled). Zero overhead outside
the R pass (gate `StereoVsGetPatchParams`).
Log lines to check after the next headset test:
- `VsPatch: MATCH rowvec/colvec reg=… cnt=…` → view/viewProj registers found and translated.
- `StereoExecPhase: #n … vsPatch=N` → N grows = matrices are being patched per frame.
- `VsPatch: seen reg=… cnt=…` (first 40 uploads of R passes) → register layout intel if
  NOTHING matches (then the game likely uploads only combined World×ViewProj per draw —
  next lever: detect gWorld+gWorldViewProj pairs in one upload, or patch at DIP time).
Expected outcomes: fusion appears → DONE (same-frame stereo complete). No fusion +
`vsPatch=0` → constants are combined WVP; plan B above. Crash → exception log has stage/code
(same forensics as mode 22/23).
Mode 14 stays the stable fallback; mode 23 is the stable dual base (45 FPS, no jumping).

**Insight for full vertical fill later:** at 16:9 the horizontal hits the frustum limit long
before the vertical fills. The proper endgame is SQUARE aspect (`commandline` 1080×1080) +
engine FOV — Luke Ross' square+FOV combo now makes sense for us; canvas adapts automatically.

**New config toggles (all OFF by default, read once at start):**
- `gtaiv_dxvk_vr.eyefwd` (cm, default 38) — smaller = less camera swing on head turns (comfort).
- `gtaiv_dxvk_vr.movemode` = 1 — do not force ped heading; strafe/backpedal via game mapping.
- `gtaiv_dxvk_vr.vehfollow` = 1 (or 2 = inverted) — camera yaw follows vehicle heading, HMD
  free look on top; baseline on entry + F9.

See `docs/HANDOFF_GROK.md` for the remaining plan (HUD positions, aim-cam hook, third-person
toggle).

**Default in code:** Mode **0**. Mode 4/6/7 not for daily use.

---

## Stereo findings (important)

1. **Parallax ≠ fusion.** Mode 4 delivers different L/R angles, but images do not merge cleanly → double vision remains.
2. **`BuildRenderList` ≠ RenderView.** Calling twice + copying BB = often **the same** finished image. Not a BotW-equivalent dual draw.
3. **Temporal alternating-eye** (frame L, frame R):
   - Per eye ~half update rate → user saw **~46 FPS** (was ~90).
   - BotW deliberately rejects alternating; for us only a spike, not a goal.
4. **OpenVR IPD raw values are correct:**  
   `L=(-0.030,…)` `R=(+0.030,…)` sepX≈60 mm.
5. **IPD via cam basis (right/up/−forward) was wrong** — right eye got negative X + large Y.  
   Fix: head rotation × EyeToHead translation, then `OvrToGta`. After fix L/R opposite — fusion still not good (FOV/projection missing).
6. **HMD FOV → `CCam+0x60` (~94°)** → freeze/black screen after load — **do not repeat** without a new plan.
7. Missing **per-eye projection** (view offset only) is the most likely remaining cause of diplopia with correct parallax.

### Ovr→GTA (position/directions)

```
gx = ox;
gy = -oz;
gz = oy;
```

Rage cam matrix: `right`=X, `up`=Y=**Forward**, `at`=Z=**Up**.

---

## Explicitly failed / forbidden

| Attempt | Effect | Rule |
|---------|--------|------|
| Script natives from EndScene (`SET_DRAW_PLAYER_COMPONENT` / PedHide) | Crash | Natives only on script thread |
| FOV write ~94° at CCam+0x60 | Freeze after load | Do not repeat this way |
| OpenVR calls from CopyMat (live `GetEyeToHead`) | Freeze | Cached offsets only |
| Pose thread separate from submit | AlreadySubmitted / freeze | WaitGetPoses+Submit same thread |
| BuildRenderList prologue AOB | MISS (FusionFix JMP) | Use mid-body AOB |
| Mode 4 daily use | ~half FPS, fusion incomplete | Default 0; search for dual draw |

---

## Hotkeys / UX

- **F9** — SteamVR recenter + 6DoF baseline  
- Close SteamVR dashboard (otherwise `DoNotHaveFocus` / `errL`≠0)  
- Quick restart: `scripts/restart-gtaiv.*` (optional `-DirectExe` after RGLess)  
- Faster start/launcher: `docs/STARTUP_SPEED.md`  
- Inspiration (VC VR / C06alt): `docs/INSPIRATION_NOTES.md`  


- ntfy phone push on agent stop: topic `cursor-henning-atldv3m1iqosbzh2` @ `https://ntfy.sh`  
  Hook: `~/.cursor/hooks.json` → `hooks/ntfy-on-stop/run.cmd`

---

## Three BuildRenderList-like phases (CE exe, unique AOBs)

All share `cmp [edi+0x938], -1`, but different continuations:

| Label | File start | Mid AOB (unique) | Mid−start | Note |
|-------|------------|------------------|-----------|------|
| **DrawScene** | `0x6DC600` | `…FF 0F 84 DE 09 00 00 6A 00 6A 0C` | 13 | hooked so far |
| **PhaseA** | `0x527EC0` | `…FF 0F 84 76 03 00 00 80 3D` | 30 | large stack `0x6D8` → **SceneToGBuffer candidate** |
| **PhaseC** | `0x975D50` | `…FF 0F 84 77 02 00 00 8D 8F B0 00 00 00` | 39 | third phase |

RTTI strings in exe: `.?AVCRenderPhaseDrawScene@@`, `.?AVCRenderPhaseDeferredLighting_SceneToGBuffer@@`.  
Virtual calls (no direct `E8` to the fn) → dual draw needs correct `this` per phase.

**Mode 5** = phase probe (log only). **Tested 2026-07-23 — success.**

### Mode 5 result (log)

- All 3 hooks OK: DrawScene `@00ADD200`, PhaseA `@00928AC0`, PhaseC `@00D76950`
- Stable order per frame: **PhaseA → PhaseC → DrawScene**
- Stable `this` pointers: PhaseA=`13CA5E60`, PhaseC=`13CB6580`, DrawScene=`10F352D0`
- Occasional duplicate triplets in same `frameSeq` (likely 2nd viewport / reflection)
- `stereo` back to **0** afterward; Mode **6** built as next spike

### Mode 6 — Same-frame dual (new)

File `gtaiv_dxvk_vr.stereo` = **`6`**

Flow (per frame, after warmup of `this` pointers + eye RTs):

1. Natural: PhaseA + PhaseC with **Left** IPD  
2. DrawScene hook: DrawScene Left → StretchRect RT_L → PhaseA+C+DrawScene **Right** → RT_R  
3. EndScene: submit L/R when both RTs filled; otherwise mono fallback  

Kill switch: `0`. Log: `StereoSameFrame:`, `StereoSubmit: ... mode=6`.

**Risk:** BuildRenderList may not immediately fill the backbuffer → L/R may still look similar; then we need real GBuffer/draw execute. Freeze → immediately `0`.

### Mode 6 test (2026-07-23)

| | |
|--|--|
| Freeze | No |
| Fusion (single image) | No — still double vision |
| FPS | ~50 (Mono ~90) |
| Stability | OK enough to walk around |
| `stereo` afterward | back to **0** |

**Interpretation:** Pipeline runs (no crash), but StretchRect from **backbuffer** after BuildRenderList likely does not yield a real L/R pair (list build ≠ draw), and/or projection is missing. ~50 FPS = cost of double chain — expected.

## Sensible next steps

1. **Find GBuffer / execute** — not BB after BuildRenderList; RTs of deferred phase or real draw call.  
2. Per-eye **projection** (without old FOV-freeze path).  
3. Optional: Mode 6 for debug only; daily use = Mode **0**.

Stereo success criteria: parallax **and** fusion, acceptable FPS.

---

## Quick start checklist

1. SteamVR on, dashboard closed  
2. `gtaiv_dxvk_vr.stereo` = `0`  
3. Start GTA → log: `CamMatrix: 4/4 hooked`, `armed after 360`, `MonoSubmit … errL=0`, **no** `StereoTemporal`  
4. Expect FPS ~90  

Kill switch on freeze: kill game → set stereo file to `0` → restart.
