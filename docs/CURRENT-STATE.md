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

**Currently deployed:** `gtaiv_dxvk_vr.stereo` = **`0`** (Mono, ~90 FPS target).

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
**Mode 7** = performance baseline (~90 FPS, normal FOV) — cover FOV/bounds **off** again (felt like a broken FOV slider).  
Mode 13 temporal remains experimental (~½ FPS). Mode 11 dead. Kill **0**.

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
