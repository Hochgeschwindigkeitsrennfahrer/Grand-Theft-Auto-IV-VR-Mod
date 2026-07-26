# Community VR Port Guide — Notes for GTA IV CE + DXVK + OpenVR

Source: `inspiration/community vr port instructions/` (VHexen2 / Quake-engine OpenXR guide, 13 MD files).  
Read date: 2026-07-26. **Not** a drop-in for Rage/GTA — extract rules that transfer.

Our stack differs: **OpenVR (SteamVR)**, stock **DXVK 3.0.2** D3D9, **ASI glue**, Win32/x86, Reverb G2. No OpenXR for v0. No foreign `d3d9.dll` VR fork as the deliverable.

---

## Top takeaways (actionable)

1. **Asymmetric per-eye projection is mandatory.** Symmetric FOV wastes pixels and breaks stereo. OpenVR: `GetProjectionRaw` / `BuildHmdEyeProjection` — never `glm_perspective`-style symmetric.
2. **Render the world twice with different view + projection** (true stereo). AER (one eye/frame + reprojection) is a quality/perf tradeoff, not the gold path. Guide’s desktop path = `R_RenderView()` ×2. Ours = Mode **77** DrawScene×2 (cheaper than BuildRootA×2).
3. **Coordinate / scale consistency beats “feel” knobs.** Wrong axis map → mirrored world; wrong world_scale → giant/tiny world. Visual scale and locomotion scale are **opposite** ops in the guide (`multiply` visuals, `divide` walk).
4. **Do not invent FOV in the compositor path.** Canvas / Submit bounds must describe the FOV that was **actually drawn**. Claiming wider FOV than the draw frustum = zoom/crop (“giant monitor”). Claiming HMD aspect while the game rendered 16:9 = **horizontal squeeze**.
5. **Prefer letterbox / black bars over squash.** Filling the HMD by stretching a different aspect is wrong. Angle-correct inset (Mode 14 family) is the safe geometry.
6. **Per-eye state, no global assumptions.** Anything called twice per frame (lights, frustum, matrices) must be per-eye. Guide anti-pattern: “global state assumptions break stereo.”
7. **Temporal coherence:** pose, input, and eye capture must share one predicted time. Mismatched times → jump / lag (our AER Mode 76 lesson).
8. **Bring-up order:** colored clear → stereo verify → world → physics/input. Log everything during bring-up.
9. **UI is world-space panels**, not screen overlays — later work; not FOV/aspect.
10. **OpenXR chapters map to OpenVR:** swapchains→Submit textures; `XrFovf`→`GetProjectionRaw`; STAGE space→seated/standing tracking; session lifecycle→SteamVR + our ASI init.

---

## Chapter map → GTA IV CE relevance

| File | Keep for us? | Notes |
|------|----------------|-------|
| `01-overview.md` | Yes (strategy) | Phases: mono→playable→visual→VR gameplay. We already have Mono-Submit; stereo/FOV/camera is Phase “visual presence.” |
| `02-openxr-integration.md` | Partial | Frame loop / stereo / **asymmetric proj** translate to OpenVR. Skip Android loader / GLES swapchains. |
| `03-coordinate-systems.md` | Yes (rules) | Quake≠Rage axes, but: round-trip verify transforms; AABB min/max swap on negate; yaw sign traps; world_scale divide vs multiply. |
| `04-rendering-pipeline.md` | Yes (stereo) | Dual call of existing renderer with per-eye VP; re-entrancy; dynamic lights both eyes; frustum from **per-eye** VP. |
| `05-vr-input-locomotion.md` | Later | Controllers / snap turn — after FOV+stereo stable. |
| `06-physics-integration.md` | Later | Fixed timestep vs render rate — useful if we drive game from VR frame time. |
| `07-vr-weapons.md` | Later | 6DOF weapons N/A until FP cam solid. |
| `08-ui-in-vr.md` | Later | Cinema/panel HUD; aspect of panels must match content. |
| `09-platform-quirks.md` | Low | Quest/Adreno — ignore for Win32 SteamVR. |
| `10-sound-spatial-audio.md` | Later | Listener = **head**, not feet. |
| `11-lessons-learned.md` | **Yes** | Anti-pattern table: symmetric FOV, global stereo state, physics-at-render-rate. |
| `12-desktop-vr-porting.md` | Partial | Dual-eye wrap of existing renderer; flat↔VR toggle; mirror window. |
| `README.md` | Index | — |

---

## FOV / aspect / canvas rules (GTA-specific)

### What the game draws vs what the HMD sees

