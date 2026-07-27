# Cursor at Home — Complete Guide

This document is enough to continue **gtaiv-dxvk-vr** at home on your own.  
No programming required: Cursor writes the code; you install tools, run builds, and test with the headset.

---

## 0. What you are building

A VR mod for **GTA IV Complete Edition**:

- Game still speaks **Direct3D 9**
- Our **`d3d9.dll`** (32-bit) translates to **Vulkan** and delivers eye images
- **SteamVR / OpenVR** shows the image in the headset (like L4D2VR / HL2VR)

Known good flat: Stock-DXVK **3.0.2** as **`d3d9.dll`**; leave `vulkan.dll` untouched.  
Next: **ASI** with `ID3D9VkInterop*` → Mono in headset (see `docs/VR_STRATEGY.md`).  
Flat lessons: `docs/DXVK_FLAT_TROUBLESHOOT.md`.

Offline already prepared: glue stubs (`src/gtaiv/`), `deploy.ps1`, `docs/IRONWOLF_API.md`, `docs/L4D2VR_MAP.md`.


---

## 1. One-time: install software

**Preferred (new PC):** one script — see **`docs/NEW_PC_SETUP.md`**.

```powershell
cd <path-to>\gtaiv-dxvk-vr
.\scripts\setup-dev-pc.ps1
```

That installs Git / VS2022 C++ / Python (via winget) and fetches MinHook + OpenVR **v2.12.14** + the DXVK submodule.

| Tool | Purpose | Note |
|------|---------|------|
| [Cursor](https://cursor.com) **Desktop** (local agent) | AI coding on this PC | Open this folder — **not** a Cloud Agent for daily build/test |
| [Git for Windows](https://git-scm.com/download/win) | Submodules, commits | "Git from command line" ok |
| [Visual Studio 2022](https://visualstudio.microsoft.com/) | Build C++ x86 | Workload **Desktop development with C++** |
| Vulkan SDK (optional at first) | DXVK build | Add later if build asks |
| Meson / Ninja / Python | DXVK build (depends on fork) | Agent helps with install |
| [Steam](https://store.steampowered.com/) + **SteamVR** | OpenVR runtime | Connect Reverb G2 through it |
| GTA IV **Complete Edition** | Target game | Singleplayer / offline while developing |
| [FusionFix for GTA IV CE](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) | ASI loader + CE fixes | In graphics options **Graphics API = DirectX 9** when our `d3d9.dll` is active (not FusionFix Vulkan) |

**Work on this Windows PC locally.** Cloud Agents cannot install VS/SteamVR or run `GTAIV.exe` on your machine.

---

## 2. Open project in Cursor

1. Start Cursor  
2. **File → Open Folder…**  
3. Choose folder:

```text
C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr
```

4. Wait until Explorer on the left shows this project tree (not `gtaiv-openxr`, not Home).

---

## 3. Start a new chat with the right prompt

Open a new agent chat and paste **exactly this** (also in `AGENTS.md`):

```text
Read AGENTS.md and docs/CURRENT-STATE.md and docs/HOME_CURSOR.md.
Project: gtaiv-dxvk-vr — GTA IV CE VR via VR-DXVK d3d9.dll + OpenVR (SteamVR), Win32, Reverb G2.
Like L4D2VR/HL2VR. No OpenXR for the first milestone.
I am not a programmer — give concrete click/command steps.
Next: initialize submodules and x86 build plan.
```

The agent should then name **one clear next step** (not ten parallel workstreams).

---

## 4. Fetch submodules (internet required)

In Cursor: open Terminal (`` Ctrl+` ``) and:

```powershell
cd C:\Users\Henning\Documents\cursor\gtaiv-dxvk-vr
.\scripts\init-submodules.ps1
```

Success: folder `dxvk\` contains source (not empty).  
On errors: paste full terminal output to the agent.

---

## 5. First build (still without VR logic)

Goal: a **32-bit** `d3d9.dll` that starts GTA IV **flat** (monitor) under FusionFix.

1. Agent: "Write build plan for x86 d3d9.dll from submodule and guide step by step."  
2. Follow the commands / VS steps.  
3. Copy DLL to `GTAIV\` (keep backup of old file).  
4. Turn FusionFix Vulkan toggle **off**, if present.  
5. Start game → if monitor image ok: note in `docs/CURRENT-STATE.md`.

Details: `docs/BUILD.md`.

---

## 6. Headset test loop (from Mono-Submit onward)

Always the same order:

1. Start **SteamVR**, headset detected  
2. One behavior change from agent (only **one** per build)  
3. Rebuild → deploy DLL  
4. Start GTA (offline)  
5. Open log file next to `GTAIV.exe` (name set by agent, e.g. `gtaiv_dxvk_vr.log`)  
6. Paste into chat: log excerpt + "headset: yes/no, what was visible"

Rule: **one change per test** — otherwise you won't know what broke.

---

## 7. What you should never forget to tell the agent

- Game is **32-bit** → everything **x86 / Win32**  
- Compositor = **OpenVR / SteamVR** (not OpenXR)  
- Do **not** drop foreign `d3d9.dll` (L4D2VR release) in blindly  
- No multiplayer during development  
- After tests: have agent update `docs/CURRENT-STATE.md`  

---

## 8. "Home Day 1" checklist

- [ ] Cursor **Desktop** shows folder `gtaiv-dxvk-vr` (local agent, not Cloud)
- [ ] `.\scripts\setup-dev-pc.ps1` succeeded (or VS2022 C++ + Git installed by hand)
- [ ] `.\scripts\build-asi.ps1` produced `out-asi\gtaiv_dxvk_vr.asi`
- [ ] SteamVR + Reverb G2 ok
- [ ] FusionFix in GTA IV CE + stock DXVK 3.0.2 flat ok
- [ ] Chat prompt from section 3 used in a **local** agent
- [ ] Next: deploy ASI / headset test (see `docs/CURRENT-STATE.md`)

---

## 9. If something goes wrong

| Problem | Action |
|---------|--------|
| Wrong folder in Cursor | Open Folder → `Documents\gtaiv-dxvk-vr` |
| Submodule empty | Run script again; paste error message |
| Game won't start after DLL | Restore backup; check FusionFix Vulkan; paste log |
| SteamVR doesn't see headset | Fix SteamVR alone first, then GTA |
| Agent talks about OpenXR | Point to `AGENTS.md` / this doc: **OpenVR only** for v0 |

More context: `docs/FAQ.md`, `docs/CONSTRAINTS.md`, `docs/REFERENCES.md`.
