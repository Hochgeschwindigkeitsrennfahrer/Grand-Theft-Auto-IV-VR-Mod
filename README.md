# gtaiv-dxvk-vr

Standalone project: **GTA IV Complete Edition in VR** — stock DXVK **3.0.2** +
Win32 ASI glue. The proven baseline uses **OpenVR/SteamVR**; the post-v0 target adds a
separate **x64 OpenXR host** for Quest 3 while preserving OpenVR/Reverb G2 fallback.

```
GTAIV.exe (32-bit, D3D9)
  → d3d9.dll              (DXVK 3.0.2 — flat / desktop OK)
  → gtaiv_dxvk_vr.asi     (x86 GTA hooks + stereo capture)
      ├─ current: OpenVR Submit via ID3D9VkInterop → SteamVR
      └─ target: shared pose/GPU bridge → gtaiv_xr_host.exe (x64) → OpenXR
```

**Status (WIP):** Mono and angle-correct temporal stereo work in SteamVR; latest code
contains same-frame stereo Mode 24, which still needs its headset decision test. See
`docs/CURRENT-STATE.md` and the full Quest 3 plan in `docs/FULL_VR_PLAN.md`.
Daily-driver baseline: stereo mode **0** (~90 FPS). Kill-switch file: `gtaiv_dxvk_vr.stereo` → `0`.

The separate x64 OpenXR calibration host now builds and reaches the installed Meta
Quest 3 runtime. Build it with `.\scripts\build-openxr-host.ps1`, then follow
[`docs/OPENXR_QUEST3_TEST.md`](docs/OPENXR_QUEST3_TEST.md) for the headset gate.

This repository is **private**. A link alone is not enough — invite people as collaborators (see below).

---

## Quick start (build ASI)

1. Clone with submodule:
   ```powershell
   git clone --recurse-submodules https://github.com/Hochgeschwindigkeitsrennfahrer/gtaiv-dxvk-vr.git
   cd gtaiv-dxvk-vr
   ```
2. Fetch dependencies (not vendored in git):
   ```powershell
   .\scripts\fetch-minhook.ps1
   git clone --depth 1 https://github.com/ValveSoftware/openvr.git thirdparty\openvr
   ```
3. Build + deploy + launch (default game dir is Steam GTAIV):
   ```powershell
   .\scripts\build-deploy-run.ps1
   ```
4. Requirements: SteamVR running, stock DXVK **3.0.2** `d3d9.dll` in the game folder, FusionFix ASI loader.

Log file (next to the game EXE): `gtaiv_dxvk_vr.log`

---

## Stereo modes (`gtaiv_dxvk_vr.stereo`)

| Mode | Meaning |
|------|---------|
| **0** | Off — mono Submit, best FPS (~90) |
| 4 / 13 | Temporal L/R (real parallax; ~half FPS) |
| 7 | Same-frame dual (experimental) |
| 11 | **Do not use** (blackscreen / freeze) |

Hotkeys: **F6** stereo scale, **F7** world scale (VRScale), **F8** eye separation, **F9** recenter, **F10** 6DoF reset.

Details: [`docs/CURRENT-STATE.md`](docs/CURRENT-STATE.md), [`docs/STEREO_EYE_OFFSET.md`](docs/STEREO_EYE_OFFSET.md), [`docs/VR_MOD_PLAYBOOK.md`](docs/VR_MOD_PLAYBOOK.md).

---

## Docs

| File | Contents |
|------|----------|
| [docs/CURRENT-STATE.md](docs/CURRENT-STATE.md) | Current status / what works |
| [docs/VR_STRATEGY.md](docs/VR_STRATEGY.md) | Strategy |
| [docs/FULL_VR_PLAN.md](docs/FULL_VR_PLAN.md) | Quest 3 OpenXR-primary plan and acceptance gates |
| [docs/OPENXR_QUEST3_TEST.md](docs/OPENXR_QUEST3_TEST.md) | Concrete Quest 3 calibration-host test |
| [docs/VR_MOD_PLAYBOOK.md](docs/VR_MOD_PLAYBOOK.md) | BotW / L4D2VR orientation |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Technical architecture |
| [docs/BUILD.md](docs/BUILD.md) | Build notes |
| [docs/HOME_CURSOR.md](docs/HOME_CURSOR.md) | Cursor setup (German) |
| [AGENTS.md](AGENTS.md) | Agent contract |
| [HANDOFF.md](HANDOFF.md) | Short handoff |

---

## Scripts

| Script | Purpose |
|--------|---------|
| `scripts/build-asi.ps1` | Build `gtaiv_dxvk_vr.asi` (Win32) |
| `scripts/deploy-asi.ps1` | Copy ASI into game dir (`-Launch` optional) |
| `scripts/build-deploy-run.ps1` | Build → stop game → deploy → Steam launch |
| `scripts/build-openxr-host.ps1` | Fetch pinned Khronos SDK, build and verify the x64 host |
| `scripts/run-openxr-calibration.ps1` | Run the generated Quest/OpenXR eye test |
| `scripts/fetch-minhook.ps1` | Clone MinHook into `thirdparty/minhook` |
| `scripts/restart-gtaiv.ps1` | Quick restart via Steam |
| `scripts/deploy.ps1` | Deploy a `d3d9.dll` (+ backup) |

---

## Invite someone (private repo)

GitHub private repos are **not** “unlisted with a link”. Viewers need an invite:

1. Open: https://github.com/Hochgeschwindigkeitsrennfahrer/gtaiv-dxvk-vr/settings/access  
2. Click **Add people**  
3. Enter their GitHub username or email  
4. Choose a role (usually **Write** if they should push, else **Read**)  
5. They accept the email / notification invite  

Or with GitHub CLI (your machine):

```powershell
gh api -X PUT repos/Hochgeschwindigkeitsrennfahrer/gtaiv-dxvk-vr/collaborators/THEIR_USERNAME -f permission=push
```

---

## License

MIT for *our* scaffolding / ASI sources. DXVK, OpenVR, MinHook, and any referenced mods keep their own licenses.
