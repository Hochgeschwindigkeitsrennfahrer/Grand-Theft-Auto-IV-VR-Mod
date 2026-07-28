# gtaiv-dxvk-vr

Standalone project: **GTA IV Complete Edition in VR** — stock DXVK **3.0.2** +
Win32 ASI glue. The current primary path is direct **OpenXR** through a separate
x64 host. OpenVR/SteamVR is retained as the Reverb G2 fallback and as development
history.

```
GTAIV.exe (32-bit, D3D9)
  → d3d9.dll              (DXVK 3.0.2 — flat / desktop OK)
  → gtaiv_dxvk_vr.asi     (x86 GTA hooks + stereo capture)
      ├─ primary: x86 CPU mailbox → gtaiv_xr_host.exe (x64) → OpenXR
      └─ fallback: OpenVR Submit via ID3D9VkInterop → SteamVR
```

**Current status:** direct-OpenXR Mode **58** is the proved primary. The supervised
Elliott run `20260728-150553-steam-safe` completed `PROOF_COMPLETE` in full GTA
gameplay with a hard first-person camera, native head hide, controller input,
stationary menu/pause quad, sRGB swapchain format `29`, and a world → pause UI →
world round trip. Mode 58 minimally reuses upstream Mode 204's same-tick parent
dual at `0x4DE020`; the run accepted 6,240 fused pairs at a 9.1 cm camera baseline
and moved the player about 7.1 m.

The matching 12-second composed OpenXR SBS video contains 713 frames; all 120
sampled frames are unique and freeze detection found zero events. Tested SHA-256:
ASI `39F9D931D95998BEB6A7111DF33ABC9661B2466B9AEA4BB3CA44B9192C3C9F4A`,
host `22272C3B7EA21396C39438CD351344C0C26253DAF18888060D5B04661F6ECEBC`,
video `CE09CAF99F8CF51272E89C70A412C6A4B9CEF30FEE19A131AF08DD16F55F998D`.
Fetched `origin/master` is `715d40f` (Mode 204) and is contained by this work.

The x86 ASI does not initialize SteamVR on the OpenXR route; the x64 host owns the
runtime. Mode **57** remains the temporal OpenXR diagnostic fallback, Mode **55**
the center-eye mono fallback, and the imported OpenVR modes remain available for
Reverb G2. Real Quest 3 visual/comfort acceptance is still pending. Every
supervised run restores the installed game to `backend=off`, `stereo=0`. See
[`docs/CURRENT-STATE.md`](docs/CURRENT-STATE.md) and
[`docs/ELLIOTT_OPENXR_SIMULATOR_PROOF.md`](docs/ELLIOTT_OPENXR_SIMULATOR_PROOF.md).

The separate x64 OpenXR calibration host now builds and reaches the installed Meta
Quest 3 runtime. Build it with `.\scripts\build-openxr-host.ps1`, then follow
[`docs/OPENXR_QUEST3_TEST.md`](docs/OPENXR_QUEST3_TEST.md) for the headset gate.
Meta XR Simulator 205.0 is also supported as a no-headset OpenXR route; follow
[`docs/META_XR_SIMULATOR_TEST.md`](docs/META_XR_SIMULATOR_TEST.md).
For the direct host-to-GTA HMD and Quest Touch controller log gate, use
[`docs/OPENXR_POSE_TOUCH_TEST.md`](docs/OPENXR_POSE_TOUCH_TEST.md).

This repository is **private**. A link alone is not enough — invite people as collaborators (see below).

---

## Quick start (build ASI) — local Windows PC

Daily work is **on your Windows PC** (Cursor Desktop local agent + VS2022). Not a Cloud Agent.

1. Clone with submodule:
   ```powershell
   git clone --recurse-submodules https://github.com/Hochgeschwindigkeitsrennfahrer/gtaiv-dxvk-vr.git
   cd gtaiv-dxvk-vr
   ```
2. New PC / missing tools — one script (Git, VS2022 C++, MinHook, OpenVR **v2.12.14**):
   ```powershell
   .\scripts\setup-dev-pc.ps1
   ```
   Details: [`docs/NEW_PC_SETUP.md`](docs/NEW_PC_SETUP.md). Or fetch dependencies
   only with `.\scripts\fetch-deps.ps1`.
3. Build only (no deployment or launch):
   ```powershell
   .\scripts\build-asi.ps1
   .\scripts\build-openxr-host.ps1
   ```
4. No headset runtime is required for these offline builds.
## Imported upstream OpenVR history

The material below records the upstream OpenVR line that culminated in Mode
204. It is retained for provenance and fallback use. It is not the current
direct-OpenXR launch path or acceptance result.

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

## Historical OpenVR snapshot — Mode **170** (`clean/mode-170`)

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

## Historical OpenVR requirements (fallback only)

