# References

All important external links for this project.

## Architecture references

| Project | Link | What we take from it |
|---------|------|----------------------|
| **L4D2VR** | https://github.com/sd805/l4d2vr | Overall shape: hooks + DXVK `d3d9.dll` + **OpenVR submit** |
| **sd805/dxvk** | https://github.com/sd805/dxvk | L4D2VR DXVK (IronWolf-based + async + L4D2 hacks) |
| **doitsujin/dxvk 3.0.2** | https://github.com/doitsujin/dxvk/releases/tag/v3.0.2 | **Flat base** + `ID3D9VkInterop*` |
| **IronWolf DXVK** | https://github.com/TheIronWolfModding/dxvk | Historical VR mailbox; flat rejected here |
| IronWolf `vr-dx9-rel` | https://github.com/TheIronWolfModding/dxvk/tree/vr-dx9-rel | Older OpenVR mailbox (L4D2VR era) |
| IronWolf `tiw-rel-241-260612` | https://github.com/TheIronWolfModding/dxvk/tree/tiw-rel-241-260612 | Newer VR line (OpenVR + optional OpenXR helpers) |
| HL2VR | (closed) | Lesson: custom DXVK + OpenVR + async — concept only |

## Optional / later

| Project | Link | Note |
|---------|------|---------|
| openRBRVR | https://github.com/Detegr/openRBRVR | DX9 + OpenXR, 32-bit — **not** v0 |
| dxvk-openRBRVR | https://github.com/Detegr/dxvk-openRBRVR | If OpenXR path ever needed |
| doitsujin/dxvk | https://github.com/doitsujin/dxvk | Upstream without VR mailbox |

## Game / CE setup

| Item | Link / location |
|------|------------|
| FusionFix (GTA IV CE) | https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix |
| **FusionShaders (decompiled Rage shaders)** | https://github.com/Parallellines0451/GTAIV.EFLC.FusionShaders |
| Halo MCC VR (same-frame / FOV lessons) | https://github.com/pancreations/Halo-MCC-VR |
| GTA IV CE | Steam / Rockstar Launcher |
| SteamVR | Steam library |
| OpenVR | https://github.com/ValveSoftware/openvr |

### FusionFix knobs we use for VR comfort

| Setting (Graphics menu or `plugins\GTAIV.EFLC.FusionFix.cfg`) | VR use |
|--------------------------------------------------------------|--------|
| `MotionBlur = 0` | Off — kills stereo echo / look-smear (Halo lesson) |
| `FieldOfView = 0..9` | Adds `n×5°` to `CCam+0x60` (FusionFix path; safer than our blind 94° write). `5` = +25° — shrinks black bars / giant-world feel |
| `GraphicsAPI` | Keep DX9/DXVK path that loads our `d3d9.dll` (do not switch away) |

## Tools

| Tool | Link |
|------|------|
| Cursor | https://cursor.com |
| Git | https://git-scm.com/download/win |
| VS 2022 | https://visualstudio.microsoft.com/ |
| Vulkan SDK | https://vulkan.lunarg.com/ |
| Meson | https://mesonbuild.com/ |

## Inspiration (local, not a dependency)

| Item | Location | What we take |
|------|----------|--------------|
| C06alt First Person v1.3 (Oculus ini) | `inspiration/firstperson mod/` | FPX/Y/Z → `camoff`; head hide → CE SetDraw (`pedhide`) |
| Third-party flat-to-VR pack (local inspo only) | `inspiration/thirdparty-vr-inspo/` (gitignored) | FOV=HMD, square+FOV, AER lessons, pitch/view overrides — **techniques only**, no binary redistribution |
| Notes | `docs/INSPIRATION_NOTES.md` | Do **not** adopt FOV 111 / FusionFix FOV / canvas-zoom claimed-FOV warp |

## Decoupled aiming / GTA IV RE sources (research 2026-07-29)

| Item | Link | What we take |
|------|------|--------------|
| **Holydh UEVR_GTASADE `WeaponManager.cpp`** | https://github.com/Holydh/UEVR_GTASADE/blob/main/WeaponManager.cpp | THE pattern: weapon→controller attach, barrel-ray from 2 points, overwrite aim forward/cam pos in memory, restore when not aiming, vehicles excluded |
| Holydh `MemoryManager.cpp` | https://github.com/Holydh/UEVR_GTASADE/blob/main/MemoryManager.cpp | NOP game writer instructions + write own values; HW-breakpoint shot detection (DR0 + vectored handler) |
| IV-Network reversing notes | https://github.com/GTA-Network/IV-Network/blob/master/Documents/Development_ReverseEngineering/IV-Reversing.txt | `CPed+0xBC0 = CPedIKManager*`, `+0x70 vecAimTarget` (1.0.7-era — UNVERIFIED CE), CWeapon/CWeaponInfo layout |
| IV-Network natives list | https://github.com/GTA-Network/IV-Network/blob/master/Documents/Development_ReverseEngineering/IV-Natives.txt | `TASK_AIM_GUN_AT_COORD`, `TASK_SHOOT_AT_COORD`, `FIRE_SINGLE_BULLET`, `GET_PED_BONE_POSITION`, `FIRE_PED_WEAPON` (arg counts) |
| Zolika1351 / ClonkAndre IV-SDK | https://github.com/Zolika1351/iv-sdk | IV class/function addresses 1.0.7/1.0.8 (NOT CE — porting reference only) |
| IV-SDK .NET (CE-aware) | https://github.com/ClonkAndre/IV-SDK-DotNet | Native signatures incl. Vector3 overloads; TLS main-thread native calling lesson |
| GTAMods memory addresses (IV) | https://gtamods.com/wiki/Memory_Addresses_(GTA4) | Classic-version addresses for cross-checking CE finds |

