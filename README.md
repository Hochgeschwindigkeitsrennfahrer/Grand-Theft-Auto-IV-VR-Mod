<img width="2560" height="1707" alt="image" src="https://github.com/user-attachments/assets/4a9ccf21-f484-4f07-b106-7419fead62b3" />

# gtaiv-dxvk-vr

**WIP** VR glue for **GTA IV Complete Edition** (Steam): stock **DXVK** `d3d9.dll` + our **Win32 ASI** + **OpenVR / SteamVR**.

Experimental singleplayer / offline project — not a polished commercial VR port.

---

## What works

| Supported | Not supported (yet) |
|-----------|---------------------|
| **SteamVR** only (OpenVR) | OpenXR / standalone WMR / Meta runtime without SteamVR |
| **Head tracking** (look around) | Motion controller hands / VR gun stock |
| Xbox / gamepad **recommended** | Full VR locomotion redesign |

Developed on **HP Reverb G2** via SteamVR. Other SteamVR headsets may work; not tested as primary.

---

## Controls

| Input | Action |
|-------|--------|
| **F3** or **Xbox Select (Back)** | Open / close VR settings menu |
| **F9** or **Right stick click** | Recenter view (SteamVR zero + heading baseline) |
| Menu: WASD / arrows / left stick | Navigate |
| Menu: Enter | Apply |
| **F4** | Cycle stereo / render profiles |

Kill VR: set `gtaiv_dxvk_vr.stereo` to `0` next to `GTAIV.exe`, then restart. Or rename `gtaiv_dxvk_vr.asi` → `.asi.off`.

---

## Requirements

1. **GTA IV Complete Edition** (Steam) — 32-bit `GTAIV.exe`
2. **[FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix)** (ASI loader + CE fixes)
3. **SteamVR** running with a tracked headset
4. Our ASI + DXVK `d3d9.dll` drop (see friend / prebuilt packs when shared)

Everything that loads into `GTAIV.exe` is **Win32 / x86**.

---

## External sources (files we rely on)

Links for every third-party piece we actively use, ship, or optionally depend on:

| Component | What it is | Link |
|-----------|------------|------|
| **DXVK 3.0.2** | Deliverable `d3d9.dll` (x32) + `ID3D9VkInterop*` for SteamVR Submit | [doitsujin/dxvk · v3.0.2](https://github.com/doitsujin/dxvk/releases/tag/v3.0.2) |
| **dxvk-gplasync** | Optional async DXVK A/B (shader stutter). Not required for VR Submit | [Ph42oN / dxvk-gplasync (GitLab)](https://gitlab.com/Ph42oN/dxvk-gplasync/-/releases) |
| **GTAIV.dxvk-cache** | Optional DXVK state cache (less first-run stutter; version-sensitive) | Community / FusionFix ecosystem caches e.g. [Nexus · FusionFix files](https://www.nexusmods.com/gta4/mods/385?tab=files) — we also ship a multi-vendor copy under `inspo/dxvk cache/` |
| **OpenVR** | SteamVR compositor API (`openvr_api.dll`) | [ValveSoftware/openvr](https://github.com/ValveSoftware/openvr) |
| **SteamVR** | Runtime compositor | [Steam · SteamVR](https://store.steampowered.com/app/250820/SteamVR/) |
| **FusionFix** | CE fixes + `plugins\` + update assets; ships ASI loader as `dinput8.dll` | [ThirteenAG/GTAIV.EFLC.FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) |
| **Ultimate ASI Loader** | Proxy that loads `.asi` plugins (`dinput8.dll`) — also bundled with FusionFix | [ThirteenAG/Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) |
| **MinHook** | Win32 / vtable hooks inside our ASI | [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook) |
| **ScriptHook.dll** (Aru) | Optional — name-resolve natives (`GetNativeAddress`) for phone/aim helpers | [dev-c.com · GTA IV Script Hook](http://dev-c.com/gtaiv/scripthook/) |
| **aCompleteEditionHook.asi** | Optional CE bridge for Aru ScriptHook on Complete Edition | [LCPDFR · CE Compatibility Patch (LMS)](https://www.lcpdfr.com/downloads/gta4mods/g17media/26726-compatibility-patch-for-gta-iv-complete-edition/) · [ModDB mirror](https://www.moddb.com/downloads/compatibility-patch-for-gta-iv-complete-edition-04) |
| **ScriptHookDotNet** | Optional .NET scripts (First Person ASI tests, etc.) | [HazardX/gta4_scripthookdotnet](https://github.com/HazardX/gta4_scripthookdotnet) · CE build [Tomasak v1.7.1.8](https://github.com/Tomasak/gta4_scripthookdotnet/releases/tag/release) |

### Important notes

- **`aCompleteEditionHook.asi` is not required for our VR ASI.** FusionFix alone is enough to load `gtaiv_dxvk_vr.asi`. On some CE + FusionFix setups the CE hook **crashes at boot** — leave it out unless you need ScriptHook scripts.
- **ScriptHook** is optional for core VR; some HUD/native helpers work better when `ScriptHook.dll` + CE hook resolve names.
- Prefer **stock DXVK 3.0.2** as `d3d9.dll` for VR. FusionFix’s own `vulkan.dll` path is separate; do not treat a random game’s `d3d9.dll` as “GTA VR”.

### Architecture inspiration (techniques only — not redistributed)

| Project | Link |
|---------|------|
| L4D2VR | [sd805/l4d2vr](https://github.com/sd805/l4d2vr) |
| UEVR (sequential stereo lessons) | [praydog/UEVR](https://github.com/praydog/UEVR) |
| Halo MCC VR | [pancreations/Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR) |
| IronWolf DXVK (historical mailbox; **not** our ship base) | [TheIronWolfModding/dxvk](https://github.com/TheIronWolfModding/dxvk) |

Full credit list: **[CREDITS.md](CREDITS.md)**.

---

## Status

**Done:** flat DXVK + ASI Mono-Submit into SteamVR.  
**Now:** stability / stereo / camera — one change per test build.

Live lab notes (dev): [`docs/CURRENT-STATE.md`](docs/CURRENT-STATE.md).

GTA IV belongs to Rockstar Games / Take-Two. This project redistributes **no** Rockstar game assets and **no** closed third-party VR binaries.

---

## License

MIT for *our* ASI / scaffolding. DXVK, OpenVR, MinHook, FusionFix, ScriptHook, and all linked projects keep **their own** licenses — see [CREDITS.md](CREDITS.md).
