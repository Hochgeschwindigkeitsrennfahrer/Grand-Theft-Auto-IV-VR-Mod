# Luke Ross R.E.A.L. VR — reverse-engineering notes

**Date:** 2026-07-25 (updated same day — GTA5 vs CP2077 mode deep-dive)  
**Scope:** Local Inspiration drop `inspiration/real vr all mods/` (gitignored).  
**Goal:** Learn architecture / hooks / stereo+submit patterns for **our own** GTA IV CE ASI glue.  
**Hard rule:** Do **not** copy or redistribute Luke Ross / 3DMigoto / closed binaries into our product.

---

## Certainty statement (stereo / render modes)

### Verdict (certainty ≥98%)

**Luke Ross RealVR does not ship Halo-style same-frame dual-eye stereo.**  
All stereoscopic paths in this drop are **alternate-eye rendering (AER)**:

| Family | What it is | Where |
|--------|------------|-------|
| **Legacy AER** | One engine eye per frame; hold other eye; compositor timewarp / ASW | GTA5 `d3d11.dll` (only path); RealVR64 menu mode |
| **AER v2** (+ rate variants) | Still AER, plus CUDA optical-flow frame synthesis to hide ghosting | Pack-root `RealVR64.dll` only (CP2077 and other modern titles) |
| **Mono** | No stereo; higher frame rate | RealVR64 menu only |
| **Same-frame / Halo dual** | Both eyes from one game tick | **Not present** — no menu label, no string, no code path name in either DLL |

**“Stereoscopic 3D”** in the RealVR64 overlay is a **category tag** on AER modes, not a separate true-stereo renderer.  
**“Parallel projections”** is a separate aiming/OpenVR option string — **not** a stereo render mode.

### Module certainty (GTA5 folder)

| Role | Module | Evidence |
|------|--------|----------|
| **VR renderer + compositor path** | `GTA 5/d3d11.dll` | Exports `RVR*`; imports `openvr_api.dll`; RTTI `OpenVRMgr` / `OpenXRMgr` / `OculusVRMgr` / `RVRMgr`; PDB `…\3DmiGTA\x64\Release\d3d11.pdb` |
| **Game camera / FOV glue** | `GTA 5/asi/RealVR.asi` | ScriptHookV; `GetProcAddress` `RVR*`; FOV/Proj/View patches; PDB `…\3DmiGTA\RealVR\bin\Release\RealVR.pdb` |
| **Not the renderer** | `dinput8.dll` | ASI loader proxy |
| **Not the renderer** | `nvapi64.dll` | 3DMigoto NVAPI stereo shim |
| **Not used by GTA5 folder** | pack-root `RealVR64.dll` | Modern multi-game core (CP2077 etc.); GTA5 ships its **own** forked `d3d11.dll` |

GTA5 stereo = **AER on/off** (`Stereo=1` → “alternate eyes”) + dual `Submit[%c]` with pose — **not** same-frame dual.

---

## Files present (GTA 5 drop)

```
inspiration/real vr all mods/
  RealVR64.dll                 (~10.4 MB)  shared multi-game injector (other titles)
  RealVR.ini                   pack stub
  GTA 5/
    d3d11.dll                  (~3.6 MB)   ★ RealVR renderer (3DMigoto fork + RVR)
    d3dx.ini                   3DMigoto config
    asi/RealVR.asi             (~0.22 MB)  ★ ScriptHookV game glue
    openvr_api.dll             OpenVR loader
    dinput8.dll                ASI loader
    nvapi64.dll                3DMigoto NVAPI shim
    ScriptHookV.dll
    RealVR.ini                 hotkeys / Stereo / VRAPI / UniversalFOVFix
    commandline.txt            -width 1080 -height 1080
    ShaderFixes/*.txt          small 3DMigoto shader overrides
    Settings/{High,Medium,Low}/ …
```

No PDBs shipped in the drop; PDB **paths** remain inside the PE debug directories (build machine paths above).

---

## Module map (who does what)

```
GTA5.exe
  ├─ dinput8.dll          → loads scripts/*.asi
  ├─ ScriptHookV.dll
  │    └─ RealVR.asi      → natives + memory patches (FOV/cam/heading)
  │         GetProcAddress("d3d11.dll", "RVRWaitAndTrackHMD" | …)
  │         reads/writes g_RVRData / g_fRVRGameProj
  └─ d3d11.dll (RealVR)   → wraps D3D11 + DXGI Present
       ├─ HackerDevice / HackerSwapChain / HackerContext   (3DMigoto)
       ├─ RVRMgr ──┬─ OpenVRMgr    (IVRSystem_021, IVRCompositor_022, IVROverlay_021)
       │           ├─ OpenXRMgr
       │           └─ OculusVRMgr
       └─ openvr_api.dll / Oculus / OpenXR runtimes
```

