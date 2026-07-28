# New PC setup — gtaiv-dxvk-vr

Concrete steps for a fresh **Windows** PC. No programming required.

**Work locally on this PC** (Cursor Desktop → local agent). Do not use a Cloud Agent for install/build/headset tests — the cloud VM cannot install Visual Studio or run GTA/SteamVR on your machine.

## A. One command (tools + repo deps)

1. Install [Cursor](https://cursor.com) **Desktop**.
2. **File → Open Folder…** → this repo (on your disk, e.g. `Documents\cursor\gtaiv-dxvk-vr`).
3. Open a **local** agent chat (not Cloud).
4. Open Terminal (`` Ctrl+` ``).
5. Run:

```powershell
cd <path-to>\gtaiv-dxvk-vr
.\scripts\setup-dev-pc.ps1
```

That installs (via winget where missing):

| Tool | Needed for |
|------|------------|
| Git for Windows | submodules / OpenVR / MinHook |
| Visual Studio 2022 Community + **Desktop development with C++** | build `gtaiv_dxvk_vr.asi` (Win32) |
| Python 3 | Meson helper (DXVK source builds) |
| MSYS2 (optional prompt) | rebuild `d3d9.dll` from DXVK source |
| Vulkan SDK (optional prompt) | DXVK shaders |

Then fetches:

- `dxvk` submodule (`tiw-rel` branch)
- `thirdparty\minhook`
- `thirdparty\openvr` **v2.12.14** (matches SteamVR 2.12)

If Git or winget was just installed and PATH is stale: **close the terminal, open a new one**, re-run the script.

## B. Manual (game / headset) — script cannot do this

| Step | What to install / do |
|------|----------------------|
| 1 | [Steam](https://store.steampowered.com/) |
| 2 | SteamVR (Library → Tools) |
| 3 | GTA IV **Complete Edition** |
| 4 | [FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) in the game folder |
| 5 | Stock **DXVK 3.0.2** `d3d9.dll` (**32-bit**) next to `GTAIV.exe` |
| 6 | FusionFix graphics: **DirectX 9** (not FusionFix Vulkan) when our DLL is active |
| 7 | Confirm SteamVR sees the headset (e.g. Reverb G2) **before** starting GTA |

## C. First ASI build

```powershell
.\scripts\build-asi.ps1
```

Success: `out-asi\gtaiv_dxvk_vr.asi` and `out-asi\openvr_api.dll`.

Deploy + start (set your real game path if different):

```powershell
.\scripts\build-deploy-run.ps1
```

Or deps only / rebuild later:

```powershell
.\scripts\fetch-deps.ps1
.\scripts\build-asi.ps1
```

## D. Checklist

- [ ] `.\scripts\setup-dev-pc.ps1` finished without MISS lines
- [ ] `.\scripts\build-asi.ps1` produced `out-asi\gtaiv_dxvk_vr.asi`
- [ ] SteamVR + headset OK alone
- [ ] FusionFix + stock DXVK 3.0.2 flat (monitor) OK
- [ ] ASI deployed; log next to EXE: `gtaiv_dxvk_vr.log`

## E. Not Cloud Agents

Install, build, and headset tests happen **on this Windows PC**.<br>
A Cloud Agent runs on a remote Linux VM — it cannot drive VS Installer, SteamVR, or GTAIV.exe here.

See also: `docs/HOME_CURSOR.md`, `docs/BUILD.md`, `docs/CURRENT-STATE.md`.