| Quantity | Meaning | Owner in our ASI |
|----------|---------|------------------|
| Backbuffer aspect W/H | Usually ~16:9 | `SetBackbufferAspect` / BB desc |
| Engine FOV (CCam+0x60) | Rage vertical-ish FOV field | FovSite / Mode74 HMD target |
| Drawn projection | What VS actually used mid-draw | EyeProj device patch + FOVPROOF |
| `gameTanH/V` | Claimed half-FOV of **drawn** image for canvas | `PublishGameFovFromCCamDegrees` |
| HMD cover tan | `GetProjectionRaw` max | `GetCoverFovTangents` |
| StereoCanvas StretchRect | Maps game frustum ↔ eye frustum | `CopySurfToEyeCanvas` |

**Invariant:** `gameTanH / gameTanV` **must** equal backbuffer aspect (within ~1%).  
If `gameTan` is forced to HMD cover `(coverH, coverV)` independently, aspect becomes ~1.1 (G2) while pixels are 16:9 → StretchRect **fills** the eye → rooms look **narrow and tall** (horizontal squeeze).

**Bars top/bottom with correct aspect** = game horizontal FOV covers HMD H, but game vertical FOV < HMD V (16:9 into near-square HMD). That is **correct**. Prefer this over squash.

**Giant monitor / crop-zoom** = canvas `gameTan` wider than the **drawn** frustum (lie), fill ≫100% → Mode79 failure. Gate canvas until drawn FOV proven.

### AER vs dual (guide + our mode map)

| Approach | Guide | Our mode | When |
|----------|-------|----------|------|
| Same-frame dual world draw | Gold: `R_RenderView` ×2 | **77** DrawScene×2; **75** sparse BuildRootA×2 (risky) | Best “real 3D” feel user liked on 77 |
| AER / alternate eye | Not emphasized as primary | **76** TextureWithPose | Jump risk; not default |
| Temporal pair-hold | — | **74** | Smooth safe / morning |
| FOV+aspect + dual | — | **80** (default), **81** letterbox, **82** fill | Presence without squeeze |

Never denser fearvr BuildRootA×2. Never live `view+0x80` write. Kill = **45**.

### IPD / stereo separation

Guide assumes true per-eye EyeToHead. We: HMD look + ±IPD on DRAW path; F8 sep (1 cm = most fused). Do **not** multiply IPD by WorldScale (broke fusion historically). Soft `stereoscale` is separate.

### Projection

- Build asymmetric eye proj from raw tangents (guide `BuildProjectionMatrix` ≡ our `BuildHmdEyeProjection`).
- Rage ignores `D3DTS_PROJECTION` (Mode 15 dead) → engine FOV via CCam / VS const patch, not SetTransform alone.
- Off-axis terms (`ox`/`oy`) matter; matrix IPD alone is not enough for correct stereo frustum.

---

## Anti-patterns (from guide) we already hit or must avoid

| Anti-pattern | Symptom | Our response |
|--------------|---------|--------------|
| Symmetric FOV | Edge warp / bad stereo | Use `GetProjectionRaw` |
| Claim FOV ≠ drawn FOV | Giant monitor / wrong bars | Drawn-FOV gate + honest `gameTan` |
| Independent H/V clamp to HMD cover | **Horizontal squeeze** after Mode80 prove | Aspect-preserving publish (Mode 80+) |
| Global state ×2 | One-eyed lights / jump | Per-eye inject windows |
| Physics @ render rate | Variable gameplay speed | Keep game tick; VR presents on top |
| Euler rotation deltas | Gimbal / drift | Quats for HMD (already) |
| Squash to kill bars | Narrow tall rooms | Prefer letterbox |

---

## What does **not** transfer 1:1

- OpenXR session / Android / GLES / Quest depth swapchain  
- Quake inch units and axis remaps (Rage has its own)  
- QuakeC / progs / BSP mesh rebuild  
- Full 6DOF weapons / holsters (future)

---

## Checklist before claiming “HMD-correct FOV”

- [ ] Log: `gameTan` aspect ≈ BB aspect  
- [ ] Log: `fill≈%h/%v` — if both ~100% with wrong aspect → bug  
- [ ] Prefer `fill≈100%h / <100%v` (bars) over squash  
- [ ] `proven=1` only after VS/EyeProj shows widened draw  
- [ ] Mode77 dual still smooth (StereoDiff climbing)  
- [ ] No `view+0x80` live write; kill file = 45  

## Session notes 2026-07-26 (Mode88/89 nap marathon)

Transferred rules that paid off this session:

1. **Never claim canvas FOV > drawn FOV** — Mode89 needed quiet FOV-proof before soft letterbox ungated.
2. **Prefer letterbox over squash** — Mode80 62%v is geometric for 16:9→G2; Mode89 soft68 is the only honest bar-reduction without Mode83 zoom.
3. **True stereo = dual world draw** — Mode77 DrawScene×2 + HOLD off-ticks (Mode88) beats AER for presence; every-frame dual (87/89) costs hitch.
4. **Device Reset lifecycle** — graphics-options freeze was POOL_DEFAULT eye RTs surviving Reset; hook Reset + release before recreate.
5. **Log I/O is a hitch source** — buffered Log + quieter EyeProj discover cut street disk thrash.

