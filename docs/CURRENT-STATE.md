# CURRENT-STATE — gtaiv-dxvk-vr

**As of:** 2026-07-24 ~19:45
**Headset test now:** stereo **`37`** (pair-hold + true-FOV canvas) + **`fovadd=18`**.
Mode 39 was rejected: its 12° cap brought the black bars back but did not improve jump/FPS.
**Kill:** stereo **`38`** (AER pose + full `fovadd`), **`37`** (no pose, full FOV), or
**`30`** + delete **`fovadd`**. Protected: **`36`/`35`**.
Keep **`ipd`≥1**, canvas **zoom DISABLED**. F7 = 6DoF only (does **not** create “inside VR” alone).  
**Expectation:** the bars should be gone again. Mode 37 cannot remove the severe temporal jump or
restore mono-level FPS: L/R are still different game frames. A real fix needs same-frame distinct
eye rendering; independent head-owned view remains a later, risky camera/collision spike. AER v2
optical-flow is **not** implemented.

### Session 2026-07-24 ~19:45 (reject Mode 39 / restore Mode 37)

**User feedback:** black bars returned in Mode 39; performance remained poor; jumping remained strong.
The existing live log proves the configuration: `StereoMode: 39`, `StereoSubmit ... mode=39`, and
`FovSite ... add=12` with requested `fovadd=18`. Mode 39 therefore reduced the true engine FOV
that removed the bars, while leaving the temporal pair-hold schedule intact.

**Deployed configuration:** changed `gtaiv_dxvk_vr.stereo` from **39** to **37**; kept
`fovadd=18`, IPD/scale files untouched, CanvasZoom off, and no forbidden hooks. Mode 37 uses the
locked 1536 canvas and pair-latched true FOV, so it is the honest no-bars baseline. **Fresh log
proof** (`ASI_BUILD_ID 20260724-192011`): `StereoMode: 37`, Mode 37 `ok=1 fovSite=1`,
`Config: fovadd=18`, 1536×1536 submit+hold RTs, promoted L/R pairs, and
`StereoSubmit ... mode=37`. Steam launched more slowly than the restart script's process-wait
window, but the new ASI session did start and logged cleanly.

**Research conclusion:** L4D2VR/HL2VR own an engine `RenderView` and render both eye views within
one simulation tick. UEVR similarly treats alternating/AFR as a last resort because game time
advances between eyes. Their transferable requirements (one pose epoch, coherent per-eye
view/projection/texture geometry, stable RTs) are already applied where Rage permits. The missing
Rage same-frame render seam—not another pose-submit/FOV cap—is the root work before an independent
VR camera.

### Session 2026-07-24 ~19:30 (Mode 39 low-motion FOV comfort)

**User:** Mode 38 logged `Submit_TextureWithPose` success (`err=0`) but still jumped severely.
**Root-cause decision:** do **not** repeat another compositor reprojection variation: successful Mode 38 proves the API path runs, while temporal L/R remains one rendered frame apart. Mode 36/37's wider true FOV also correlated with lower FPS and stronger jump.
**Shipped Mode 39:** Mode 37's fused, pair-held, true-FOV canvas path with the same locked 1536 canvas, but the existing `fovadd` is capped to **12°** (file can remain `18`; log records requested/effective cap). No AER pose submit, no CanvasZoom, no IPD×WorldScale, no new render hooks.
**Expectation:** a safer comfort/FPS experiment, not a temporal-stereo cure. If no clear improvement, return to Mode 30 and stop spending experiments on temporal reprojection; same-frame remains the only root fix.
**Deploy verified (`ASI_BUILD_ID 20260724-192011`):** `StereoMode: 39`; `StereoRender: mode 39 LOW-MOTION COMFORT … ok=1 fovSite=1`; `Mode39: fovadd requested=18 capped=12`; `FovSite` writes `45.152 → 57.152 (add=12)`; pair promotion and `StereoSubmit: L=1 R=1 mode=39` are live.
**Kill:** write `38` to restore AER pose/full FOV, `37` for full FOV without pose, or `30` then delete `gtaiv_dxvk_vr.fovadd` for the smoother protected baseline.

