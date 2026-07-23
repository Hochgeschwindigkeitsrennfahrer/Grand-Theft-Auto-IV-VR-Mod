# gtaiv-dxvk-vr

**Phase B** pivot project: GTA IV Complete Edition VR via a **VR-patched DXVK `d3d9.dll`**, oriented on [sd805/l4d2vr](https://github.com/sd805/l4d2vr) (and HL2VR / openRBRVR lessons).

**Phase A** (keep using until you pivot): sibling folder  
`../gtaiv-openxr` — ASI + companion D3D11 + OpenXR.

This repo is the **ready detour** if Phase A stalls or you lose interest in the D3D11 bridge.

## Target stack (L4D2VR-shaped)

```
GTAIV.exe (Win32, speaks D3D9)
  → custom d3d9.dll  (forked DXVK + VR hooks + async)
       → Vulkan eye textures
  → OpenXR submit preferred (Reverb G2 / WMR)
       OpenVR only as secondary if needed
  → headset
```

L4D2VR uses **OpenVR**; we prefer **OpenXR** on Win32+G2 (SteamVR OpenXR often lacks a solid 32-bit path — see `docs/LESSONS_FROM_PHASE_A.md`).

## Status

Scaffold **0.1.0** — docs + submodule hooks + build notes. **No working VR d3d9.dll yet.**  
Init DXVK submodule at home: `.\scripts\init-submodules.ps1`

## Relation to L4D2VR

| L4D2VR | This project |
|--------|----------------|
| Source engine hooks + DXVK VR fork | RAGE/GTA IV — game-specific RE still required |
| Builds `d3d9.dll` (x86) | Same deliverable shape |
| OpenVR | OpenXR first (WMR), OpenVR optional |
| `sd805/dxvk` submodule (async branch) | Same starting submodule URL; may diverge toward openRBRVR/IronWolf VR patches |
| VAC / `-insecure` | Singleplayer / offline only while developing |

**Do not** drop L4D2VR’s release `d3d9.dll` into GTA IV and expect VR — IronWolf/openRBRVR note that VR DXVK needs **game-side** work.

## Quick links

| Doc | Purpose |
|-----|---------|
| [HANDOFF.md](HANDOFF.md) | Human pivot guide |
| [AGENTS.md](AGENTS.md) | Cursor agent contract |
| [docs/CURRENT-STATE.md](docs/CURRENT-STATE.md) | Status ledger |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Intended modules |
| [docs/IRONWOLF_DXVK.md](docs/IRONWOLF_DXVK.md) | **What IronWolf VR-DXVK does (plain language)** |
| [docs/LESSONS_FROM_PHASE_A.md](docs/LESSONS_FROM_PHASE_A.md) | Constraints from gtaiv-openxr |
| [docs/RELATION_TO_PHASE_A.md](docs/RELATION_TO_PHASE_A.md) | When to switch |

## License

MIT for *our* scaffolding. DXVK / L4D2VR / third-party code keep their own licenses when submodules are added.