1. **GTA IV Complete Edition** (Steam) — 32-bit `GTAIV.exe`
2. **[FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix)** (ASI loader + CE fixes). Graphics API = **DirectX 9** (not FusionFix Vulkan)
3. **SteamVR** running; headset tracked (Reverb G2 via WMR → SteamVR is fine)
4. **Singleplayer / offline** while developing
5. Optional build PC: VS 2022 C++ x86 tools, PowerShell, Git

**Still-current hard rule:** everything that loads into `GTAIV.exe` is **Win32 /
x86**. OpenXR therefore lives in the separate x64 host. The statement that v0 was
OpenVR-only is historical; Mode 58 is now the primary proved OpenXR path. Do not
drop a foreign game's `d3d9.dll` and call it GTA VR.

---

## Historical OpenVR quick install (prebuilt Mode 170)

1. Install FusionFix into the GTAIV folder (official release).
2. Close GTA IV.
3. Copy everything from [`prebuilt/mode170/drop-into-GTAIV/`](prebuilt/mode170/drop-into-GTAIV/) into the folder that contains `GTAIV.exe` (overwrite VR files when asked).
4. Start **SteamVR**, put the headset on, then launch GTA IV (offline / SP).
5. Check `gtaiv_dxvk_vr.log` next to the EXE:
   - `VR_Init OK`
   - `StereoMode: 170`
   - `StereoSubmit: L=1 R=1 mode=170`

Full steps: [`prebuilt/mode170/INSTALL.txt`](prebuilt/mode170/INSTALL.txt).

**Kill VR:** `gtaiv_dxvk_vr.stereo` → `0` (flat DXVK on monitor).<br>
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

## Historical OpenVR stereo modes

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
| [docs/CURRENT-STATE.md](docs/CURRENT-STATE.md) | Current status / what works |
| [docs/VR_STRATEGY.md](docs/VR_STRATEGY.md) | Strategy |
| [docs/FULL_VR_PLAN.md](docs/FULL_VR_PLAN.md) | Quest 3 OpenXR-primary plan and acceptance gates |
| [docs/OPENXR_QUEST3_TEST.md](docs/OPENXR_QUEST3_TEST.md) | Concrete Quest 3 calibration-host test |
| [docs/ELLIOTT_OPENXR_SIMULATOR_PROOF.md](docs/ELLIOTT_OPENXR_SIMULATOR_PROOF.md) | Headless full-game stereo/input proof |
| [docs/META_XR_SIMULATOR_TEST.md](docs/META_XR_SIMULATOR_TEST.md) | No-headset stereo/input test through Meta XR Simulator |
| [docs/VR_MOD_PLAYBOOK.md](docs/VR_MOD_PLAYBOOK.md) | BotW / L4D2VR orientation |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Technical architecture |
| [docs/BUILD.md](docs/BUILD.md) | Build notes |
| [docs/HOME_CURSOR.md](docs/HOME_CURSOR.md) | Cursor setup (German) |
| [AGENTS.md](AGENTS.md) | Agent contract |
| [HANDOFF.md](HANDOFF.md) | Short handoff |
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

| Script | Purpose |
|--------|---------|
| `scripts/setup-dev-pc.ps1` | **New Windows PC:** winget tools + fetch deps |
| `scripts/fetch-deps.ps1` | Submodule + MinHook + OpenVR v2.12.14 |
| `scripts/fetch-openvr.ps1` | Clone pinned OpenVR |
| `scripts/build-asi.ps1` | Build `gtaiv_dxvk_vr.asi` (Win32) |
| `scripts/deploy-asi.ps1` | Copy ASI into game dir (`-Launch` optional) |
| `scripts/build-deploy-run.ps1` | Legacy OpenVR-only build/deploy/Steam launch; never use for direct OpenXR |
| `scripts/build-openxr-host.ps1` | Fetch pinned Khronos SDK, build and verify the x64 host |
| `scripts/run-openxr-gta.ps1` | Safe direct-OpenXR preflight only; launches nothing |
| `scripts/run-openxr-calibration.ps1` | Run the generated Quest/OpenXR eye test |
| `scripts/openxr-runtime-info.ps1` | Classify Quest Link, Meta XR Simulator, SteamVR, or other runtime |
| `scripts/setup-elliott-openxr-simulator.ps1` | Build the pinned headless/file-IPC Elliott simulator package |
| `scripts/fetch-minhook.ps1` | Clone MinHook into `thirdparty/minhook` |
| `scripts/restart-gtaiv.ps1` | Quick restart via Steam |
| `scripts/deploy.ps1` | Deploy a `d3d9.dll` (+ backup) |
See **[CREDITS.md](CREDITS.md)** for the full dependency and license list.

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

**Earlier milestone:** flat DXVK 3.0.2 OK + ASI Mono-Submit in SteamVR.

**Current proved primary:** direct-OpenXR Mode 58 first-person same-tick parent-dual
gameplay. Next acceptance gate: real Quest 3 visual comfort and scale.
