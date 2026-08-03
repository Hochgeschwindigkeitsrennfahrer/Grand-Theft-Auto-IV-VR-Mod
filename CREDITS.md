# Credits & external sources

Experimental community glue. Links for everything we ship, require, or optionally depend on.

## Runtime / ship dependencies

| Component | Link | Role |
|-----------|------|------|
| **DXVK 3.0.2** | [doitsujin/dxvk · v3.0.2](https://github.com/doitsujin/dxvk/releases/tag/v3.0.2) | Deliverable `d3d9.dll` (x32). Flat OK. `ID3D9VkInterop*` → OpenVR Submit |
| **dxvk-gplasync** | [Ph42oN / dxvk-gplasync](https://gitlab.com/Ph42oN/dxvk-gplasync/-/releases) | Optional async present A/B (stutter). Not required for VR |
| **GTAIV.dxvk-cache** | [Nexus FusionFix caches](https://www.nexusmods.com/gta4/mods/385?tab=files); local multi-vendor copy in `inspo/dxvk cache/` | Optional state cache; version-sensitive |
| **OpenVR** | [ValveSoftware/openvr](https://github.com/ValveSoftware/openvr) | SteamVR API. Pin headers to match SteamVR (e.g. **v2.12.14** / `IVRSystem_023`) |
| **SteamVR** | [Steam app 250820](https://store.steampowered.com/app/250820/SteamVR/) | Compositor runtime |
| **FusionFix** | [ThirteenAG/GTAIV.EFLC.FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) | CE ASI loader + fixes + update assets |
| **Ultimate ASI Loader** | [ThirteenAG/Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) | `dinput8.dll` plugin loader (also via FusionFix) |
| **MinHook** | [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook) | Hooks inside `gtaiv_dxvk_vr.asi` |

## Optional ScriptHook stack (natives / .NET scripts)

| Component | Link | Role |
|-----------|------|------|
| **ScriptHook.dll** (Aru) | [dev-c.com/gtaiv/scripthook](http://dev-c.com/gtaiv/scripthook/) | `GetNativeAddress` by name |
| **aCompleteEditionHook.asi** | [LCPDFR CE Compatibility Patch](https://www.lcpdfr.com/downloads/gta4mods/g17media/26726-compatibility-patch-for-gta-iv-complete-edition/) · [ModDB](https://www.moddb.com/downloads/compatibility-patch-for-gta-iv-complete-edition-04) | CE bridge for Aru ScriptHook. **Not required for VR ASI**; can crash with some FusionFix setups |
| **ScriptHookDotNet** | [HazardX/gta4_scripthookdotnet](https://github.com/HazardX/gta4_scripthookdotnet) · [CE v1.7.1.8](https://github.com/Tomasak/gta4_scripthookdotnet/releases/tag/release) | Optional .NET scripts (e.g. First Person ASI tests) |

## Architecture & technique references

| Project | Link | What we take |
|---------|------|----------------|
| **L4D2VR** | [sd805/l4d2vr](https://github.com/sd805/l4d2vr) | Hooks + DXVK + OpenVR Submit shape |
| **UEVR** | [praydog/UEVR](https://github.com/praydog/UEVR) | Sequential stereo / FOV honesty |
| **Halo MCC VR** | [pancreations/Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR) | Same-frame stereo + FOV lessons |
| **IronWolf DXVK** | [TheIronWolfModding/dxvk](https://github.com/TheIronWolfModding/dxvk) | Historical VR mailbox — **not** ship base |
| **openRBRVR** | [Detegr/openRBRVR](https://github.com/Detegr/openRBRVR) | DX9→VR patterns (OpenXR deferred) |

## Stereo / FOV lessons (public writeups only)

| Source | Link |
|--------|------|
| OpenVR compositor overview | [IVRCompositor_Overview](https://github.com/ValveSoftware/openvr/wiki/IVRCompositor_Overview) |
| OpenVR pose Submit | [openvr#1253](https://github.com/ValveSoftware/openvr/issues/1253) |
| Luke Ross public AER notes | [Patreon](https://www.patreon.com/posts/gaming-in-vr-at-76076877) |
| GTA V R.E.A.L. FAQ (public) | [LukeRoss00/gta5-real-mod](https://github.com/LukeRoss00/gta5-real-mod) |

**We do not redistribute** closed Luke Ross / R.E.A.L. / HL2VR binaries.

## This project

| Item | Note |
|------|------|
| **gtaiv_dxvk_vr.asi** | Our glue: GTA IV CE + DXVK interop + OpenVR |
| Tooling | Cursor, Git, Visual Studio 2022 (Win32), Vulkan SDK (DXVK builds) |

## License reminder

- Our scaffolding / ASI sources: **MIT**.
- DXVK, OpenVR, MinHook, FusionFix, ScriptHook, and all referenced projects keep **their own** licenses.
- Respect Rockstar’s terms: no game asset redistribution.