### Session 2026-07-24 ~19:10 (Mode 38 AER pose submit)

**User:** Mode 37 still huge/monitor, jump, bad FPS; F7 doesn’t feel like being in the world; read Luke AER v2 Patreon.  
**Takeaway:** AER v2 = optical-flow intermediates (skip). Transferable = stamp each eye with capture-time pose (`Submit_TextureWithPose`, OpenVR #1253).  
**Shipped Mode 38:** Mode 37 path + pose cache on L/R capture + pose promote with pair-hold + `Submit_TextureWithPose`. Fallback to `Submit_Default` if pose missing/err.  
**Deploy verified (`ASI_BUILD_ID 20260724-191333`):** `StereoMode: 38`, `mode 38 AER-POSE SUBMIT … ok=1`, pair promote `poseL=1 poseR=1`, `AERPose: eye=L/R submitWithPose=1 err=0`, `StereoSubmit … mode=38`. Kill **37** / **30**.  

### Session 2026-07-24 ~18:45–19:00 (Mode 37 comfort — Mode36 bars-gone + Mode30 smooth)

**User feedback (Mode 36):** bars completely gone (keep); still screen-on-face; **FPS much worse**; **jump/jitter very strong** (Mode 30 was smooth); world **too close/huge**. Continue.  
**Diagnose:** Mode 36 pair-hold path was intact. Jump/FPS from (1) fovadd=22 wide engine FOV + temporal edge disparity, (2) FOV wobble recreating 4 eye RTs every few frames, (3) scale=50 = huge 6DoF feel.  
**Shipped Mode 37:** same true-FOV canvas + Mode30 pair-hold; **fovadd=18**; canvas **maxDim=1536 LOCKED**; pair-latched rect tangents; publish gate ~2.5%; **WorldScale→100** (one size lever; IPD untouched). No CanvasZoom / no IPD×scale / no forbidden hooks.  
**Deferred:** independent VR cam; Mode 34 dual.

### Session 2026-07-24 ~18:25–18:45 (Mode 36 true-canvas FOV — fill HMD / kill bars)

**User feedback (Mode 35):** better presence / less warp, still screen-on-face + black bars; protect as baseline; push harder.  
**Root cause:** engine `fovadd` wrote CCam+0x60, but canvas still used stale `D3DTS_PROJECTION` tangents (Rage ignores that transform — Mode 15 dead) → bars stayed.  
**Shipped Mode 36:** Mode 35 pair-hold + FOV site + `PublishGameFovFromCCamDegrees` (Mode 16 factor 58.7/45) → canvas/rect track TRUE FOV. No CanvasZoom.  
**Also fixed:** `ParseModeFile` capped at 35 (file `36` became mode 3!); FOV ADD idempotent (no 67→89 compound); ADD only when base FOV ≤55° (skip cutscene/menu spikes).  
**Deployed:** stereo=`36`, fovadd=`22` (toward G2 ~94°; vertical fill ~90%). Mode **35** unchanged as kill/protect.  
**Headset check:** smaller/no bars? Still fused? No look-warp? If bad → stereo **35** or **30**. Try fovadd **18–25** if needed.

**Deferred:** independent VR cam; Mode 34 dual; soft TextureBounds fill; theater quad.

### Session 2026-07-24 ~18:10–18:25 (Mode 35 FOV recompute — research + spike)

**User ask:** feel inside the game with real 6DoF; Mode 30 fusion OK but warping + screen-on-face.  
**Research:** Luke Ross / Halo / L4D2 / UEVR / FusionFix — kill monitor-on-face with **true engine FOV**, not canvas zoom / theater. FusionFix `fixes.ixx` = cam process CALL then `*(CCam+0x60) += n*5` = our Mode 17 recompute site.  
**Shipped Mode 35:** Mode 30 pair-hold + chain-hook that CALL; `gtaiv_dxvk_vr.fovadd` ADD degrees (deployed **15**). Additive preserves aim zoom.  
**Log proof:** 45→60 stable; thousands of FovSite calls; submits mode=35; game stayed up.  
**Headset check:** less black-bar / monitor feel? No look-warp? If warp/bad → kill to 30 + delete fovadd. Keep FusionFix menu FOV at **0**.

**Deferred:** independent VR cam vs wall collision; Mode 34 dual; theater quad.

### Session 2026-07-24 ~17:50–18:05 (kill canvas zoom + research)

**Headset feedback:** `zoom=85` claimed-FOV warp — every head move warps environment (same class as FusionFix FOV look-up warp).  
**Shipped:**
- Canvas zoom **forced OFF** in `vr_display.cpp` (always true FOV; file ignored). Game `zoom=100`.
- Mode **30** + **`ipd=1`** redeploy (preserve scale/stereoscale/eyefwd/pedhide).
- Read-only `VsRetStatic:` PE scan E8→`0x2C73E` on Mode 30 arm — live **`callSites=0`** (indirect callers only; dual stays off).
- Docs: REFERENCES (OpenVR/Meta/parallel proj/Luke Ross), INSPIRATION_NOTES (R.E.A.L. pack), PLAN_NEXT (deferred independent cam).

**Opinion on independent VR cam:** **Agree — defer.** Game cam collision/pitch clamps fight HMD look near walls; Luke Ross uses pitch overrides + render-time view fixes. Invasive; not this session.

### Session 2026-07-24 ~17:30–17:40 (canvas zoom — REJECTED)

**Problem:** Mode 30 felt **too zoomed in** (world size / window feel). Not a 6DoF WorldScale issue (`scale=50` = less head-move only).

**Lever tried:** soft **`gtaiv_dxvk_vr.zoom`** on Mode 14/30 angle-correct canvas only.
- Multiplied claimed game half-tangents by `100/zoom%` inside `GetGameFovTangents`.
- Deployed **`zoom=85`** → warp on head move → **KILLED** next session.

**Preserved after kill:** stereo=30, ipd=1, scale=50, stereoscale=125, eyefwd=20, pedhide=1.

### Session 2026-07-24 ~17:15–17:25 (consolidate: Mode34 dual dead → Mode30)

**Live log proof (buildId `20260724-171146`, stereo was 34):**
- `Mode34: DUAL armed fnRva=0x4DDAD0 avgEntries=2.00`
- Thousands of `Mode34: SAME-FRAME … vsPatch=0 vsCallsR=0 sep=7cm fnRva=0x4DDAD0`
- StereoDiff gate did **not** fall back (canvas noise ≥200). Half-armed dual = same-cam, no 3D lever.

**Shipped harden:**
- Forbid `0x4DDAD0`. COUNT may still probe; **never arm Dual**. COUNT-ok → write stereo=**30**.
- Dual safety net: `vsPatch=0` after dualN≥5 → fallback (if old ASI left Dual on).
- Deployed knobs: stereo=**30**, ipd=**1**, pedhide=**1**, eyefwd=**42**, scale/stereoscale preserved.
- F8 prime + early `ReloadIpdScale` (before CamMatrix arm) — stops sticky-F8 overwriting ipd file (6→7).

### Session 2026-07-24 ~16:55–17:10 (Inspiration FP + PedHide)

**Inspiration read** (`inspiration/firstperson mod/`):
- ini: `FPX/FPY/FPZ=0`, `HMD=1`, FOV=`111` (not adopted — look-up warp), phone/sens ignored
- ASI: ScriptHook `NativeThread` + `SET_DRAW_PLAYER_COMPONENT` for head/hair hide

**Shipped:**
- **PedHide ON** via CE `SetDrawPlayerComponent` helper (unique AOB) — same effect as Inspiration, **no** EndScene natives / ScriptHook. Components 0/7/9/10.
- Optional `gtaiv_dxvk_vr.camoff` = `x y z` cm (FPX/FPY/FPZ style). Absent = 0.
- Kept Mode **30** + `eyefwd=42`. Do not touch FusionFix FOV.

**Log proof (buildId `20260724-170626`):**
- `Config: PedHide=ON (gtaiv_dxvk_vr.pedhide)`
- `PedHide: SetDrawPlayerComponent @ … (Inspiration-style, NO natives/ScriptHook)`
- `PedHide: head/hair/teeth/face off via SetDraw# 1 ok=4 dead=0` … later `ok=404 dead=0`
- `StereoSubmit: L=1 R=1 mode=30` + `eyeFwd=42cm` — game stayed up

**Headset check for user:** hair/face gone in FP? If yes, try `eyefwd` **12–20** (less turn-swing). If crash/weird → `pedhide=0`.

### Session 2026-07-24 ~16:25–16:45 (comfort + Mode 34)

**Shipped for headset:**
- F8 floor **1 cm**; file still allows 0–500; warn if file=0.
- `eyefwd=42` (default/file) — cam past skull/hair. `PedHide: OFF` (natives from EndScene/CopyMat crash CE).
- Mode **30** + `sep=1cm` + `eyeFwd=42cm` live (buildId `20260724-164438`).

Log quotes (playable):
- `StereoSep: 1 cm (file gtaiv_dxvk_vr.ipd) — F8 cycles 1,2,3,4,6,7,10`
- `Config: eyeForward=42 cm … PedHide: OFF …`
- `CamMatrix: FP lock #1 … sep=1cm eyeFwd=42cm`
- `StereoSubmit: L=1 R=1 mode=30`

**Mode 34 RESULT (buildId `20260724-164125`):**  
Goal: when HookSetVSConstF ret==`0x2C73E`, resolve stack → **function starts**, CC-pad thiscall0, count ~1×/frame, dual+pair-hold. Fail → stereo=30.  
- EndScene brace bug (Mode34 nested under Mode33) fixed mid-session.
- `Mode34: DISCOVER armed … rareAgg=24 sampleHits=1392 vsRetHits=276`
- Rare starts with avg≈1.0 e.g. `0x52E7C0`, `0x5303D0`, `0x4DDAD0`
- `Mode34: COUNT exeRva=0x1BF010` (thiscall-frame) → **process hard-kill** (no SEH FALLBACK write)
- Hardened after: forbid `0x1BF010`, reject `thiscall-frame`, prefer avg∈[0.8,2.5] closest to 1.0
- Dual **not armed**. Next Mode34 try only with hardened filter; kill → **30/26**.

### “No 3D” on Mode 30 — ROOT CAUSE (2026-07-24 ~16:00)

Pair-hold was **not** broken: L/R eye flip + `StereoPairHold: promoted pair` + `StereoSubmit L=1 R=1 mode=30` all healthy.  
Live log had **`sep=0cm`** because `gtaiv_dxvk_vr.ipd` = **`0`** (F8 preset included 0).  
CamMatrix with ipd=0 → `ipd=(±0.000…)` → flat image, comfort intact.  
**Fix:** restored IPD; F8 presets no longer include 0 (now start at **1**). Log warn if file=0.

### CE vs version downgrade (verdict)

**Stay on CE.** Classic/1.0.7–1.0.8 + VorpX/C06alt FP offsets would not give us a replay-thread draw walker: those mods are cam/FP/ScriptHook (and often SBS cinema), not Rage dual-draw. Downgrade would invalidate FusionFix + all CE AOBs/RVAs we already mapped (Mode 18–26), force a parallel glue rewrite, and still leave the same “find the VsRet walker” problem on a different binary. Singleplayer/offline OK, but not worth a dedicated spike unless CE same-frame is abandoned. **Full steam ahead on CE.**

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
| Stereo kill switch | `...\GTAIV\gtaiv_dxvk_vr.stereo` → **`30`** playable / **`26`** baseline / **`0`** mono |
| IPD / scale / stereoscale | `gtaiv_dxvk_vr.ipd` / `.scale` / `.stereoscale` — preserved across mode probes; **ipd ≥1 for 3D** (F8: 1,2,3,4,6,7,10) |
| Eye-forward | `gtaiv_dxvk_vr.eyefwd` — default **42** cm; live **20** with PedHide |
| PedHide | `gtaiv_dxvk_vr.pedhide` — **`1`** ON (Inspiration SetDraw) / **`0`** OFF |
| camoff | `gtaiv_dxvk_vr.camoff` — optional `x y z` cm (Inspiration FPX/FPY/FPZ) |
| Canvas zoom | **DISABLED** (warp) — file ignored; was `gtaiv_dxvk_vr.zoom` |
| Build | `.\scripts\build-asi.ps1` → `out-asi\gtaiv_dxvk_vr.asi` |
| Deploy + start | `.\scripts\build-deploy-run.ps1` (Build→Kill→Deploy→Steam start→close dashboard) |
| Deploy only | `.\scripts\deploy-asi.ps1 -GameDir "<GTAIV>"` (`-Launch` starts the game) |

**Currently deployed:** stereo=**`30`**, ipd=**`1`**, eyefwd=**`20`**, pedhide=**`1`**, zoom=**OFF**.  
**Mode 30 (2026-07-24):** PAIR-HOLD = Mode 14/26 CCam temporal, but submit textures  
update **only when a complete L→R pair is ready** (both eyes promote together).  
User: jumps **definitely less**, barely noticeable — keep as playable. With ipd≥1: try fusion.  
Log: `StereoPairHold: promoted pair #N`, `StereoSubmit … mode=30`, `CanvasZoom: DISABLED`, `sep=1cm`, `eyeFwd=20cm`.

**Mode 34 RESULT (2026-07-24) — dual armed then DEAD for parallax:**  
VsRet(`0x2C73E`) stack → fn starts; DISCOVER armed; COUNT `0x4DDAD0` avg=2.0 → DUAL armed;  
live `vsPatch=0` / `vsCallsR=0` for 40k+ frames (no parallax). Earlier COUNT `0x1BF010` hard-kill.  
**Hardened:** forbid `0x4DDAD0` + `0x1BF010`; **never arm Dual** (COUNT-ok → stereo=30). Kill → **30** or **26**.

**Mode 33 RESULT (2026-07-24) — wait worked, dual not armed:**  
Goal: WAIT until live VsParent samples (Mode32 Soft was empty), then only CC-pad  
zero-arg thiscall → count → dual+VS. Never `0x2C6AC`/`0x37BD0`/`0x32A40`/`0x372B0`.  
Fail → write stereo=30.

Log quotes (buildId `20260724-161039`):
- `StereoSep: 6 cm (file …)` / `mode 33 … ok=1`
- `Mode33: still waiting … es=60 rareAgg=0` (correct — no early seed)
- `Mode33: VsParent sample#1 slot=10 exeRva=0x22187` (+ `0x37520`/`0x32BD7`/`0x4D901B`)
- `Mode33: DISCOVER armed (live VsParent appeared; rareAgg=4 sampleHits=8; no early seed)`
- Mid bytes prove **epilogues**, not walkers: `0x22187=5F C2 10 00` (pop/ret 0x10),  
  `0x32BD7→0x32A40` REJECT forbidden stackarg, `0x37520→0x372B0` REJECT forbidden
- `Mode33: COUNT exeRva=0x4D8F10` → `avgEntries=0.31` REJECT
- `Mode33: FALLBACK pair-hold — wrote stereo=30`
- After: `StereoSubmit: L=1 R=1 mode=30` (alive). F8 no longer cycles to 0.

**Hard finding:** VsParent slots 10–32 are **return addresses inside callees** (epilogues /  
mid-instr), not ~1×/frame thiscall0 draw walkers. Next same-frame spike must find the  
**caller** that drives VsRet ~1×/frame (static E8 / deeper stack), not re-hook these mids.

**Mode 32 RESULT (earlier) — safe fallback, dual not armed:**  
Soft ran before VsRet traffic → empty hist → early seed of stackarg. Mode 33 fixed the wait.

**Next RVA / ABI (do NOT blind-hook):**

| RVA | ABI | Notes |
|-----|-----|--------|
| `0x2C73E` | VsRet | Upload site; not a walker |
| `0x22187` | mid epilogue `5F C2 10 00` | VsParent slot — not a start |
| `0x37520` / `0x32BD7` / `0x4D901B` | mid | VsParent slots — resolve to stackarg or none |
| `0x370D0` | stdcall `56 57 8B 7C` | Helper — no dual |
| `0x32A40` | thiscall+stackarg | `[ebp+8]`, ret 4 — **FORBIDDEN** Mode33 |
| `0x372B0` | thiscall+stackarg | CC-pad but stackarg — **FORBIDDEN** Mode33 |
| `0x4D8F10` | thiscall-83EC | Safe; count avg≈0.3 (not ~1×/frame) |
| `0x1BF010` | thiscall-frame | Mode34 COUNT **hard-kill** — **FORBIDDEN** |
| `0x4DDAD0` | thiscall-56 | Mode34 COUNT ok + DUAL `vsPatch=0` — **FORBIDDEN** (no parallax) |
| `0x52E7C0` / `0x5303D0` | ~1×/frame starts | Mode34 rare (0x52E7C0 stackarg forbidden; others untested) |
| `0x37BD0` / `0x2C6AC` | | **FORBIDDEN** |

**Next session:** headset-check Mode **30** + **ipd=1** + zoom OFF (no warp?). Stay on **30**.  
Do not re-arm Mode34 dual. Use `VsRetStatic` safe RVAs for next same-frame COUNT-only probe.  
Playable stays **`30`** + **`ipd=1`** + **`eyefwd=20`** + **`pedhide=1`**. Kill stereo → **`26`**.

**Mode 31:** discover failed (`frameSlots=0`, callee too deep). Superseded by Mode 32/33.

**Mode 30 dual probe (earlier) — DEAD for parallax:**  
device-VS dual `deviceVsPatches=0`. Do not re-arm without replay-thread evidence.

**Mode 29 FAILED (2026-07-24):** `FindFnStartNear(Cand37C01)` → `0x37BD0` crash.  
Modes **27/29** alias Mode 26. **Mode 28:** wrap@0x2C6AC crashed — aliases 26.  
**SteamVR dashboard:** `restart-gtaiv.ps1` closes it after GTAIV is up (default).  
**Quick restart (pinable):** `scripts\install-restart-shortcut.ps1`.

---

## What works (keep)

| Feature | Status | Details |
|---------|--------|---------|
| Stock DXVK 3.0.2 + Interop | Done | `ID3D9VkInteropDevice` / Texture → OpenVR Vulkan submit |
| Same-thread WaitGetPoses + Submit | Done | Extra pose thread → `AlreadySubmitted=108` / freeze — **do not repeat** |
| Mono image in headset | Done | Same BB to L+R; `errL=0 errR=0` |
| FP cam on ped (`FindPlayerPed` + CopyMat) | Done | Armed after ~360 submits; 4/4 call sites |
| HMD orient + 6DoF relative F9 | Done | Only **F9** recenter (no look toggles) |
| Eye-Forward ~0.42 | Done | Default/file 42 cm; with PedHide try 12–20 |
| PedHide (Inspiration) | **ON (test)** | CE SetDraw helper AOB — NOT EndScene natives. Kill: `pedhide=0` |
| camoff FPX/Y/Z | Optional | `gtaiv_dxvk_vr.camoff` = `x y z` cm |
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

**Mode 24 — ExecViewConstDual (DEAD for fusion):** phase re-exec never sees the real draws
(`vsCallsR=0`). All VS-constant uploads go through one Rage wrapper (exeRva `0x2C73E`) on a
**dedicated render/replay thread**, while BuildRootA + ExecRoot share the game thread.

**Mode 25 — BuildDualViewShift (DEAD for fusion):** BuildRootA ×2 + manager matrix shift +
VS patch during walk 2. Stable, but `vsCallsR≈1` — same wrong-thread problem.

**Mode 26 — RecordDualReplayShift:**
- Submit fix: `openvr_mono` always calls `StereoTrySubmitEyes` (was stale mode list).
- Headset after submit: fusion YES, jump NO, 90 FPS — but "monitor on face", wrong world
  size, black bars. Cause: VS-only shifted identity-world draws + stale `scale=50`.
- Now: Mode 14 CCam temporal only (VS stack removed — doubled IPD on static geo).
  Entering 14/26 resets scale=150% + HMD IPD. IPD is NOT multiplied by WorldScale
  anymore (150%×IPD broke fusion + violent jump). F7 = 6DoF size only; F8 = stereo cm.
  Black bars = canvas. Desktop L/R jump + look-smear = temporal (same-frame next).
**Kill-switch baseline:** `gtaiv_dxvk_vr.stereo` = **`26`**. Kill → `14` or `0`.

**Mode 30 — PhaseDualDeviceVs → PAIR-HOLD (2026-07-24):**
- Tried: Mode 23 dual + device Get/SetVSConstF before R (safer than prologue hooks).
- Result: dual stable but `deviceVsPatches=0` — no parallax lever at exec.
- Shipped: CCam temporal + **pair-hold** (promote L+R submit textures together).
- User feedback: jump barely noticeable — **keep as playable**. Needs **`ipd≥1`** (was 0 → no 3D).
**Currently deployed:** stereo = **`30`**, ipd = **`1`**, pedhide = **`1`**. Kill → **`26`**.

**Mode 31 — SameFrameReplayDual (2026-07-24):** discover failed (stack too deep).  
Superseded by Mode 32/33. Kill → **`30`** or **`26`**.

**Mode 32 — SameFrameVsParentDual (2026-07-24):** HookSetVSConstF-depth VsParent +  
ABI classify + count gate. Soft empty → early seed. Dual not armed; wrote stereo=30.  
Kill → **`30`** or **`26`**.

**Mode 33 — SameFrameLateVsParentDual (2026-07-24):** WAIT for live VsParent samples,  
then CC-pad thiscall0 only. Wait **worked**; mids are epilogues not walkers;  
`0x4D8F10` count avg=0.31. Dual not armed; wrote stereo=30. See header.  
Kill → **`30`** or **`26`**. Next: find ~1×/frame **caller** of VsRet (not mid slots).

**Comfort pack / Mode 27 (2026-07-24):** Best 3D so far = Mode 26 + user IPD. F6 subtle.
Jitter = temporal. Mode 27 ReplayRootProbe: Cand37BD0 wrong ABI (exception), Cand4D8F85
never called; VS wrapper `0x2C6AC` has **no static E8 xrefs** (indirect only). Back on
**Mode 26**; FusionFix `MotionBlur=0` + `FieldOfView=7` (+35°); **IPD/scale files no longer
overwritten** on mode enter. Next: Mode 28 dynamic caller of `0x2C6AC` for same-frame.

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
| Script natives from EndScene (`SET_DRAW_PLAYER_COMPONENT` via NativeContext) | Crash | Use CE SetDraw helper AOB (`pedhide`) instead |
| FOV write ~94° at CCam+0x60 | Freeze after load | Do not repeat this way |
| OpenVR calls from CopyMat (live `GetEyeToHead`) | Freeze | Cached offsets only |
| Pose thread separate from submit | AlreadySubmitted / freeze | WaitGetPoses+Submit same thread |
| BuildRenderList prologue AOB | MISS (FusionFix JMP) | Use mid-body AOB |
| Mode 4 daily use | ~half FPS, fusion incomplete | Default 0; search for dual draw |

---

## Hotkeys / UX

- **F9** — SteamVR recenter + 6DoF baseline  
- SteamVR dashboard auto-closes on restart / after OpenVR init (manual close still OK)  
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
