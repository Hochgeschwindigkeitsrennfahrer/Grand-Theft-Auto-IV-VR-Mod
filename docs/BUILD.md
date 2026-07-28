# Build — x86 game components and x64 OpenXR host

## Quest 3 calibration host (x64)

This is separate from `GTAIV.exe`; it does not change the x86 ASI or deploy anything
to the game folder.

```powershell
cd D:\code\gta-iv
.\scripts\build-openxr-host.ps1
```

The build fetches the exact Khronos OpenXR SDK `release-1.1.61`, builds
`out-openxr\gtaiv_xr_host.exe`, verifies PE machine `0x8664`, and runs a shader
self-test plus ABI-v6 mono/stereo mailbox, stationary-UI, aspect, and sRGB checks without
touching the headset runtime.

For the live Quest test, follow [`OPENXR_QUEST3_TEST.md`](OPENXR_QUEST3_TEST.md).

## Direct GTA-to-Quest image test (no SteamVR)

Mode 55 is the guarded FNVVR-style CPU-mailbox mono fallback. Mode 56 uses that same
no-fence transport for a guarded DrawScene x2 left/right pair: the x86 ASI downsizes
each GTA eye render to 1280x720 and writes both into one advancing triple-slot BGRA
transaction. The x64 host ingests them into separate D3D11 textures and accepts world
stereo only with the same-tick DrawScene proof. Pause/map, loading, and phone retain
the stationary local-space quad with original GTA aspect. The host samples GTA pixels
through an sRGB SRV. OpenVR is a delayed fallback import, so `backend=openxr` does not
load `openvr_api.dll` or start SteamVR.

The x86 build also compiles and runs `gtaiv_stereo_wvp_test.exe`. It checks row-vector
WVP math, inverse residuals, singular/NaN rejection, exact CTAB parsing, and rejection
of mismatched or partially patched draw-pair audits. A second x86 test checks all
Touch-to-XInput mappings and haptic motor routing. These tests do not load GTA,
OpenXR, OpenVR, Steam, or a headset runtime.

```powershell
cd D:\code\gta-iv
.\scripts\run-openxr-gta.ps1 -Build
```

`PREFLIGHT PASS` starts no process and changes no game file. Direct GTA/OpenXR launch
remains quarantined while the replacement transport awaits review and one controlled
test; this script cannot start Steam, SteamVR, GTA, or the OpenXR host.

---

## Legacy x86 DXVK notes

Goal step 1: **32-bit** `d3d9.dll` from the IronWolf submodule that starts GTA IV CE **flat** (monitor) under FusionFix.  
**No** OpenVR submit in this step yet.

Submodule branch: `tiw-rel-241-260612` (see `.gitmodules`).  
Compositor later: **OpenVR / SteamVR** — not OpenXR.

---

## 0. One-time: check tools

