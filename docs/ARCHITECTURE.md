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
    -> Mode 55: completed center-eye capture downsized to 1280x720
    -> FNVVR-style advancing triple-slot BGRA mailbox
    -> pointer-free frame descriptor ABI v5
       (WorldMono / WorldStereo / UiQuad + content aspect + mailbox route)
    -> gtaiv_xr_host.exe (x64) D3D11 upload + sRGB SRV
    -> OpenXR projection swapchains or a stationary LOCAL-space UI quad
```

The host's generated calibration scene and the offline mailbox transfer test pass.
Mode 55 still needs its first complete gameplay/menu headset acceptance.
Immersive-mono frames require one source frame, pose sequence, and rendered `XrTime`
but explicitly carry no WVP-stereo proof. True `WorldStereo` remains fail-closed
behind Mode 54's exact WVP flag; the live audit proved that Mode 54's capture window
does not own GTA's later replay-thread draws. Pause/map, loading, and phone use one
fresh image on a quad whose pose is latched in `LOCAL` space until the UI closes.

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
