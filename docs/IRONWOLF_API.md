# IronWolf `IDirect3DVR9` — Referenz für unser Glue

Quelle: [tiw-rel-241-260612 `d3d9_vr.h`](https://github.com/TheIronWolfModding/dxvk/blob/tiw-rel-241-260612/src/d3d9/d3d9_vr.h)  
Dieses Projekt nutzt **nur OpenVR** (v0). OpenXR-Methoden ignorieren.

Nach `init-submodules.ps1` liegt die echte Header-Datei unter `dxvk/src/d3d9/d3d9_vr.h`.  
Die Kopie unter `thirdparty/ironwolf/` ist eine **Snapshot-Referenz** für Planung ohne Submodule.

---

## Einstieg

```text
Direct3DCreateVRImpl(IDirect3DDevice9*, IDirect3DVR9**)
```

Danach typischer Mono-Submit (wie L4D2VR-Familie):

1. `BeginVRSubmit()` / `LockDevice()`  
2. Backbuffer-/Eye-Surface wählen  
3. `GetVRDesc(surface, &desc)` → Vulkan-`Image` + Device/Queue/Format/Größe  
4. `TransferSurfaceForVR(surface)` falls nötig  
5. OpenVR: `IVRCompositor::Submit(Eye, &Texture_t{ handle, TextureType_Vulkan, ColorSpace })`  
6. `EndVRSubmit()` / `UnlockDevice()`  

---

## Für uns relevante Methoden

| Methode | Nutzen v0 |
|---------|-----------|
| `GetVRDesc` | Vulkan-Infos für Submit |
| `TransferSurfaceForVR` | Surface für VR bereit |
| `BeginVRSubmit` / `EndVRSubmit` | Sync um Compositor |
| `LockDevice` / `UnlockDevice` | Device-Lock |
| `WaitDeviceIdle` / `WaitGraphicsQueueIdle` | experimentell, nur bei Sync-Problemen |

## Später / nicht v0

| Methode | Hinweis |
|---------|---------|
| `GetOXRVkDeviceDesc` | OpenXR — Phase B v0 **nein** |
| Multi-View / `CopySurfaceLayers` / … | Stereo-SPS (detegr) — nach Mono |

---

## `D3D9_TEXTURE_VR_DESC` (Felder)

- `Image`, `Device`, `PhysicalDevice`, `Instance`, `Queue`, `QueueFamilyIndex`  
- `Width`, `Height`, `Format`, `SampleCount`  

OpenVR erwartet typischerweise `VRVulkanTextureData_t` aus diesen Werten (siehe OpenVR-Headers / L4D2VR).
