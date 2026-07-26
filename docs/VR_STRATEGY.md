# VR Strategy (from 0.6)

> **Post-v0 status:** The OpenVR mono milestone below is complete. The primary next
> product target is Quest 3 through a separate x64 OpenXR host, while preserving this
> OpenVR path for fallback and Reverb G2 compatibility. See
> [`FULL_VR_PLAN.md`](FULL_VR_PLAN.md). GTA-loaded code remains Win32/x86.

## Decision

**Do not** keep forcing IronWolf `tiw-rel` (~DXVK 2.4) — flat rendering is broken on this PC.  
**Instead:** Stock DXVK **3.0.2** (flat proven) + official **`ID3D9VkInterop*`** API for OpenVR submit.

IronWolf `IDirect3DVR9` was the predecessor; in modern DXVK the equivalent is:

| IronWolf (legacy) | Stock DXVK 3.0.2 |
|----------------|------------------|
| `Direct3DCreateVRImpl` | `QueryInterface(ID3D9VkInteropDevice)` on the device |
| `GetVRDesc` | `ID3D9VkInteropTexture::GetVulkanImageInfo` + device/queue handles |
| `BeginVRSubmit` / Lock | `FlushRenderingCommands` + `LockSubmissionQueue` / `LockDevice` |
| Multi-View / OpenXR helpers | **ignore** (v0) |

Reference header (upstream): `src/d3d9/d3d9_interfaces.h` @ [v3.0.2](https://github.com/doitsujin/dxvk/tree/v3.0.2).  
Local GUID copy: `thirdparty/dxvk/d3d9_vk_interop.h`.

## Architecture v0

```
GTAIV.exe
  → d3d9.dll          = Stock DXVK 3.0.2 (flat ok)
  → FusionFix (ASI loader, CE fixes)
  → gtaiv_dxvk_vr.asi = our glue (OpenVR init + mono submit)
       → QI ID3D9VkInteropDevice / Texture
       → IVRCompositor::Submit (TextureType_Vulkan)
  → SteamVR → Reverb G2
```

No OpenXR in v0. One behavior change per test build.

## Milestones

1. **Interop probe** — ASI loads, logs successful QI + VkImage/device (no headset image yet).  
2. **Mono submit** — one eye image in the headset.  
3. **Stereo / camera** — see `docs/VR_MOD_PLAYBOOK.md` (reference model: BotW BetterVR + L4D2VR).

## Submodules

| Path | Role |
|------|--------|
| `dxvk/` (formerly IronWolf) | Reference / optional; **no longer** the deliverable base |
| Stock 3.0.2 binary | Deliverable `d3d9.dll` (as currently in the game) |
| Optional later: `doitsujin/dxvk` @ `v3.0.2` | only if we need to patch DXVK ourselves |

## What to keep in the game folder

- `d3d9.dll` = DXVK **3.0.2** x32 (proven)  
- `vulkan.dll` = FF original, untouched  
- FusionFix installed  
