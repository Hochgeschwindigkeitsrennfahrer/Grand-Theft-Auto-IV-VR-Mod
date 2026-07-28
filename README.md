# gtaiv-dxvk-vr

**WIP** VR glue for **GTA IV Complete Edition** (Steam): stock **DXVK 3.0.2** `d3d9.dll` + our **Win32 ASI** + **OpenVR / SteamVR**. Target headset in development: **HP Reverb G2**.

```
GTAIV.exe (x86 / D3D9)
  → d3d9.dll              Stock DXVK 3.0.2 (flat desktop OK)
  → FusionFix             ASI loader + CE fixes (install separately)
  → gtaiv_dxvk_vr.asi     OpenVR init + stereo / camera glue
       → ID3D9VkInterop*  → IVRCompositor::Submit (Vulkan textures)
  → SteamVR → HMD
```

This is **not** a polished commercial VR port. It is an experimental singleplayer / offline project. Stereo is **real IPD parallax** via same-tick **DrawScene×2** (Praydog-style sequential dual), not Luke Ross AER v2.

---

## Current snapshot — Mode **170** (`clean/mode-170`)

| Item | Value |
|------|--------|
| Stereo file | `gtaiv_dxvk_vr.stereo` = **`170`** |
| What it is | FP-style cam + **fresh DrawScene dual every frame** (`everyN=1`), no Flush; hitch → HOLD |
| Feel (headset) | Strong FPS; **driving can still flicker** |
| Kill | Write **`167`** or **`0`** into `gtaiv_dxvk_vr.stereo`, restart |
| Prebuilt drop | [`prebuilt/mode170/`](prebuilt/mode170/) |

Earlier clean LKG (smoother, dual every 2nd DrawScene): Mode **120**. Safer car/HUD family: Mode **167**.

Live lab notes: [`docs/CURRENT-STATE.md`](docs/CURRENT-STATE.md).

---

## Requirements

1. **GTA IV Complete Edition** (Steam) — 32-bit `GTAIV.exe`
2. **[FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix)** (ASI loader + CE fixes). Graphics API = **DirectX 9** (not FusionFix Vulkan)
3. **SteamVR** running; headset tracked (Reverb G2 via WMR → SteamVR is fine)
4. **Singleplayer / offline** while developing
5. Optional build PC: VS 2022 C++ x86 tools, PowerShell, Git

**Hard rules:** everything that loads into `GTAIV.exe` is **Win32 / x86**. Compositor for v0 is **OpenVR only** (no OpenXR milestone yet). Do not drop a foreign game’s `d3d9.dll` and call it GTA VR.

---

## Quick install (prebuilt Mode 170)

1. Install FusionFix into the GTAIV folder (official release).
2. Close GTA IV.
3. Copy everything from [`prebuilt/mode170/drop-into-GTAIV/`](prebuilt/mode170/drop-into-GTAIV/) into the folder that contains `GTAIV.exe` (overwrite VR files when asked).
4. Start **SteamVR**, put the headset on, then launch GTA IV (offline / SP).
5. Check `gtaiv_dxvk_vr.log` next to the EXE:
   - `VR_Init OK`
   - `StereoMode: 170`
   - `StereoSubmit: L=1 R=1 mode=170`

Full steps: [`prebuilt/mode170/INSTALL.txt`](prebuilt/mode170/INSTALL.txt).

**Kill VR:** `gtaiv_dxvk_vr.stereo` → `0` (flat DXVK on monitor).  
**Safer stereo fallback:** `167` or `120`.

### Hotkeys

| Key | Action |
|-----|--------|
| F5 | Cycle VR eye-canvas resolution |
| F6 | Stereo scale |
| F7 | World-scale / FOV presets |
| F8 | IPD / separation |
| F9 | Recenter look |
| F10 | 6DoF / seated lean reset |

---

## Build from source

```powershell
git clone --recurse-submodules https://github.com/Hochgeschwindigkeitsrennfahrer/gtaiv-dxvk-vr.git
cd gtaiv-dxvk-vr
git checkout clean/mode-170
.\scripts\fetch-minhook.ps1
git clone --depth 1 --branch v2.12.14 https://github.com/ValveSoftware/openvr.git thirdparty\openvr
.\scripts\build-asi.ps1
.\scripts\deploy-asi.ps1   # copies into Steam GTAIV (edit path if needed)
```

Use **OpenVR SDK v2.12.14** (`IVRSystem_023`) with SteamVR **2.12.x**. Newer OpenVR headers against older SteamVR → `VR_Init` error **105**.

Pack a Mode 170 drop after build:

```powershell
.\scripts\pack-mode170-zip.ps1
```

---

## How stereo works here (honest)

| Path | Meaning |
|------|---------|
| **Mode 170** | Same-tick **DrawScene×2** with IPD (sequential L then R). Real parallax. Can flicker under motion. |
| **Mode 120** | Cleaner cadence dual (`everyN=2` + HOLD) — former clean LKG |
| **Mode 0** | Mono Submit — one image both eyes |
| **AER / temporal** | Older modes (e.g. 38/51); **not** the Mode 170 product path |
| **AER v2** | Optical-flow synthesis (Luke Ross) — **not implemented** |

True simultaneous same-moment stereo (frozen game time between eyes / SPS) remains an open research item.

---

## Docs

| File | Contents |
|------|----------|
| [docs/CURRENT-STATE.md](docs/CURRENT-STATE.md) | Headset status / next |
| [docs/VR_STRATEGY.md](docs/VR_STRATEGY.md) | Stock DXVK 3.0.2 + interop |
| [docs/CONSTRAINTS.md](docs/CONSTRAINTS.md) | Hard rules |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Modules |
| [docs/BUILD.md](docs/BUILD.md) | Build detail |
| [docs/REFERENCES.md](docs/REFERENCES.md) | Links |
| [CREDITS.md](CREDITS.md) | Credits & licenses |
| [AGENTS.md](AGENTS.md) | Agent / contributor contract |

---

## Credits & sources

See **[CREDITS.md](CREDITS.md)** for the full list. Short version:

- **DXVK** — [doitsujin/dxvk](https://github.com/doitsujin/dxvk) (stock **v3.0.2** deliverable)
- **OpenVR / SteamVR** — [ValveSoftware/openvr](https://github.com/ValveSoftware/openvr)
- **FusionFix** — [ThirteenAG/GTAIV.EFLC.FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix)
- **Architecture inspiration** — [sd805/l4d2vr](https://github.com/sd805/l4d2vr), HL2VR patterns (concept only)
- **Stereo / FOV lessons** — [praydog/UEVR](https://github.com/praydog/UEVR), [Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR), public Luke Ross / R.E.A.L. writeups (**techniques only** — no closed binaries)
- **Historical DXVK VR mailbox** — [TheIronWolfModding/dxvk](https://github.com/TheIronWolfModding/dxvk) (reference; **not** our ship base)
- **MinHook** — TsudaKageyu / related MinHook tree

GTA IV belongs to Rockstar Games / Take-Two. This project does **not** redistribute Rockstar game assets or closed third-party VR binaries.

---

## License

MIT for *our* ASI / scaffolding sources. DXVK, OpenVR, MinHook, FusionFix, and referenced projects keep their own licenses — see [CREDITS.md](CREDITS.md).

---

## Status / invite

Repository may be **private**. A link alone is not enough — ask the owner for a collaborator invite.

**Definition of Done (first milestone, already reached):** flat DXVK 3.0.2 OK + ASI Mono-Submit in SteamVR.  
**Now:** Stability / Stereo / Camera — one behavior change per test build.
