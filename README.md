# gtaiv-dxvk-vr

Standalone project: **GTA IV Complete Edition in VR** — stock DXVK **3.0.2** + ASI glue + **OpenVR (SteamVR)** (inspired by L4D2VR / HL2VR patterns).

```
GTAIV.exe (32-bit, D3D9)
  → d3d9.dll              (DXVK 3.0.2 — flat / desktop OK)
  → gtaiv_dxvk_vr.asi     (OpenVR Submit via ID3D9VkInterop)
  → SteamVR → headset (e.g. HP Reverb G2)
```

**Status (WIP):** Mono image in SteamVR works. Stereo modes are experimental (see `docs/CURRENT-STATE.md`).  
Daily-driver baseline: stereo mode **0** (~90 FPS). Kill-switch file: `gtaiv_dxvk_vr.stereo` → `0`.

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
   Details: [`docs/NEW_PC_SETUP.md`](docs/NEW_PC_SETUP.md). Or deps only: `.\scripts\fetch-deps.ps1`
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
| `scripts/setup-dev-pc.ps1` | **New Windows PC:** winget tools + fetch deps |
| `scripts/fetch-deps.ps1` | Submodule + MinHook + OpenVR v2.12.14 |
| `scripts/fetch-openvr.ps1` | Clone pinned OpenVR |
| `scripts/build-asi.ps1` | Build `gtaiv_dxvk_vr.asi` (Win32) |
| `scripts/deploy-asi.ps1` | Copy ASI into game dir (`-Launch` optional) |
| `scripts/build-deploy-run.ps1` | Build → stop game → deploy → Steam launch |
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
