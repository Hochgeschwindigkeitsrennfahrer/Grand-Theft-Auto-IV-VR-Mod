# Flat DXVK under FusionFix (Troubleshooting)

Goal: Some x86 DXVK `d3d9`/`vulkan.dll` shows GTA IV CE **flat** correctly.  
Without that, no OpenVR submit milestone.

## Quick switch

File `GTAIV\d3d9.cfg`:

```ini
[MAIN]
API = 0   ; DirectX 9
API = 1   ; Vulkan (loads vulkan.dll = DXVK)
```

After every change: **restart** the game.

## Known failed attempts (this setup)

| Test | Result |
|------|----------|
| IronWolf tiw-rel as `d3d9.dll` | Textures gone, vertex explosion |
| FF Vulkan 2.6.2 | black screen, crash |
| without `dxvk.conf` | same |
| Admin | same |
| OneDrive Rockstar | not present |
| GPL=False / hideNvidia=False | same |
| new NVIDIA driver | same |

## Known good (this setup)

| File | Version / note |
|-------|-------------------|
| `GTAIV\d3d9.dll` | Stock [DXVK 3.0.2](https://github.com/doitsujin/dxvk/releases/tag/v3.0.2) → folder **`x32\d3d9.dll`** (named directly as such) |
| `GTAIV\vulkan.dll` | **left unchanged** (FF original) |
| FusionFix | installed |

Important: success was **replacing `d3d9.dll`**, not renaming/deleting `vulkan.dll`.

FF-bundled older DXVK and our IronWolf **tiw-rel (~2.4.1)** → flat **not** usable here on RTX 4070 Ti.

## Replacing d3d9.dll (recipe that worked here)

1. Backup: existing `d3d9.dll` → e.g. `d3d9.dll.bak`  
2. [DXVK Releases](https://github.com/doitsujin/dxvk/releases) → archive → **`x32\d3d9.dll`**  
3. Copy to `GTAIV\` as **`d3d9.dll`**  
4. Do not touch `vulkan.dll`  
5. Start game, verify flat rendering  

Alternative (not the successful path here): FF docs often mention renaming to `vulkan.dll` + `API = 1` — test separately if needed.

## Implication for VR

Stock 3.0.2 has **no** IronWolf mailbox (`GetVRDesc` / `IDirect3DVR9`).  
Next step: VR patches on a **new** DXVK base (toward 3.0.x), deliverable still as x86 `d3d9.dll`.
