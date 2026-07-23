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
  dxvk/              # git submodule (IronWolf tiw-rel / vr-dx9-rel oder sd805)
  src/gtaiv/         # GTA-Glue (Kamera, Sigscan, Logs) — später
  thirdparty/        # openvr headers/libs falls nötig
  config/            # dxvk.conf.example
  scripts/           # init-submodules, später deploy
  docs/
```

## Module

1. **DXVK-Kern** — D3D9→Vulkan  
2. **VR-Mailbox** — `IDirect3DVR9` / `GetVRDesc` / Submit-Sync (IronWolf)  
3. **OpenVR-Submit** — `IVRCompositor::Submit` (L4D2VR-Muster)  
4. **Async (optional)** — weniger Shader-Stutter  
5. **GTA-Adapter** — Timing, RTs, später Kamera  
6. **Logging** — Datei neben `GTAIV.exe`

## Nicht-Ziele (v0)

- OpenXR  
- Fertiger Motion-Control-Mod  
- Multiplayer / VAC  
- 1:1-Port von L4D2VR-Source-Hooks  

## Referenzen (Technik)

Siehe `docs/REFERENCES.md` und `docs/IRONWOLF_DXVK.md`.
