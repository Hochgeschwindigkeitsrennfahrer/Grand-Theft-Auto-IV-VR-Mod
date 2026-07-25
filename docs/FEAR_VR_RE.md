# FEAR VR RE — what transfers to GTA IV CE

Source: [DR-89/fear-vr](https://github.com/DR-89/fear-vr) (cloned under `inspiration/fear-vr`, gitignored).  
License: **MIT** for their Host/Proxy/GameClient loader/docs (see repo `LICENSE`). Do **not** redistribute F.E.A.R. retail/SDK assets. Cite and adapt patterns only.

Status: studied 2026-07-25 for Mode **75** proper 3D stereo.

---

## How fear-vr does 3D stereo (short)

1. **Hook below sim, above world draw** — replace LithTech `ILTRenderer` VTable slot 17 (player `RenderCamera` alias → slot 19). Client updates / AI / audio / HUD shell stay **once** per frame (`docs/STEREO-RESEARCH.md`).
2. **Same-frame eye loop** (`stereo_hook.cpp` → `RenderStereo`):
   - Read OpenXR render request (poses + per-eye FOV).
   - Build **shared symmetric FOV** from both eyes (`stereo_math.h` `SharedSymmetricFov`).
   - **Save** camera transform + FOV.
   - For each eye: apply relative HMD rotation + eye position (IPD from eye poses), `SetCameraFOV(symmetric)`, clear RT, call original `RenderCamera`, **capture** backbuffer to that eye.
   - **Restore** camera + FOV; stage pair on Present → x64 OpenXR host.
3. **Not used:** geometry-shader duplication, viewport SBS split, or AER-only as the main path. AER/depth reprojection is only a documented **fallback** if double `RenderCamera` fails.
4. **Safety:** SEH around stereo; on fault restore camera and mono `RenderCamera`. F8 toggles stereo. Version/byte-pattern checks before patching.

Architecture note: FEAR is x86 D3D9 + **separate x64 OpenXR host** over IPC. We stay **in-process OpenVR** (Win32 ASI) — that IPC/host split does **not** transfer.

---

## Map to our stack (DXVK d3d9 + ASI + OpenVR)

| fear-vr | GTA IV CE (us) | Transfers? |
|---------|----------------|------------|
| LithTech `RenderCamera` ×2 | Rage `BuildRootA` ×2 (Mode 73/75) | **Yes concept** — two world builds same tick with L/R view |
| Hook *below* client updates | `BuildRootA` still includes phase/streaming work | **Partial** — no clean “world-only” API; full×2 is heavy / apt→city risk |
| Save/restore camera + FOV | `SetStereoEye` + `RefreshLiveCam` + PubProj bak/restore; Mode74 HMD FOV site | **Yes** |
| Symmetric HMD FOV | `Mode74HmdTargetCcamDegrees` + `BuildHmdEyeProjection` | **Yes** |
| Relative HMD look + eye offset | `Mode74ApplyHmdEyeLocal` + `Mode74OffsetViewLocal` / UploadFn / CamMatrix dual IPD | **Yes** (fixed 2026-07-25 jumpfix) |
| Capture BB per eye → Submit | `CopyBbToEyeCanvas` → OpenVR `Submit` | **Yes** |
| OpenXR host + shared textures | OpenVR in ASI + DXVK interop | **No** — keep our compositor |
| GameClient rebuild / Public Tools | No Rage SDK | **No** |
| VTable retail pattern gate | Our AOB / mapped RVA map (`RE_OFFSETS.md`) | **Concept only** |
| HUD once after world | We canvas-capture mid dual; HUD often baked into BB | **Partial** |

**Does not transfer 1:1:** FEAR engine ≠ Rage. We cannot call a documented `RenderCamera`. Same-frame dual on GTA = gated `BuildRootA`×2 + VS/PubProj src-copy (**never** live `view+0x80`).

---

## What we ported into Mode 75 — then SCRAPPED denser

Fear-vr-inspired **same-frame dual** was tried as Mode75 denser **1/2** BuildRootA×2.

**Headset 2026-07-25:** denser fearvr dual felt **LESS like actual 3D** than hitchcut → **SCRAPPED**.

Mode **75** now = safer sparse **1/8** BuildRootA×2 only (opt-in).  
Mode **77** = cheaper DrawScene×2 (Approach A) — log-proven StereoDiff overnight.  
Mode **74** = morning default. Mode **76** = AER. Mode **78**/ **79** = shader-const / FOV hammer.

---

## Honest limits

- Perf will often suck on any BuildRootA×2 until a smaller Rage “world-only” seam is headset-validated (Mode77 is the candidate).
- Apt→city / streaming can still force SESSION OFF or kill=45 on Mode75.
- Viewport/SBS (Approach B) cannot invent parallax without a second world draw — exhausted offline.
