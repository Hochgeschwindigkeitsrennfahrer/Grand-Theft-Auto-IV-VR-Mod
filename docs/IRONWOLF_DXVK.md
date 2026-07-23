# IronWolf DXVK — explained

Upstream: [TheIronWolfModding/dxvk](https://github.com/TheIronWolfModding/dxvk)

---

## Branches (important: no branch named `openxr`)

| Branch | Role |
|-------|--------|
| [`master`](https://github.com/TheIronWolfModding/dxvk/tree/master) | GitHub default. **No** game VR mailbox (`d3d9_vr.h` missing). |
| [`vr-dx9-rel`](https://github.com/TheIronWolfModding/dxvk/tree/vr-dx9-rel) | Older **OpenVR** mailbox. Base for L4D2VR/sd805. |
| [`tiw-rel-241-*`](https://github.com/TheIronWolfModding/dxvk/tree/tiw-rel-241-260612) | Newer VR line. OpenVR + optional OpenXR helpers. For us: use **OpenVR only**. |

---

## In plain terms

Normal DXVK: game thinks D3D9 → GPU gets Vulkan → **monitor**.

IronWolf VR lines: additionally a **mailbox** so a mod can give Vulkan eye images to **OpenVR** (SteamVR).

The DLL alone is **not** a finished VR mod. Author:

> Game-side changes needed… dropping this `.dll` into a random game **won't magically** add Vulkan VR.

---

## Useful changes

| Change | Meaning |
|----------|-----------|
| DX9→Vulkan OpenVR mailbox | `IDirect3DVR9` / `GetVRDesc` … |
| OpenXR helpers (tiw-rel) | Optional; **this project v0 does not use them** |
| Multi-View (detegr) | Stereo efficiency; game still drives |
| GTR2_SPECIFIC | **Ignore** for GTA |
| AA / depth options | Nice-to-have |

---

## Handshake (our path = OpenVR)

1. Start OpenVR (`VR_Init` / SteamVR)  
2. `GetVRDesc` — Vulkan info for the texture  
3. Begin/lock submit  
4. `IVRCompositor::Submit`  
5. Unlock/end  

**Who decides "left eye"?** Our glue — not IronWolf alone.

---

## L4D2VR split (reference model)

- **DXVK:** drawing + mailbox  
- **L4D2VR code:** camera, eyes, controls  

Same separation for GTA IV.

---

## Application here

```
GTAIV.exe
  → d3d9.dll (IronWolf/sd805-based, x86)
  → GTA glue (submit timing, camera later)
  → OpenVR / SteamVR → headset
```

Steps: fork base → build x86 → flat boot → mono submit → stereo/camera.

See `docs/FAQ.md`, `docs/REFERENCES.md`, `docs/HOME_CURSOR.md`.
