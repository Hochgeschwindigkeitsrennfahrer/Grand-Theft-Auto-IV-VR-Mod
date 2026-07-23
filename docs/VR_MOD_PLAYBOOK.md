# VR Mod Playbook — Orientation for GTA IV CE

Goal: reasonable VR display like successful flat→VR mods.  
**Reference model: BotW BetterVR** (third-person game → true stereo VR).

**Current operating status / restrictions / modes:** → [`CURRENT-STATE.md`](CURRENT-STATE.md)

---

## What the reference mods actually do

| Mod | Starting point | Core trick | Relevant for us |
|-----|---------|------------|------------------|
| **BotW BetterVR** | Third-person (Cemu) | **Render loop 2×** before game state ticks; cam L/R + FOV via patch; capture → compositor | **Most important reference** |
| **L4D2VR** | First-person (Source) | Hook `RenderView` → RT left/right, origin ± IPD, FOV/aspect, submit L/R | Stereo pipeline pattern |
| **HL2 VR Mod** | First-person (Source) | Engine VR / dual-eye + motion controls | Comfort/input |
| **SubmersedVR** | First-person (Unity) | Unity stereo / HUD→world | HUD ideas; not a DXVK reference |
| **Vice City VR** (2026) | reVC + librw D3D12 | **Single-pass / double-wide stereo** + OpenXR; shared visibility/prep; menu=theater quad; HUD=separate layer | Architecture reference (not ASI-portable) — see `INSPIRATION_NOTES.md` |
| **C06alt IV FP** | ScriptHook ASI | FP cam / offsets / possibly mesh | Head hide on script thread; stereo unlikely |

Common denominator:

1. **True stereo** = scene **drawn twice** (alternating-eye is not the goal).  
2. **Cam per eye**: pose + EyeToHead (IPD) + **HMD projection**.  
3. **Game logic 1×** per stereo pair.  
4. Capture/submit separate from tick.  
5. Hide avatar head or use third-person body.

Mono submit = bootstrap, not the end state.

---

## BotW → GTA IV mapping

```
BotW                                      GTA IV CE (target)
─────────────────────────────────────────────────────────────
render twice / tick once               →  real world draw 2× (not just BuildRenderList)
Cam + FOV per eye                      →  CopyMat + IPD + projection (FOV write previously caused freeze)
Capture → Compositor                   →  DXVK interop → OpenVR submit L/R
```

### Do not copy
Cemu/PowerPC, Vulkan layer-in-layer, D3D12 shim.  
Stack stays: **Stock DXVK 3.0.2 + ASI + OpenVR**.

---

## What we have already tried (as of 2026-07-23)

| Approach | Result |
|--------|----------|
| Dual `BuildRenderList` | Hook OK (Mid-AOB). Builds lists 2×, **does not draw world 2×**. |
| Mode 4 temporal (frame L / frame R + StretchRect + submit) | **Parallax yes** (user verified). Fusion no. FPS roughly halved (~90→~46). |
| IPD via cam basis right/up | **Wrong** (right eye negative X). |
| IPD = R_head × EyeToHead, then OvrToGTA | L/R signs plausible; fusion still poor → projection/FOV missing. |
| FOV → CCam+0x60 ~94° | Freeze — reverted. |

**Lesson:** Next spike must be a function that **actually draws the scene** (L4D2VR `RenderView` equivalent), two RTs **in the same frame**, full eye rate. Alternating-eye is not target quality.

String present in `GTAIV.exe`: `SceneToGBuffer`, `CRenderPhaseDrawScene` — not yet used as a reliable dual-draw hook.

---

## Milestones

### M1 — Stereo foundation (next focus)
1. Find/hook draw pass (not just BuildRenderList).  
2. Per frame: cam L → draw → RT_L; cam R → draw → RT_R.  
3. Submit both; tick only 1×.  
4. Per-eye projection without freeze path.

### M2 — Display quality
FOV/aspect, near clip, HUD, SteamVR SS / recommended RT size.

### M3 — Body / UI
Head hide on script thread; optional third-person body.

### M4 — Comfort
Snap turn, vignette, vehicle cam.

---

## Decision rules

1. Prefer 2× render + 1× tick over mono cinema.  
2. **No alternating-eye** as the goal (BotW; for us an FPS trap).  
3. One behavior change per risky test.  
4. No natives from EndScene.  
5. Keep stock DXVK as long as interop is sufficient.
