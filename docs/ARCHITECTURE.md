# Architecture — Phase B (DXVK VR)

## Inspired by

- [sd805/l4d2vr](https://github.com/sd805/l4d2vr) — game hooks + **DXVK submodule** → `d3d9.dll`
- [sd805/dxvk](https://github.com/sd805/dxvk) (async / VR-oriented fork used by L4D2VR)
- [Detegr/openRBRVR](https://github.com/Detegr/openRBRVR) + dxvk-openRBRVR — DX9 + **OpenXR** on 32-bit
- HL2VR — custom DXVK + OpenVR Submit + async (closed; lessons only)

## Intended layout (after submodule init)

```
gtaiv-dxvk-vr/
  dxvk/                 # git submodule (start: sd805/dxvk)
  src/gtaiv/            # GTA-specific glue (camera, sigscan, ini) — future
  thirdparty/           # OpenXR loader / MinHook if needed outside DXVK
  scripts/
  docs/
```

Deliverable: **`d3d9.dll` (Win32)** into `GTAIV\`.

## Runtime choice

| Runtime | Role here |
|---------|-----------|
| **WMR OpenXR** | Preferred on Reverb G2 + Win32 |
| SteamVR OpenXR | Often broken for 32-bit — last resort |
| OpenVR | L4D2VR default; optional if OpenXR blocked |

## Modules (planned)

1. **DXVK core** — D3D9→Vulkan (submodule)
2. **VR present path** — acquire eye images; `xrEndFrame` or OpenVR `Submit`
3. **Async** — keep/port dxvk-async / gplasync ideas for stutter
4. **GTA adapter** — FusionFix coexistence; Present/RT sizing; later camera AOB/natives
5. **Logging** — `gtaiv_dxvk_vr.log` next to exe (Phase A lesson: pasteable logs)

## Explicit non-goals (v0)

- Shipping a full copy of L4D2VR game code
- Supporting multiplayer / VAC
- Replacing Phase A until you decide to pivot
