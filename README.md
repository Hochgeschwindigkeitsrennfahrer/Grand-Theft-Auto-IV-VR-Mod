# gtaiv-dxvk-vr

**Phase B** pivot project: GTA IV Complete Edition VR via a **VR-patched DXVK `d3d9.dll`**, oriented on [sd805/l4d2vr](https://github.com/sd805/l4d2vr) and HL2VR.

**Phase A** (sibling, OpenXR/D3D11 path): `../gtaiv-openxr` — keep until you fully pivot.

## Target stack (L4D2VR / HL2VR-shaped)

```
GTAIV.exe (Win32, speaks D3D9)
  → custom d3d9.dll  (forked DXVK + VR mailbox + async)
       → Vulkan eye textures
  → OpenVR Submit  (SteamVR)     ← primary, like L4D2VR/HL2VR
  → headset (Reverb G2 via SteamVR)
```

**OpenVR first** — same compositor path as L4D2VR/HL2VR, so we can copy their `GetVRDesc` → `IVRCompositor::Submit` pattern.  
OpenXR stays optional later (Phase A / openRBRVR lessons); not required for Phase B v0.

Reverb G2: run **SteamVR** (WMR for SteamVR / SteamVR driver). That is normal for OpenVR mods.

## Status

Scaffold **0.2.0** — OpenVR-first strategy locked. **No working VR `d3d9.dll` yet.**  
Init DXVK submodule at home: `.\scripts\init-submodules.ps1`

## Relation to L4D2VR

| L4D2VR | This project |
|--------|----------------|
| Source engine hooks + DXVK VR fork | RAGE/GTA IV — game-specific RE still required |
| Builds `d3d9.dll` (x86) | Same deliverable shape |
| **OpenVR** | **OpenVR** (same) |
| `sd805/dxvk` submodule (async) | IronWolf `tiw-rel` / `vr-dx9-rel` or sd805 — see `docs/IRONWOLF_DXVK.md` |
| VAC / `-insecure` | Singleplayer / offline only while developing |

**Do not** drop L4D2VR’s release `d3d9.dll` into GTA IV and expect VR — still needs **game-side** glue.

## Quick links

| Doc | Purpose |
|-----|---------|
| [HANDOFF.md](HANDOFF.md) | Human pivot guide |
| [AGENTS.md](AGENTS.md) | Cursor agent contract |
| [docs/CURRENT-STATE.md](docs/CURRENT-STATE.md) | Status ledger |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Intended modules |
| [docs/IRONWOLF_DXVK.md](docs/IRONWOLF_DXVK.md) | IronWolf VR-DXVK (plain language) |
| [docs/LESSONS_FROM_PHASE_A.md](docs/LESSONS_FROM_PHASE_A.md) | Constraints from gtaiv-openxr |
| [docs/RELATION_TO_PHASE_A.md](docs/RELATION_TO_PHASE_A.md) | When to switch |

## License

MIT for *our* scaffolding. DXVK / L4D2VR / third-party code keep their own licenses when submodules are added.