| Tool | Required? | Check in PowerShell |
|------|-----------|---------------------|
| Git for Windows | yes (submodules) | `git --version` |
| Visual Studio 2022 + **Desktop development with C++** | recommended | VS Installer |
| [MSYS2](https://www.msys2.org/) | yes (DXVK builds with MinGW/Meson) | Start menu → **MSYS2 UCRT64** |
| Vulkan SDK | often needed (glslang) | `C:\VulkanSDK\...` |
| Python 3 | often already with Meson | `python --version` |

If `git` is not found: install [Git for Windows](https://git-scm.com/download/win), open a **new** Cursor/PowerShell tab, check again.

---

## 1. Fetch submodules (internet)

In Cursor: Terminal `` Ctrl+` ``, then:

```powershell
cd C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr
.\scripts\init-submodules.ps1
```

**Success:** folder `dxvk\src` exists and is not empty.  
Also verify:

```powershell
Test-Path .\dxvk\src\d3d9\d3d9_vr.h
```

Must be `True`. If `False`: wrong branch / empty submodule → paste full terminal output to the agent.

---

## 2. MSYS2 packages (one-time)

`build-win32.txt` expects a compiler named `i686-w64-mingw32-gcc` → use the **MINGW32** environment for that.

1. Install **MSYS2**: https://www.msys2.org/ (default path `C:\msys64` ok).  
2. Start menu → open **"MSYS2 MINGW32"** (not UCRT64 for this build).  
3. Update:

```bash
pacman -Syu
```

Window may close → open **MINGW32** again, possibly run `pacman -Syu` once more.

4. Build tools (**without** `i686-glslang` — package no longer exists):

In **MINGW32**:

```bash
pacman -S --needed \
  mingw-w64-i686-gcc \
  mingw-w64-i686-meson \
  mingw-w64-i686-ninja \
  mingw-w64-i686-pkgconf \
  git
```

In **MINGW64** (separate window — only for shader tool):

```bash
pacman -S --needed mingw-w64-x86_64-glslang
```

5. Quick check (back in **MINGW32**):

```bash
export PATH="/mingw32/bin:/mingw64/bin:$PATH"
gcc -dumpmachine
which glslangValidator || which glslang
meson --version
ninja --version
```

Expected: `gcc -dumpmachine` contains `i686`; `glslangValidator` found.  
If `i686-w64-mingw32-gcc` missing but `gcc` present: paste `which gcc` + `gcc -dumpmachine`.

---

## 3. x86 build (Meson + Ninja)

In **MSYS2 UCRT64** (adjust paths if your username differs):

```bash
cd /c/Users/Henning/Documents/cursor/gtaiv-dxvk-vr/dxvk

# Build directory for Win32 only
# MSYS2: scripts/msys2-win32.txt (not IronWolf build-win32.txt — ar names)
meson setup \
  --cross-file ../scripts/msys2-win32.txt \
  --buildtype release \
  --prefix "$PWD/../out-win32" \
  build.w32

cd build.w32
ninja
ninja install
```

**Expected DLL:**

```text
C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr\out-win32\bin\d3d9.dll
```

(Exact subfolder may be `bin` or similar — after `ninja install` open `out-win32` in Explorer and search for `d3d9.dll`.)

### If `build-win32.txt` is missing or fails

1. In folder `dxvk\` look for `build-win*.txt` / `BUILD` / `README`.  
2. Paste full error output to the agent.  
3. Alternative reference (read only, do not copy their DLL to GTA):  
   [L4D2VR Build](https://github.com/sd805/l4d2vr) → `l4d2vr.sln` → platform **x86**.

---

## 4. Verify: really 32-bit?

In PowerShell (Developer PowerShell for VS or normal PowerShell with VS tools):

```powershell
dumpbin /headers "C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr\out-win32\bin\d3d9.dll" | Select-String "machine"
```

Expected: anything with **`8664` is wrong (x64)** — want **`14C` / x86**.  
Without `dumpbin`: agent can add a small check script later; roughly: file must **not** come from an `x64`/`win64` build.

---

## 5. Deploy to GTA IV

Game folder = folder containing `GTAIV.exe` (often `...\Grand Theft Auto IV\GTAIV`).

```powershell
cd C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr
.\scripts\deploy.ps1 `
  -GameDir "D:\Steam\steamapps\common\Grand Theft Auto IV\GTAIV" `
  -DllPath ".\out-win32\bin\d3d9.dll"
```

Set `-GameDir` to **your** actual path.

Then:

1. FusionFix **Vulkan/DXVK toggle off**  
2. Start game **offline / Singleplayer**  
3. Monitor image ok? → success for "flat" step  
4. On problems paste log next to EXE (DXVK often creates `GTAIV_d3d9.log` or similar)  

Rollback:

```powershell
copy "…\GTAIV\d3d9.dll.gtaiv-dxvk-vr.bak" "…\GTAIV\d3d9.dll"
```

---

## 6. Order after that (not yet)

1. Flat DLL ok  
2. Read L4D2VR `vr.cpp` submit pattern (`docs/L4D2VR_MAP.md`)  
3. Glue to `GetVRDesc` + `IVRCompositor::Submit`  
4. SteamVR + Reverb G2 → **Mono** in headset  

One behavior change per test build.

---

## Conflicts / pitfalls

| Problem | Action |
|---------|--------|
| `git` unknown | Install Git, new terminal |
| `dxvk\src` empty | Run `init-submodules.ps1` again; paste log |
| `d3d9_vr.h` missing | Branch must be `tiw-rel-*`, not `master` |
| Game won't start | Restore backup; FusionFix Vulkan off; paste log |
| x64 DLL in GTA | Rebuild with `build-win32` / i686 |
| Antivirus | May need to allow new `d3d9.dll` |

See also `config/dxvk.conf.example`, `docs/HOME_CURSOR.md`, `docs/CONSTRAINTS.md`.
