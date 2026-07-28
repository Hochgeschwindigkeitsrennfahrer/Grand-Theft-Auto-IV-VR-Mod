# gtaiv-dxvk-vr

Standalone project: **GTA IV Complete Edition in VR** — stock DXVK **3.0.2** +
Win32 ASI glue. The proven baseline uses **OpenVR/SteamVR**; the post-v0 target adds a
separate **x64 OpenXR host** for Quest 3 while preserving OpenVR/Reverb G2 fallback.

```
GTAIV.exe (32-bit, D3D9)
  → d3d9.dll              (DXVK 3.0.2 — flat / desktop OK)
  → gtaiv_dxvk_vr.asi     (x86 GTA hooks + stereo capture)
      ├─ fallback: OpenVR Submit via ID3D9VkInterop → SteamVR
      └─ direct: x86 CPU mailbox → gtaiv_xr_host.exe (x64) → Meta OpenXR
```

**Status (WIP):** the latest clean upstream camera/stereo release through Modes
**120–136** is integrated as the explicit OpenVR fallback. Direct-OpenXR Mode **57**
now has a complete headless in-game proof: temporal, pixel-distinct L/R frames,
captured per-eye poses, sustained fully loaded gameplay, stationary menu/pause quad,
sRGB color, Start/A, locomotion/neutral, and pose sweep. The x86 ASI never
initializes SteamVR on this route; the separate x64 host owns OpenXR.

Mode 57 is deliberately temporal stereo, not same-tick stereo. Mode **56** was
rejected as the current candidate because its nominal DrawScene x2 outputs remained
byte-identical. Mode **55** remains the center-eye mono fallback. Real Quest 3
visual/comfort acceptance is still pending, and the installed game remains
`backend=off`, `stereo=0` after every supervised run. See
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
| [docs/ELLIOTT_OPENXR_SIMULATOR_PROOF.md](docs/ELLIOTT_OPENXR_SIMULATOR_PROOF.md) | Headless full-game stereo/input proof |
| [docs/META_XR_SIMULATOR_TEST.md](docs/META_XR_SIMULATOR_TEST.md) | No-headset stereo/input test through Meta XR Simulator |
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
| `scripts/build-deploy-run.ps1` | Legacy OpenVR-only build/deploy/Steam launch; never use for direct OpenXR |
| `scripts/build-openxr-host.ps1` | Fetch pinned Khronos SDK, build and verify the x64 host |
| `scripts/run-openxr-gta.ps1` | Safe direct-OpenXR preflight only; launches nothing |
| `scripts/run-openxr-calibration.ps1` | Run the generated Quest/OpenXR eye test |
| `scripts/openxr-runtime-info.ps1` | Classify Quest Link, Meta XR Simulator, SteamVR, or other runtime |
| `scripts/setup-elliott-openxr-simulator.ps1` | Build the pinned headless/file-IPC Elliott simulator package |
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
