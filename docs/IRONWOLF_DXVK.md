# IronWolf DXVK — what it does & how we use it for GTA IV

Upstream: [TheIronWolfModding/dxvk](https://github.com/TheIronWolfModding/dxvk)  
Also used by L4D2VR / related Source VR work; openRBRVR builds on the same family of ideas.

---

## Branches (important — there is **no** `openxr` branch)

| Branch / line | Role |
|---------------|------|
| [`master`](https://github.com/TheIronWolfModding/dxvk/tree/master) | Default GitHub branch + fork README. Newer DXVK base / GTR2 notes. **Does not ship `src/d3d9/d3d9_vr.h`** (mailbox API missing). `dxvk_openxr.*` / `dxvk_openvr.*` here are mostly upstream Wine **extension providers**, not the game submit mailbox. |
| [`vr-dx9-rel`](https://github.com/TheIronWolfModding/dxvk/tree/vr-dx9-rel) | **Older** VR mailbox (OpenVR-era `IDirect3DVR9`). What L4D2VR/sd805 leaned on. Author: Vulkan 1.2 / fewer features. |
| [`tiw-rel-241-*`](https://github.com/TheIronWolfModding/dxvk/tree/tiw-rel-241-260612) | **Current** VR-capable release line (DXVK ~2.4.1 / Vulkan 1.3). Has `d3d9_vr.h` with OpenVR helpers **and** `GetOXRVkDeviceDesc` (OpenXR), plus Multi-View/SPS (detegr). Prefer this for Phase B. |

OpenXR is a **feature inside `tiw-rel-*` / newer READMEs**, not a separate Git branch named `openxr`.

---

## In plain language

Normal DXVK = “Game thinks it is Direct3D 9, GPU actually gets Vulkan” → better performance on the **monitor**.

IronWolf’s **VR lines** add: “And if the **game** asks for VR, hand Vulkan eye images to **OpenVR / OpenXR**.”

It is a **translator + VR mailbox**, not a complete GTA VR mod by itself.

Author’s own warning (README / repo):

> Game-side changes needed… dropping this `.dll` into a random game **won’t magically** add Vulkan VR.

---

## What he changed (the useful bits)

| Change | Meaning for humans |
|--------|---------------------|
| **DX9 → Vulkan OpenVR** (Joshua Ashton lineage) | `IDirect3DVR9` / `GetVRDesc` so a mod can `Submit` **Vulkan** textures to the headset. |
| **OpenXR helpers** (on `tiw-rel-*`, e.g. `GetOXRVkDeviceDesc`) | Same mailbox idea for OpenXR (important for us + Reverb G2). **Not** a separate branch. |
| **Multi-view / stereo helpers** (detegr / RBR) | Draw stereo more efficiently; game still drives eyes / SPS. |
| **GTR2-specific hacks** | Racing-engine tweaks — **ignore for GTA** (`GTR2_SPECIFIC`). |
| Extra AA / depth options | Nice-to-have; not the VR core. |

### The VR “handshake” (conceptual)

Game / mod code roughly does:

1. Init VR runtime (OpenVR or OpenXR).  
2. Ask DXVK for Vulkan image info for the eye texture (`GetVRDesc`-style).  
3. Tell DXVK “about to submit” (`Presubmit`) → lock/sync.  
4. `IVRCompositor::Submit` / `xrEndFrame` with those Vulkan images.  
5. `Postsubmit` → unlock.

So **who decides “this is the left eye frame”?** → still the **game or our glue**, not IronWolf alone.

---

## How L4D2VR uses it

L4D2VR = **Source hooks** (camera, weapons, 6DoF) **+** VR-oriented DXVK as `d3d9.dll`.

- DXVK: draw path + Vulkan eye buffers + submit plumbing.  
- L4D2VR code: when to render each eye, head pose into the engine, controls.

Same split we must respect for GTA IV.

---

## How we apply this to GTA IV (Phase B)

```
GTAIV.exe (still talks D3D9)
    │
    ▼
d3d9.dll = IronWolf/openRBRVR-class VR-DXVK (Win32)   ← translator + VR mailbox
    │
    ▼
Our GTA glue (ASI and/or code inside/beside the fork)
    • head pose → camera
    • left/right (or mono) eye targets
    • call Presubmit / GetVRDesc / Submit / Postsubmit
    • later: motion controls
    │
    ▼
OpenXR (prefer WMR on Reverb G2)  or  OpenVR if needed
```

### Practical steps for `gtaiv-dxvk-vr`

1. **Base fork:** start from IronWolf **`tiw-rel-241-*`** (not bare `master`) **or** [dxvk-openRBRVR](https://github.com/Detegr/dxvk-openRBRVR). Prefer OpenXR for G2.  
2. Build **x86** `d3d9.dll`.  
3. Prove flat boot under FusionFix (no VR yet).  
4. Add **minimal GTA glue**: mono “mirror game backbuffer → one/both eyes → Submit” (same milestone as Phase A’s first pixels).  
5. Only then stereo + camera RE.  
6. Keep **async/gplasync** ideas for stutter (HL2VR lesson) if the fork allows.

### What we do **not** do

- Drop IronWolf DLL into GTA and expect VR.  
- Port GTR2-only flags.  
- Assume SteamVR OpenXR works in Win32 (use WMR first — Phase A lesson).

---

## Phase A vs this fork

| | Phase A (`gtaiv-openxr`) | Phase B + IronWolf |
|--|-------------------------|---------------------|
| Where VR lives | Our ASI + companion D3D11 | Inside/next to VR-DXVK |
| Submit API | OpenXR D3D11 swapchains | OpenVR/OpenXR **Vulkan** images via DXVK |
| First pixels | D3D9 blit → D3D11 → XR | D3D9→VK inside DXVK → Submit |

Same **product** goal; different **mailbox**. IronWolf teaches the mailbox shape for Phase B.
