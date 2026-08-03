# Credits & sources

This project is experimental community glue. Credit goes to the people and repos whose work we build on or learn from.

## Core dependencies (shipped / required)

| Component | Project | Role in gtaiv-dxvk-vr |
|-----------|---------|------------------------|
| **DXVK 3.0.2** | [doitsujin/dxvk](https://github.com/doitsujin/dxvk) · [v3.0.2](https://github.com/doitsujin/dxvk/releases/tag/v3.0.2) | Deliverable `d3d9.dll` (x32). Flat rendering proven. `ID3D9VkInterop*` for OpenVR Submit |
| **OpenVR** | [ValveSoftware/openvr](https://github.com/ValveSoftware/openvr) | SteamVR compositor API. Pin headers/runtime to match installed SteamVR (we use **v2.12.14** / `IVRSystem_023`) |
| **SteamVR** | Valve (Steam) | Runtime compositor |
| **FusionFix** | [ThirteenAG/GTAIV.EFLC.FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) | CE ASI loader + fixes. **Install from upstream** — we do not redistribute Rockstar assets from its `update\` tree |
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
| **openRBRVR** | [Detegr/openRBRVR](https://github.com/Detegr/openRBRVR) | DX9→VR patterns (OpenXR path deferred for us) |

## GTA IV CE ecosystem

| Project | Link | Note |
|---------|------|------|
| **FusionShaders** | [Parallellines0451/GTAIV.EFLC.FusionShaders](https://github.com/Parallellines0451/GTAIV.EFLC.FusionShaders) | Decompiled Rage shaders (research) |
| **GTA IV Complete Edition** | Rockstar / Steam | Game belongs to Rockstar Games / Take-Two. We redistribute **no** game assets |

## Stereo / FOV / AER lessons (techniques only)

| Source | Link | Takeaway |
|--------|------|----------|
| OpenVR compositor overview | [IVRCompositor_Overview](https://github.com/ValveSoftware/openvr/wiki/IVRCompositor_Overview) | WaitGetPoses → render → Submit |
| OpenVR `#1253` (pose submit) | [issue 1253](https://github.com/ValveSoftware/openvr/issues/1253) | `Submit_TextureWithPose` / per-eye pose stamping |
|  public AER notes | [Patreon post]() | Legacy AER = alt-eye + reproject; **AER v2 OF not copied** |
| GTA V flat-to-VR FAQ (public) | []() | FOV match HMD; body/helmet class problems — inspiration only |
| Meta render / compositor docs | Meta Horizon docs | Predicted poses, timewarp concepts |

**We do not redistribute** closed HL2VR / third-party VR binaries or packs.

## Optional / local inspiration (not dependencies)

| Item | Note |
|------|------|
| C06alt First Person (community) | FP offsets / head-hide ideas → our `camoff` / `pedhide` |
| dxvk-gplasync (Ph42oN et al.) | Async present experiments only — **not** required for Mode 170; stock 3.0.2 remains the documented deliverable |

## This project

| Item | Note |
|------|------|
| **gtaiv_dxvk_vr.asi** | Original glue for GTA IV CE + DXVK interop + OpenVR |
| Tooling | Cursor, Git, Visual Studio 2022, Vulkan SDK (DXVK builds) |

## License reminder

- Our scaffolding / ASI sources: **MIT** (see `LICENSE` if present).
- DXVK, OpenVR, MinHook, FusionFix, and all referenced projects keep **their own** licenses.
- Respect Rockstar’s terms: no game asset redistribution.