`RealVR.ini` `VRAPI`: `0=autodetect, 1=Oculus, 2=OpenVR, 3=OpenXR`.

---

## Exported bridge API (`d3d11.dll` → ASI)

| Export | RVA (ImageBase `0x180000000`) | Role |
|--------|-------------------------------|------|
| `RVRLog` | `0x00084150` | Levelled logger used by both sides |
| `RVRSeqCheck` | `0x00084560` | Frame sequencing / Present markers (`'M'`, `'P'`, …) into `RVRSeqCheck.txt` |
| `RVRWaitAndTrackHMD` | `0x00084670` | Pose wait / frame begin gate (virtual calls on `RVRMgr`) |
| `RVRGetFrameDesc` | `0x00084B30` | Fills frame desc; selects eye char `'L'`/`'R'` from low bit of flags |
| `RVRGetPoseDesc` | `0x00084B70` | Copies pose + floats for ASI (“Predicted head pose …”) |
| `g_RVRData` | `0x0036D710` | Shared runtime state blob |
| `g_fRVRGameProj` | `0x0036BDD0` | Shared game projection / FOV float(s) |
| `Install3DMigotoDriverProfileW` | `0x000EC340` | 3DMigoto leftover |

**`RVRGetFrameDesc` eye select (disasm):** `test cl,1` → `mov eax,'R'` / `cmovne eax,'L'` (constants `0x52`/`0x4C`) — AER eye identity for this frame.

---

## How RealVR renders VR (call flow)

### 1) Init

1. Game loads `d3d11.dll` (RealVR fork of 3DMigoto).
2. Device/swapchain create wraps into `HackerDevice` / `HackerSwapChain`.
3. `RVRMgr` picks backend:
   - OpenVR: `VR_InitInternal2` → `VR_GetGenericInterface`:
     - `IVRSystem_021`
     - `IVRCompositor_022`
     - `IVROverlay_021` (HUD overlay `"RVR_HUD"`)
   - or OpenXR / Oculus paths (`xrBeginFrame`/`xrEndFrame`, `ovr_SubmitFrame2`, …).
4. ASI loads, resolves `RVR*` via `GetProcAddress`, applies game patches.

### 2) Per-frame stereo loop (AER + dual Submit)

**Game side (ASI)** — roughly:

1. Call `RVRWaitAndTrackHMD` early (string: if late, d3d11 does it itself:  
   `"RVRWaitAndTrackHMD() was not called promptly for this frame, doing it now"`).
2. Read `RVRGetFrameDesc` / `RVRGetPoseDesc` → which eye this frame, predicted pose.
3. Patch / adjust view + projection for that eye (`AdjustViewInverse`, `TrackProj`, FOV patches).
4. Game renders **one** eye’s worth of frame (alternate-eye).
5. `RVRSeqCheck` marks progress through the frame.

**Compositor side (`d3d11` Present path)** — roughly:

1. `HackerSwapChain::Present` / `Present1` (logged).
2. Frame gate logs: `"Waiting to begin frame #%d"` → `"Ready to begin frame #%d"` → `"Beginning frame #%d"`.
3. Capture / hold eye textures (fresh eye + previous eye).
4. Log:  
   `"Submitting frame #%d: sampletime=… displaytime=… [L] pose … [R] pose …"`
5. **OpenVR Submit loop** (site ~`VA 0x180125Axx`):
   - `ebx` counts `0..1` (`cmp ebx,2` / `jl`) — **two eyes every Present**.
   - `call [IVRCompositor+0x28]` → **`Submit`** on `IVRCompositor_022`.
   - Stack flag **`8`** = OpenVR `Submit_TextureWithPose` (`0x08`).
   - On failure: `"ERROR: Submit[%c] failed…"` with `'L'`/`'R'`.
6. Optional HUD via `IVROverlay` (texture + HMD-relative / seated transform).
7. Stats strings mention **ASW** (SteamVR asynchronous spacewarp) — compositor reprojection of the held eye.

### 3) Projection / FOV (anti “giant screen”)

Three coordinated layers — **not** canvas zoom:

| Layer | Where | Evidence |
|-------|-------|----------|
| Square RT | `commandline.txt` | `-width 1080 -height 1080` |
| Engine FOV patches | `RealVR.asi` | `UniversalFOVFix`; patches `FOVUni`, `FOV1stCar`, `FOV3rd`, `Proj`, `ViewInverse`, `CamParams` |
| Projection CB rewrite | `d3d11.dll` | `"Unmap() overrode the FOV in the projection matrix"` — scales projection terms when mapped CB unmaps |
| HMD tangents | `d3d11.dll` | `"Left/Right eye FOV: U=… D=… L=… R=…"` |
| Shared proj float | both | `g_fRVRGameProj`; ASI logs `ProjFOV = … Near=… Far=…` |

Ini default: `UniversalFOVFix = 3` (always). Wrong FOV = warp / wrong scale — same lesson as our Mode 35/74 work.

### 4) Pose / heading (game wrestling)

ASI strings show a **render-time view fix** on top of gameplay cam:

- `AdjustViewInverse() fixed the gameplay camera to {yaw,pitch,roll; pos}…`
- `FoldWorldRotIntoRenderPose`
- `HeadingControl`, `PitchControl`, `GyroVehicle`, `Decouple3rdPersonCam`
- Cutscene modes: `StereoInCutscenes`, `CutscenePitchMode`, theater-like flat options

This matches Luke’s public FAQ: game “wrestles” for camera ownership → supplementary render-time view + pitch overrides.

### 5) Same-frame L/R?

**No — not in GTA5, and not in modern RealVR64 / CP2077 either.**

- GTA5 ini: stereo = **alternate eyes** (boolean).
- One engine render pass per frame (eye bit in `RVRGetFrameDesc`).
- Both eyes still **Submitted** each Present; the non-rendered eye is prior texture + pose stamp + SteamVR ASW/timewarp.
- RealVR64 “Render mode” menu only lists AER v2 rate caps + Legacy AER + Mono (see matrix below). Zero Halo / same-frame / dual-render labels.

AER v2 optical-flow synthesis lives in `RealVR64.dll` (CUDA: `cudart64_12.dll`, `nvcuda.dll`, `nvCreateOpticalFlowCuda`) — **out of scope / not portable** to our Win32 DXVK path.

---

## Mode matrix — GTA5 vs CP2077 (certainty ≥98%)

### Architecture split

| | **GTA 5** (this drop folder) | **CP2077** (this drop folder) |
|--|------------------------------|-------------------------------|
| **Renderer** | Forked `GTA 5/d3d11.dll` (3DMigoto + RVR) | Pack-root **`RealVR64.dll`** (`runtime_CP2077`) |
| **Game glue** | `asi/RealVR.asi` (ScriptHookV) | In-DLL AOBs (`CP2077LocateCamera`, `CP2077PatchCamera`, …) |
| **Folder contents** | Full DLL/ASI/ini/settings | Thin: `Settings/option.replace` + `S0.dxbc`/`S1.dxbc` only |
| **Menu UI** | Hotkeys via `RealVR.ini` (no “Render mode” cycle) | Full **R.E.A.L. VR overlay** / Quick Menu |
| **Stereo family** | Legacy AER only | Legacy AER **or** AER v2 (+ rate variants) + Mono |
| **CUDA / OF** | No | Yes (`cudart64_12`, optical flow) |
| **Submit** | `Submit[%c]` ×2, `Submit_TextureWithPose` (flag 8) on OpenVR | Same pattern: `ERROR: Submit[%c] failed…` in RealVR64 |
| **Same-frame dual** | **No** | **No** |

Note: RealVR64 also has `runtime_GTAVE` (GTA V Enhanced) — a **newer** shared-DLL path, separate from the classic `GTA 5/` folder fork.

### GTA5 — all stereo/render-related menu options (`GTA 5/RealVR.ini`)

These are the **complete** VR-facing toggles in the GTA5 ini (Release 7). None select same-frame dual.

