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

## Related (optional, not a prerequisite)

ASI + D3D11 + OpenXR (WMR): `Documents\gtaiv-openxr` — separate home guide there: `gtaiv-openxr\docs\HOME_CURSOR.md`.  
**This repo is standalone** and does not require it.
