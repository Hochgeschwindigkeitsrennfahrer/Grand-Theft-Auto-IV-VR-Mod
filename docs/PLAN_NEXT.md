# Plan — where we are and what to do next (2026-07-24)

Read with `docs/CURRENT-STATE.md` and `docs/HANDOFF_GROK.md`.  
**No headset test in this step** — strategy only.

---

## 1. What the last tests proved

| Setting / build | World size | Fusion | Jump | Notes |
|-----------------|------------|--------|------|-------|
| Mode 26, scale 100%, IPD×scale | too big | near-perfect with F8 | mild | proportions OK after removing VS+CCam double |
| Mode 26, scale 150%, IPD×scale | **top** | **gone** | **violent** | effective eye sep ≈ 9 cm |
| Mode 26, scale 150% 6DoF only, IPD = HMD ~6 cm | too big again | jump less | better | size feel was mostly from **extra parallax**, not 6DoF |

**Conclusion:** F7 “WorldScale” was doing two jobs at once (L4D2-style). Cranking it for size also cranked stereo disparity → temporal stereo cannot fuse that. Decoupling was correct for fusion; size must be fixed by **another lever**.

---

## 2. Why the world still feels huge (even with “correct” IPD)

Three separate effects stack:

1. **Letterbox / window (Mode 14 canvas)**  
   Game FOV (~59° vertical) inside G2 (~94°+). Black bars top/bottom = you look through a smaller window. Brain often reads that as “giant world / drone / monitor on face”, even when stereo geometry is right.

2. **Temporal stereo**  
   L and R from different frames. Motion (look around) → smear. Large disparity → jump. Halo explicitly disables **motion blur** for the same “stereo echo” reason.

3. **No same-frame dual eye** yet  
   Halo MCC VR, L4D2VR, BotW BetterVR all render **both eyes in one tick** with per-eye pose/FOV. We still alternate frames (Mode 14/26). That is why jump/smear/“half FPS feel” remain.

IPD alone cannot fix (1). Scale×IPD “fixed” (1) by fake size cues and broke fusion.

---

## 3. What Halo-MCC-VR teaches us (relevant parts only)

Repo: https://github.com/pancreations/Halo-MCC-VR (OpenXR, D3D11 — different stack, same VR physics).

| Halo practice | Meaning for GTA IV |
|---------------|--------------------|
| **True per-eye stereo same-frame** | Our #1 remaining stereo bug is temporal. Same-frame is mandatory for “real 3D” without jump. |
| **Engine FOV ≈ 120** (user setting) | Their #1 display rule: wrong FOV = wrong scale + edge pop-in. For us: **raise Rage vertical FOV** (Mode 16/17 path), not square resolution. |
| **`resolution_scale` ≠ world/IPD** | Separate knobs. We must keep **F7 size** and **F8 IPD** separate (already started). |
| **Motion blur off** | Reduces stereo echo / smear. Check FusionFix / GTA blur settings. |
| **FSR off (breaks VR scale)** | Analog: don’t let post-process stretch fight the canvas. |
| Stereo proof toggle (F11 alternate) | They treat temporal L/R as a **dev check**, not the product. |

Do **not** copy their OpenXR/D3D11 code 1:1. Copy the **priority order**: FOV fill → same-frame stereo → HUD/input polish.

---

## 4. Recommended roadmap (one change per session)

### Phase A — Size without breaking fusion (next 1–2 sessions)

**A1. Soft stereo scale (new file, default ~115–125%)**  
- `gtaiv_dxvk_vr.stereoscale` independent of F7.  
- F7 stays 6DoF only (keep 150 if head-move feel is good).  
- F8 = base IPD cm.  
- Effective half-IPD = `0.5 * sep * stereoScale` with stereoScale capped (e.g. 1.0–1.25).  
- Goal: reclaim a bit of “size” without returning to 9 cm / fusion break.

**A2. Engine FOV (the Halo lesson — real size + kill bars)**  
- Mode 17 proved `+80` is live FOV but **recomputed every frame**.  
- Next: find the **recompute / projection-build site** (RE), patch once per frame there, or patch the source constant the recompute reads.  
- Success metric: `gameTan` vertical rises (log), black bars shrink, world feels less giant **with IPD still ~6 cm**.  
- Kill switch: stereo `14` / `26`, delete `fovpatch`.

