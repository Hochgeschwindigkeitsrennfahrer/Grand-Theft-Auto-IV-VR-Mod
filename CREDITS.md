# Credits & sources

This project is experimental community glue. Credit goes to the people and repos whose work we build on or learn from.

## Core dependencies

| Component | Project | Role in gtaiv-dxvk-vr |
|-----------|---------|------------------------|
| **DXVK 3.0.2** | [doitsujin/dxvk](https://github.com/doitsujin/dxvk), [v3.0.2](https://github.com/doitsujin/dxvk/releases/tag/v3.0.2) | Stock x86 `d3d9.dll`; flat rendering and CPU-mailbox capture source |
| **OpenXR SDK** | [KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) | Primary x64 sidecar API; OpenXR never loads into `GTAIV.exe` |
| **OpenXR Simulator** | [elliotttate/OpenXR-Simulator](https://github.com/elliotttate/OpenXR-Simulator) | Headless development and first-person stereo/input proof runtime; not redistributed |
| **OpenVR** | [ValveSoftware/openvr](https://github.com/ValveSoftware/openvr) | Preserved x86 SteamVR/Reverb G2 fallback |
| **SteamVR** | Valve (Steam) | Optional legacy compositor for the OpenVR fallback; not used by Mode 58 |
| **FusionFix** | [ThirteenAG/GTAIV.EFLC.FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) | CE ASI loader + fixes. **Install from upstream**; we do not redistribute its `update\` tree or Rockstar assets |
| **MinHook** | [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook) | Win32 API / vtable hooks in the ASI |

## Architecture & VR pattern references

| Project | Link | What we take |
|---------|------|----------------|
| **L4D2VR** | [sd805/l4d2vr](https://github.com/sd805/l4d2vr) | Overall shape: game hooks + DXVK + OpenVR Submit |
| **sd805/dxvk** | [sd805/dxvk](https://github.com/sd805/dxvk) | L4D2VR DXVK line (reference) |
| **HL2 VR Mod** | community / closed builds | Concept only: custom DXVK + OpenVR — **do not redistribute** |
| **IronWolf DXVK** | [TheIronWolfModding/dxvk](https://github.com/TheIronWolfModding/dxvk) | Historical VR mailbox / `IDirect3DVR9`. Useful history; **not** our ship base (flat broke here → stock 3.0.2 + interop) |
| **UEVR** | [praydog/UEVR](https://github.com/praydog/UEVR) | Native / synchronized sequential stereo priority; FOV honesty |
| **Halo MCC VR** | [pancreations/Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR) | Same-frame stereo + wide FOV lessons |
| **openRBRVR** | [Detegr/openRBRVR](https://github.com/Detegr/openRBRVR) | DX9-to-OpenXR architecture and input patterns |

## GTA IV CE ecosystem

| Project | Link | Note |
|---------|------|------|
| **FusionShaders** | [Parallellines0451/GTAIV.EFLC.FusionShaders](https://github.com/Parallellines0451/GTAIV.EFLC.FusionShaders) | Decompiled Rage shaders (research) |
| **GTA IV Complete Edition** | Rockstar / Steam | Game belongs to Rockstar Games / Take-Two. We redistribute **no** game assets |

## Stereo / FOV / AER lessons (techniques only)

| Source | Link | Takeaway |
|--------|------|----------|
| OpenVR compositor overview | [IVRCompositor_Overview](https://github.com/ValveSoftware/openvr/wiki/IVRCompositor_Overview) | WaitGetPoses, render, then Submit (fallback path) |
| OpenVR `#1253` (pose submit) | [issue 1253](https://github.com/ValveSoftware/openvr/issues/1253) | `Submit_TextureWithPose` / per-eye pose stamping |
| Luke Ross public AER notes | [Patreon post](https://www.patreon.com/posts/gaming-in-vr-at-76076877) | Legacy AER = alt-eye + reproject; **AER v2 OF not copied** |
| GTA V R.E.A.L. FAQ (public) | [LukeRoss00/gta5-real-mod](https://github.com/LukeRoss00/gta5-real-mod) | FOV match HMD; body/helmet class problems — inspiration only |
| OpenXR specification | [Khronos OpenXR Registry](https://registry.khronos.org/OpenXR/) | Predicted display time, views, actions, and composition layers |
| Meta render / compositor docs | Meta Horizon docs | Predicted poses and compositor behavior |

**We do not redistribute** closed Luke Ross / R.E.A.L. / HL2VR binaries or packs.

## Optional / local inspiration (not dependencies)

| Item | Note |
|------|------|
| C06alt First Person (community) | FP offsets/head-hide ideas only; no unlicensed files are required |
| dxvk-gplasync (Ph42oN et al.) | Async-present experiments only; stock DXVK 3.0.2 remains the documented base |

## This project

| Item | Note |
|------|------|
| **gtaiv_dxvk_vr.asi** | Original x86 GTA IV camera, stereo, UI, controller, and frame-producer glue |
| **gtaiv_xr_host.exe** | Original x64 OpenXR sidecar, compositor, controller bridge, and CPU-mailbox consumer |
| Tooling | Cursor, Git, Visual Studio 2022, Vulkan SDK (DXVK builds) |

## License reminder

- Our scaffolding / ASI sources: **MIT** (see `LICENSE` if present).
- DXVK, OpenXR, OpenVR, MinHook, FusionFix, and all referenced projects keep **their own** licenses.
- Respect Rockstar’s terms: no game asset redistribution.
