# Architektur

## Laufzeit

```
GTAIV.exe (Win32)
    │  D3D9 API
    ▼
d3d9.dll          ← Stock DXVK 3.0.2 (flach bewiesen)
    │  Vulkan images + ID3D9VkInterop*
    ▼
gtaiv_dxvk_vr.asi ← unser Glue (OpenVR Submit)
    ▼
OpenVR (openvr_api / SteamVR)
    ▼
Headset (z. B. Reverb G2)
```

FusionFix: ASI-Loader + CE-Fixes. `vulkan.dll` von FF unangetastet lassen, wenn `d3d9.dll` = Stock 3.0.2.

## Ordner

```
gtaiv-dxvk-vr/
  dxvk/                 # altes IronWolf-Submodule (Referenz, nicht Deliverable)
  src/gtaiv/            # Glue / bald ASI
  thirdparty/dxvk/      # ID3D9VkInterop Snapshot (v3.0.2)
  thirdparty/ironwolf/  # historische IDirect3DVR9-Referenz
  config/
  scripts/
  docs/                 # inkl. VR_STRATEGY.md
```

## Modules

1. **DXVK 3.0.2** — flache `d3d9.dll` (Binary oder später eigener Build)
2. **Interop** — `ID3D9VkInteropDevice` / `Texture` (`docs/VR_STRATEGY.md`)
3. **OpenVR-Submit** — Muster L4D2VR `vr.cpp`, aber Interop statt IronWolf
4. **GTA-ASI** — `src/gtaiv/` → Present-Hook + Submit
5. **Logging** — `gtaiv_dxvk_vr.log`

## Nicht-Ziele (v0)

- OpenXR  
- Fertiger Motion-Control-Mod  
- Multiplayer / VAC  
- 1:1-Port von L4D2VR-Source-Hooks  

## Referenzen (Technik)

Siehe `docs/REFERENCES.md` und `docs/IRONWOLF_DXVK.md`.
