# FAQ

## Why not just IronWolf `d3d9.dll`?

On this setup (RTX 4070 Ti), old VR forks (IronWolf ~2.4, FF-DXVK 2.6.2) are broken flat.  
**Stock-DXVK 3.0.2** works flat. Modern DXVK provides Vulkan handles via **`ID3D9VkInteropDevice`** — that is enough for OpenVR submit. Our code lives in an **ASI**, not necessarily in a custom DXVK fork. Details: `docs/VR_STRATEGY.md`.

## What is FusionFix for?

FusionFix is the usual **CE base**: fixes + **ASI loader**.

- It does **not** do VR.  
- It helps load mods and run CE more stably.  
- When **our** `d3d9.dll` is active: turn FusionFix **Vulkan/DXVK switch off**, or two Vulkan paths fight each other.

Theoretically you could use only a DLL next to the EXE — in practice CE + FusionFix is the standard setup.

## Why OpenVR instead of OpenXR?

Because L4D2VR and HL2VR work that way and IronWolf's mailbox is built for it. Less custom work for the first headset frame. SteamVR must be running (Reverb G2 through it).

OpenXR can come later; not needed for v0.

## Is it enough to drop IronWolf/sd805 DLL into GTA?

**No.** Author and L4D2VR: without **game-side** glue there is no magic VR. Also, L4D2 hacks (viewport, HUD, …) are not meant for GTA.

## Which IronWolf branch?

| Branch | Use |
|--------|-----|
| `master` | **Not** for VR mailbox (`d3d9_vr.h` missing) |
| `vr-dx9-rel` | Classic OpenVR mailbox (good for learning/copying) |
| `tiw-rel-241-*` | Current line; use OpenVR, ignore OpenXR helpers |

Details: `docs/IRONWOLF_DXVK.md`.

## What is the first success?

Game starts with our x86 `d3d9.dll` **flat** (monitor).  
Then: **one** image in the headset (Mono-Submit). Only then stereo/camera.

## Where is the Cursor guide for home?

→ [`docs/HOME_CURSOR.md`](HOME_CURSOR.md)
