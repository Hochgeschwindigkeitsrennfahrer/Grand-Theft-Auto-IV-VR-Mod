# L4D2VR — what we borrow (file map)

Repo: https://github.com/sd805/l4d2vr  
DXVK submodule there: https://github.com/sd805/dxvk @ `d856042…`

**Do not** drop their release `d3d9.dll` into GTA. Only read patterns and rewrite for GTA.

---

## Repo structure (relevant)

| Path | Role |
|------|--------|
| `l4d2vr.sln` | VS solution, **x86** |
| `dxvk/` | VR DXVK (IronWolf-based + L4D2 hacks + async) |
| `L4D2VR/` | Game hooks (Source) + OpenVR usage |
| `thirdparty/` | incl. OpenVR |

---

## Specifically in `L4D2VR/` (filenames)

| File | Typical role |
|-------|----------------|
| [`vr.cpp`](https://github.com/sd805/l4d2vr/blob/main/L4D2VR/vr.cpp) / `vr.h` | OpenVR init, submit, poses — **main place to study** |
| [`hooks.cpp`](https://github.com/sd805/l4d2vr/blob/main/L4D2VR/hooks.cpp) / `hooks.h` | Engine/D3D hooks (Source-specific) |
| [`game.cpp`](https://github.com/sd805/l4d2vr/blob/main/L4D2VR/game.cpp) / `game.h` | Game state |
| [`dllmain.cpp`](https://github.com/sd805/l4d2vr/blob/main/L4D2VR/dllmain.cpp) | DLL entry |
| `offsets.h`, `sigscanner.h` | Source offsets — **do not** reuse for GTA |
| `SteamVRActionManifest/` | SteamVR input manifest |
| `manifest.vrmanifest` | SteamVR app manifest |

At home, read **`vr.cpp`** first (submit), then decide where our GTA glue attaches (ASI beside DXVK or code in the fork).

---

## What to search for (when repo is cloned)

In folder `L4D2VR/`, grep for typical symbols:

| Search term | Why |
|-------------|--------|
| `VR_Init` / `VR_Shutdown` | Start OpenVR |
| `IVRCompositor` / `Submit` | Image to headset |
| `GetVRDesc` / `Direct3DCreateVRImpl` / `IDirect3DVR9` | IronWolf mailbox |
| `TextureType_Vulkan` / `VRVulkanTextureData` | Submit payload |
| `TrackedDevicePose` / `GetEyeToHeadTransform` | Pose (camera later) |

In folder `dxvk/`:

| Search term | Why |
|-------------|--------|
| `d3d9_vr` | Mailbox implementation |
| `async` | Stutter patches (optional to adopt) |
| Viewport / resolution override | **L4D2-specific** — do not blindly copy to GTA |

---

## What we adopt vs. discard

| Adopt | Discard / rewrite for GTA |
|------------|-------------------------|
| OpenVR init + submit order | Source engine hooks |
| `GetVRDesc` → Vulkan texture → `Submit` | L4D2 HUD/menu/viewport hacks in DXVK |
| x86 `d3d9.dll` as deliverable | VAC/`-insecure` assumptions |
| Async idea | GTR2/IronWolf race flags |

---

## Recommended order at home

1. `git clone --recurse-submodules https://github.com/sd805/l4d2vr.git` (read only)  
2. Find submit path in `L4D2VR/` and add notes here (filename + function)  
3. In parallel: build our `dxvk` submodule (`init-submodules.ps1`)  
4. Our glue in `src/gtaiv/` following the same submit pattern  

See also `docs/IRONWOLF_API.md`, `docs/HOME_CURSOR.md`.
