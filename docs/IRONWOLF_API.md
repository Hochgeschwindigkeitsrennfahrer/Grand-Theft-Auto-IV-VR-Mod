# IronWolf `IDirect3DVR9` — reference for our glue

Source: [tiw-rel-241-260612 `d3d9_vr.h`](https://github.com/TheIronWolfModding/dxvk/blob/tiw-rel-241-260612/src/d3d9/d3d9_vr.h)  
This project uses **OpenVR only** (v0). Ignore OpenXR methods.

After `init-submodules.ps1`, the real header lives at `dxvk/src/d3d9/d3d9_vr.h`.  
The copy under `thirdparty/ironwolf/` is a **snapshot reference** for planning without submodules.

---

## Entry point

```text
Direct3DCreateVRImpl(IDirect3DDevice9*, IDirect3DVR9**)
```

Then typical mono submit (like L4D2VR family):

1. `BeginVRSubmit()` / `LockDevice()`  
2. Choose backbuffer/eye surface  
3. `GetVRDesc(surface, &desc)` → Vulkan `Image` + device/queue/format/size  
4. `TransferSurfaceForVR(surface)` if needed  
5. OpenVR: `IVRCompositor::Submit(Eye, &Texture_t{ handle, TextureType_Vulkan, ColorSpace })`  
6. `EndVRSubmit()` / `UnlockDevice()`  

---

## Methods relevant for us

| Method | Use in v0 |
|---------|-----------|
| `GetVRDesc` | Vulkan info for submit |
| `TransferSurfaceForVR` | Prepare surface for VR |
| `BeginVRSubmit` / `EndVRSubmit` | Sync around compositor |
| `LockDevice` / `UnlockDevice` | Device lock |
| `WaitDeviceIdle` / `WaitGraphicsQueueIdle` | experimental, only for sync issues |

## Later / not v0

| Method | Note |
|---------|---------|
| `GetOXRVkDeviceDesc` | OpenXR — Phase B v0 **no** |
| Multi-View / `CopySurfaceLayers` / … | Stereo SPS (detegr) — after mono |

---

## `D3D9_TEXTURE_VR_DESC` (fields)

- `Image`, `Device`, `PhysicalDevice`, `Instance`, `Queue`, `QueueFamilyIndex`  
- `Width`, `Height`, `Format`, `SampleCount`  

OpenVR typically expects `VRVulkanTextureData_t` from these values (see OpenVR headers / L4D2VR).