| Ini key | Hotkey (comment) | Values / meaning |
|---------|------------------|------------------|
| `Hotkeys` | F11 | 0/1 — enable hotkeys |
| **`Stereo`** | NUMPAD2 | **0/1 — off / “stereo rendering (alternate eyes)”** ← only stereo render toggle |
| `StereoInCutscenes` | 0 | **0=normal, 1=dynamic, 2=flat screen** — cutscene presentation, not engine dual-eye |
| `DominantEye` | T | 0=none, 1=left, 2=right (ADS) |
| `HeadingControl` | Y | 0=always, 1=only aiming, 2=never |
| `CutscenePitchMode` | J | 0=absolute, 1=relative, 2=cut relative |
| `ShowFPS` | N | 0/1 |
| `GyroVehicle` | End | 0/1 |
| `UniversalFOVFix` | — | 0=never … 3=always |
| `ZoomOverride` | NUMPAD. | 0=never … 3=always |
| `VRAPI` | — | 0=autodetect, 1=Oculus, 2=OpenVR, 3=OpenXR |

`d3dx.ini` `[Stereo]` / `StereoMode` strings are **3DMigoto / NVAPI leftovers**, not Luke’s VR eye mode.

**Eye identity evidence (disasm):** `RVRGetFrameDesc` @ RVA `0x84B30`: `test cl,1` → `mov eax,'L'(0x4C)` / `cmovne eax,'R'(0x52)`.

### CP2077 / RealVR64 — “Render mode” menu (exact UI names from binary)

Overlay labels: `Render mode`, `VR rendering mode:`, `Previous/Next render mode`, `Force 60 fps for render modes with adaptive frame rates` (`ForceAdaptiveTo60`).

Exact mode **short names** in the string table (file ~`0x5BC140`…):

| UI name | Category tag | Binary description | Ghosting tag | Family |
|---------|--------------|--------------------|--------------|--------|
| **`1/2 rate`** | Stereoscopic 3D | Frame interpolation with fps cap at 1/2 the headset refresh rate | Low ghosting | **AER v2** |
| **`AER v2`** | Stereoscopic 3D | Automatic frame interpolation with adaptive fps cap | Low ghosting | **AER v2** |
| **`1/4 rate`** | Stereoscopic 3D | Frame interpolation with fps cap at 1/4 the headset refresh rate | Low ghosting | **AER v2** |
| **`1/3 rate`** | Stereoscopic 3D | Frame interpolation with fps cap at 1/3 the headset refresh rate | Low ghosting | **AER v2** |
| **`1/6 rate`** | Stereoscopic 3D | Frame interpolation with fps cap at 1/6 the headset refresh rate | Low ghosting | **AER v2** |
| **`1/5 rate`** | Stereoscopic 3D | Frame interpolation with fps cap at 1/5 the headset refresh rate | Low ghosting | **AER v2** |
| **`Mono`** | No 3D | Double frame rate without interpolation | Low ghosting | Mono |
| **`Legacy AER`** | Stereoscopic 3D | Minimal latency / No frame interpolation | Some ghosting | **Legacy AER** |

Supporting strings (same DLL): `Higher quality AER v2`, `Legacy AER mode should always be supported`, `No supported render modes`, `Camera rotation compensation (only relevant for Legacy AER)`, ASW warning *“not compatible with the alternate eye rendering tech used by R.E.A.L. VR mods”*.

**Absent from both DLLs (searched):** `Halo`, `same-frame`, `true stereo`, `dual render`, `both eyes same`, `simultaneous eye`. The only “same frame” hits are unrelated camera-debug errors.

### Non-mode options often confused with stereo

| String / option | What it actually is |
|-----------------|---------------------|
| `Parallel projections` | Separate aiming/projection tweak (“Extra aiming tweaks are available with parallel projections”) — **not** a render mode |
| `ForceMonoZoom` | Zoom/mono helper for scopes/UI |
| `Auto2DInMenus` | Flat UI in menus |
| `StereoInCutscenes` (GTA5) | Cutscene flat/dynamic/normal — still not same-tick L+R |

Public corroboration: Luke’s Patreon / press list games as **“AER v2”**; AMD guidance = set Render mode to **Legacy AER** (no AER v2 OF). No public “true stereo / Halo” mode.

---

## OpenVR interface versions (GTA5 `d3d11.dll`)

| Interface | String RVA |
|-----------|------------|
| `IVRSystem_021` | `0x002D4AD8` |
| `IVRCompositor_022` | `0x002D4AE8` |
| `IVROverlay_021` | `0x002D4B00` |

Imports from `openvr_api.dll`:  
`VR_InitInternal2`, `VR_ShutdownInternal`, `VR_GetGenericInterface`, `VR_IsInterfaceVersionValid`, `VR_GetInitToken`, `VR_GetVRInitErrorAsEnglishDescription`.

