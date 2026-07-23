# Architektur

## Laufzeit

```
GTAIV.exe (Win32)
    │  D3D9 API
    ▼
d3d9.dll          ← unser Deliverable (VR-DXVK-Fork + optional Glue)
    │  Vulkan images
    ▼
OpenVR (openvr_api / SteamVR)
    ▼
Headset (z. B. Reverb G2)
```

FusionFix: lädt ASI-Mods und stabilisiert CE; **ersetzt nicht** unsere `d3d9.dll`. Bei Konflikt: FusionFix-Vulkan aus.

## Geplante Ordner

```
gtaiv-dxvk-vr/
  dxvk/                 # git submodule (nach init-submodules)
  src/gtaiv/            # Glue-Stubs (Log/Config/Submit-Platzhalter)
  thirdparty/ironwolf/  # API-Referenz-Header (Snapshot)
  config/               # dxvk.conf + ini examples
  scripts/              # init-submodules, deploy
  docs/
```

## Modules (planned)

1. **DXVK-Kern** — D3D9→Vulkan (submodule)
2. **VR-Mailbox** — `IDirect3DVR9` (`docs/IRONWOLF_API.md`)
3. **OpenVR-Submit** — Muster aus L4D2VR `vr.cpp` (`docs/L4D2VR_MAP.md`)
4. **GTA-Adapter** — `src/gtaiv/` stubs → echte Verdrahtung
5. **Logging** — `gtaiv_dxvk_vr.log`

## Nicht-Ziele (v0)

- OpenXR  
- Fertiger Motion-Control-Mod  
- Multiplayer / VAC  
- 1:1-Port von L4D2VR-Source-Hooks  

## Referenzen (Technik)

Siehe `docs/REFERENCES.md` und `docs/IRONWOLF_DXVK.md`.
