# Architecture — Phase B (DXVK + OpenVR)

## Inspired by

- [sd805/l4d2vr](https://github.com/sd805/l4d2vr) — game hooks + DXVK as `d3d9.dll` + **OpenVR Submit**
- [TheIronWolfModding/dxvk](https://github.com/TheIronWolfModding/dxvk) — DX9→VK + OpenVR mailbox (`docs/IRONWOLF_DXVK.md`)
- HL2VR — custom DXVK + OpenVR Submit + async (closed; lessons only)
- [Detegr/openRBRVR](https://github.com/Detegr/openRBRVR) — optional later (OpenXR / 32-bit); **not** Phase B v0

## Intended layout (after submodule init)

```
gtaiv-dxvk-vr/
  dxvk/                 # git submodule (IronWolf tiw-rel or vr-dx9-rel / sd805)
  src/gtaiv/            # GTA-specific glue (camera, sigscan, ini) — future
  thirdparty/           # openvr_api / MinHook if needed outside DXVK
  scripts/
  docs/
```

Deliverable: **`d3d9.dll` (Win32)** into `GTAIV\`.

## Runtime choice

| Runtime | Role here |
|---------|-----------|
| **OpenVR via SteamVR** | **Primary** — same as L4D2VR / HL2VR |
| WMR OpenXR | Phase A / optional later — not required for first HMD frame |
| SteamVR OpenXR | Avoid for Phase B v0 (Phase A 32-bit pain); OpenVR is enough |

Reverb G2: use SteamVR as the OpenVR runtime (same as other OpenVR mods).

## Modules (planned)

1. **DXVK core** — D3D9→Vulkan (submodule)
2. **VR present path** — `GetVRDesc` / Begin·End submit → OpenVR `IVRCompositor::Submit`
3. **Async** — keep/port dxvk-async / gplasync ideas for stutter
4. **GTA adapter** — FusionFix coexistence; Present/RT sizing; later camera AOB/natives
5. **Logging** — `gtaiv_dxvk_vr.log` next to exe (Phase A lesson: pasteable logs)

## Explicit non-goals (v0)

- OpenXR session / swapchains in Phase B
- Shipping a full copy of L4D2VR game code
- Supporting multiplayer / VAC
- Replacing Phase A until you decide to pivot
