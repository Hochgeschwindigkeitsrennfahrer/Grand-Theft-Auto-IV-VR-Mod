# Architecture

> This file describes the proven v0 OpenVR architecture. The post-v0 target adds a
> separate x64 OpenXR host for Quest 3 while keeping this path as fallback. See
> [`FULL_VR_PLAN.md`](FULL_VR_PLAN.md).

The direct Quest prototype now contains two separate pieces:

```text
gtaiv_xr_host.exe (x64, separate process)
    -> Khronos OpenXR loader 1.1.61 (statically linked)
    -> XR_KHR_D3D11_enable
    -> Meta OpenXR / Quest 3
```

```text
GTAIV.exe (x86) EndScene
    -> DXVK D3D9 shared surface
    -> native x86 D3D11 copy + shared NT textures/fences
    -> fixed-width shared-memory descriptor
    -> gtaiv_xr_host.exe (x64) D3D11 copy
    -> OpenXR eye swapchains
```

The host's generated calibration scene passed its Quest test. The new GTA bridge
compiles but still needs its first headset result. It is intentionally a mono-frame
transport for that test; pose publication, per-eye frame identity, and accepted
same-frame stereo remain later gates.

## Runtime

```
GTAIV.exe (Win32)
    │  D3D9 API
    ▼
d3d9.dll          ← Stock DXVK 3.0.2 (proven flat)
    │  Vulkan images + ID3D9VkInterop*
    ▼
gtaiv_dxvk_vr.asi ← our glue (OpenVR Submit)
    ▼
OpenVR (openvr_api / SteamVR)
    ▼
Headset (e.g. Reverb G2)
```

FusionFix: ASI loader + CE fixes. Leave FF `vulkan.dll` untouched when `d3d9.dll` = Stock 3.0.2.

## Folders

```
gtaiv-dxvk-vr/
  dxvk/                 # old IronWolf submodule (reference, not deliverable)
  src/gtaiv/            # glue / soon ASI
  src/openxr_host/      # separate x64 OpenXR calibration/sidecar host
  thirdparty/dxvk/      # ID3D9VkInterop snapshot (v3.0.2)
  thirdparty/ironwolf/  # historical IDirect3DVR9 reference
  config/
  scripts/
  docs/                 # incl. VR_STRATEGY.md
```

## Modules

1. **DXVK 3.0.2** — flat `d3d9.dll` (binary or later own build)
2. **Interop** — `ID3D9VkInteropDevice` / `Texture` (`docs/VR_STRATEGY.md`)
3. **OpenVR submit** — pattern from L4D2VR `vr.cpp`, but interop instead of IronWolf
4. **GTA ASI** — `src/gtaiv/` → Present hook + Submit
5. **Logging** — `gtaiv_dxvk_vr.log`

## Non-goals (v0)

- OpenXR  
- Finished motion-control mod  
- Multiplayer / VAC  
- 1:1 port of L4D2VR source hooks  

## References (technical)

See `docs/REFERENCES.md` and `docs/IRONWOLF_DXVK.md`.