**Submit evidence:** vtable slot `+0x28` + flags `8` ⇒ `IVRCompositor::Submit` with `Submit_TextureWithPose` (aligns with Luke’s public OpenVR #1253 guidance).

---

## RTTI classes (renderer)

From `d3d11.dll`:

- `RVRMgr`, `OpenVRMgr`, `OpenXRMgr`, `OculusVRMgr`
- `RVRTexture`, `OpenVRTexture`, `OpenXRTexture`, `OculusVRTexture`
- `HackerDevice`, `HackerContext`, `HackerSwapChain`, `HackerUpscalingSwapChain`
- `DirectModeSetActiveEyeCommand`, `PerDrawStereoOverrideCommand` (3DMigoto stereo remnants)

---

## ASI patches (game EXE)

Failed-to-apply strings name the patch set:

1. `ViewInverse`
2. `Proj`
3. `FOV1stCar`
4. `FOV3rd`
5. `FOVUni` (Universal FOV)
6. `CamParams`

Plus runtime helpers: `SetCamMetadataPitch`, world-rotation tracking for vehicles/cutscenes, dominant-eye ADS, etc.

---

## Pack-root `RealVR64.dll` (CP2077 + modern titles)

- ~500 exports: proxies `D3D9/10/11/12` create + DXGI factories + window/input + Bink; Vulkan layer JSON in some game folders (`VK_LAYER_RealVR`).
- Depends on `openvr_api.dll` + CUDA (`cudart64_12`, `nvcuda`) for AER v2 optical flow.
- Per-game RTTI: `runtime_CP2077`, `runtime_ELDEN`, `runtime_GTAVE`, … (GTA5 classic folder is **not** this path).
- Overlay Quick Menu: Render mode / camera / tourist / game speed / presets.
- Submit still AER-shaped: `ERROR: Submit[%c] failed with error code %d`.
- **GTA5 folder does not load this as its d3d11.**

---

## What to port to GTA IV CE / Mode74 (actionable)

**Do not** port binaries, 3DMigoto, ScriptHookV, or CUDA AER v2. Port **ideas**:

1. **Keep AER honest with `Submit_TextureWithPose`**  
   Every eye texture must carry the **capture-time** pose. Luke submits **both** eyes each Present with distinct poses — match that on WMR/G2.

2. **Engine FOV = HMD cover FOV (not canvas lie)**  
   UniversalFOVFix + Unmap projection rewrite = same class as our FovSite / Mode 35/74.

3. **Split game-glue vs submit-glue**  
   RealVR’s split maps to: CE memory hooks for cam/FOV vs DXVK interop Submit (`g_RVRData` / `g_fRVRGameProj` analog = our FrameDesc).

4. **Render-time view fix > fighting gameplay cam**  
   `AdjustViewInverse` / `FoldWorldRotIntoRenderPose` family — our CopyMat / DRAW-PATH seated 6DoF.

5. **Do not expect a Luke “true stereo” switch to copy**  
   He has **no** Halo/same-frame option. Presence for him = AER (+ AER v2 OF on NVIDIA, which we cannot port) + FOV + pose-stamped Submit.  
   **Our Mode74 AER path is the honest RealVR analog.**  
   **Our dual×2 / same-frame** is a *different* ambition (Halo/L4D2 class) — optional, not “what Luke ships.”

6. **If we ever want AER-v2-class comfort** without CUDA OF: improve held-eye pose stamp, soft motion guard, and FOV — not invent a fake “Luke dual mode.”

---

## Tooling used (this RE session)

- `dumpbin` (VS 18) — exports/imports/dependents/headers  
- Python 3.13 + `pefile` + Capstone — strings, RVA↔offset, disasm, LEA xrefs  
- Manual correlation with OpenVR `Submit` flags / `IVRCompositor_022` vtable layout  
- Cross-check: RealVR64 render-mode string table + public Patreon/press (AER v2 lists)

Scratch artifacts under `docs/_re_scratch/` are disposable local dumps (not required to rediscover the above).

---

## References (public)

- https://github.com/LukeRoss00/gta5-real-mod (FAQ)  
- https://github.com/ValveSoftware/openvr/issues/1253 (Submit_TextureWithPose / AER)  
- https://www.patreon.com/posts/score-one-for-129841705 (Render mode → Legacy AER on AMD; AER v2 = NVIDIA OF)  
- Project docs: `docs/INSPIRATION_NOTES.md`, `docs/REFERENCES.md`, `docs/STEREO_EYE_OFFSET.md`