Plan docs: `docs/DECOUPLED_AIM_PLAN.md` (plans A-D, CE recipes, checklist),
`docs/CAMERA_ATTACH_REVIEW.md` (attach redesign), `docs/HANDOFF_HOME_AIM_CAMERA.md`,
`docs/STEREO_TRUETICK_REVIEW.md` (TrueStereo 230–234).

## VR rendering / flat→VR docs (web research 2026-07-24)

| Topic | Link | Takeaway for us |
|-------|------|-----------------|
| OpenVR compositor | https://github.com/ValveSoftware/openvr/wiki/IVRCompositor_Overview | `WaitGetPoses` → render L/R → `Submit`; same-thread; serial L then R OK |
| OpenVR Submit + AER pose | https://github.com/ValveSoftware/openvr/issues/1253 () | `Submit_TextureWithPose` + per-eye `mDeviceToAbsoluteTracking`; SteamVR historically used last pose for both eyes on WMR/Oculus backends — test on G2 |
| pose-stamped AER / 60 fps (public Patreon) |  | Legacy AER = one eye/frame + reproject other; AER v2 = optical-flow intermediates (not for us) |
| AER v2 announcement |  | Paywalled; same lesson: pose-stamped AER, not canvas FOV lies |
| Meta / Rift render loop | https://developers.meta.com/horizon/documentation/native/pc/dg-render/ | Predicted eye poses; compositor does distortion + timewarp |
| Meta compositor / timewarp | https://developers.meta.com/horizon/documentation/spatial-sdk/os-compositor/ | Rotational timewarp; positional needs depth |
| Asymmetric / parallel projection | https://slugcat.systems/post/25-03-18-virtual-reality-projection-shenanigans/ | Per-eye 4-tangent FOV + eye pose; parallel-projection compat mode for canted panels |
| Parallel projection (OpenMR) | https://forum.openmr.com/t/why-do-some-games-require-parallel-projection-but-others-dont/29411 | Apps that ignore eye rotation break on canted HMDs |
| GTA V flat-to-VR FAQ (public) |  | FOV match HMD; AER; pitch override; body/helmet fixes — inspiration only |
| Halo MCC VR | https://github.com/pancreations/Halo-MCC-VR | Same-frame stereo + wide FOV priority |
| UEVR (praydog) | https://github.com/praydog/UEVR | Native per-eye view+proj; do not fake FOV in compositor |
| FusionFix Custom FOV | https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix `fixes.ixx` | Hook cam process CALL → `*(CCam+0x60) += n*5` — **our Mode 35 site** |
| FusionFix FOV issue #1326 | https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix/issues/1326 | Hipfire/driving FOV instruction RVAs (classic); additive preferred |

## Flat→VR presence lessons (research 2026-07-24 evening)

| Approach | Kills “monitor on face”? | Breaks look-around? | Transfer to GTA IV CE |
|----------|--------------------------|---------------------|------------------------|
| **True engine FOV ≈ HMD** (Halo/L4D2/UEVR) | Yes — fills frustum, correct scale | No if canvas/submit uses **true** tangents | Mode **35** = FusionFix CCam+0x60 site ADD |
| Claimed canvas FOV / zoom | Fake fill | **Yes** (warp) — REJECTED | Do not re-enable |
| FusionFix menu FOV alone | Partial | Look-up warp with our old path | Keep menu at **0**; we own ADD via `fovadd` |
| Theater / OpenVR quad | Menus only | N/A | Later (Mode C deferred) |
| Stretched BB / nullptr Submit | No — diplopia | Yes | Mode 14 canvas already fixed |
| Independent VR cam vs wall collision | Presence near walls | Invasive | Deferred (`PLAN_NEXT`) |

ASI + D3D11 + OpenXR (WMR): `Documents\gtaiv-openxr` — separate home guide there: `gtaiv-openxr\docs\HOME_CURSOR.md`.  
**This repo is standalone** and does not require it.