**A3. Motion blur off (cheap)**  
- Confirm in FusionFix / graphics whether blur is on; disable for VR.  
- Reduces look-around smear even while still temporal.

### Phase B — Kill jump + smear + “half FPS” (main stereo milestone)

**B1. Same-frame on the render/replay thread**  
Evidence we already have:

- Draws + `SetVertexShaderConstantF` run on a **dedicated replay thread** (VsRet `0x2C73E`).  
- BuildRootA / ExecRoot are on the **game thread** — re-calling them does not redo real draws (`vsCallsR=0` in Modes 23–25).  
- Mode 23 proved **phase Execute ×2 is crash-free at ~45 FPS** but same camera.  
- Mode 26 proved **VS view-translate works** when armed on the native replay window.

**Target Mode 27 (design only until implemented):**  
Find/hook the **native replay draw walker** (caller chain around `0x2C73E`), run it **twice per recorded frame** with:

1. Left: CCam / constants as today  
2. Right: VS view-translate (identity-world V/VP — already proven) **or** manager-cam shift if we can prove it reaches that path  
3. Capture L/R canvases (Mode 14 geometry)  
4. Submit both  

Expect ~half GPU cost vs 90 FPS mono (acceptable — user already accepted 45–60).  
**Do not** re-open Mode 20/22 ExecRoot double (consume/crash) or Mode 24 phase stubs.

**B2. Desktop mirror**  
While temporal or during dual: show **left eye only** (or mono BB) on desktop so the monitor doesn’t L/R flash. Headset is source of truth.

### Phase C — After fusion + size + no jump (later)

- HUD inward (minimap / status) — not StretchRect of whole BB (breaks angles).  
- Free-move / vehicle follow / aim-cam FP lock (toggles already sketched).  
- Third-person hotkey.  
- OpenXR only if OpenVR path is solid (project rule: OpenVR first).

---

## 5. What we will *not* do next

- Stack VS patch + CCam IPD again (double separation).  
- Multiply F8 IPD by F7 WorldScale again at 150%.  
- Square `commandline` resolution without FOV fix (shrinks image).  
- `D3DTS_PROJECTION` widen (Mode 15 dead).  
- Blind `CCam+0x60` FOV write (historic freeze).  
- Same-frame via BuildRootA×2 / ExecRoot×2 without new evidence.

---

## 6. Suggested next session (pick ONE)

**Best next test (highest leverage for “world too big” + bars):**  
→ **A2 probe:** locate FOV recompute site (read-only logging first), then one safe write mode. Halo’s success is literally “FOV must be wide enough”.

**Best next test (highest leverage for jump/smear):**  
→ **B1 Mode 27:** hook replay-thread draw walker, dual pass + VS right eye, Mode 14 canvas submit.

**Quick comfort while planning B1:**  
→ **A3** motion blur off + optional **A1** stereoscale 1.15 (small, measurable).

Recommended order: **A3 (minutes) → A1 if size still wrong → A2 FOV → B1 same-frame**.

### Applied 2026-07-24 (this session — awaiting headset)

1. **FusionFix.cfg:** `MotionBlur = 0`, `FieldOfView = 5` (+25° via FusionFix’s additive `CCam+0x60` — not our old absolute 94° freeze).
2. **StereoScale** file/hotkey F6 (default 115%, cap 130%) — soft disparity separate from F7.
3. **VsParent** stack log in Mode 26 — finds caller of VS wrapper for Mode 27.
4. Shader repo documented: https://github.com/Parallellines0451/GTAIV.EFLC.FusionShaders

---

## 7. Current knobs (for the user)

| File / key | Role now |
|------------|----------|
| `gtaiv_dxvk_vr.stereo` = `26` | Temporal Mode-14 camera + canvas submit |
| `gtaiv_dxvk_vr.scale` = `150` | 6DoF head move only (not IPD) |
| `gtaiv_dxvk_vr.ipd` = `6` | Stereo separation cm (F8) |
| F7 | Cycle world/6DoF scale |
| F8 | Cycle IPD cm |
| Kill | stereo `14` or `0` |

Baseline to protect: **Mode 14 geometry canvas** (fusion geometry). Mode 26 is Mode 14 + soft-start + submit wiring.
