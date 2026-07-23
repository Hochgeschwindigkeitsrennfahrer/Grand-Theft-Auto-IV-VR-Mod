# VR-Strategie (ab 0.6)

## Entscheidung

**Nicht** IronWolf `tiw-rel` (~DXVK 2.4) weiter erzwingen — flach auf diesem PC kaputt.  
**Stattdessen:** Stock-DXVK **3.0.2** (flach bewiesen) + offizielle **`ID3D9VkInterop*`**-API für OpenVR-Submit.

IronWolf `IDirect3DVR9` war der Vorläufer; in modernem DXVK heißt das Pendant:

| IronWolf (alt) | Stock DXVK 3.0.2 |
|----------------|------------------|
| `Direct3DCreateVRImpl` | `QueryInterface(ID3D9VkInteropDevice)` am Device |
| `GetVRDesc` | `ID3D9VkInteropTexture::GetVulkanImageInfo` + Device/Queue-Handles |
| `BeginVRSubmit` / Lock | `FlushRenderingCommands` + `LockSubmissionQueue` / `LockDevice` |
| Multi-View / OpenXR-Helfer | **ignorieren** (v0) |

Referenz-Header (Upstream): `src/d3d9/d3d9_interfaces.h` @ [v3.0.2](https://github.com/doitsujin/dxvk/tree/v3.0.2).  
Lokale GUID-Kopie: `thirdparty/dxvk/d3d9_vk_interop.h`.

## Architektur v0

```
GTAIV.exe
  → d3d9.dll          = Stock DXVK 3.0.2 (flach ok)
  → FusionFix (ASI-Loader, CE-Fixes)
  → gtaiv_dxvk_vr.asi = unser Glue (OpenVR Init + Mono-Submit)
       → QI ID3D9VkInteropDevice / Texture
       → IVRCompositor::Submit (TextureType_Vulkan)
  → SteamVR → Reverb G2
```

Kein OpenXR in v0. Eine Verhaltensänderung pro Test-Build.

## Meilensteine

1. **Interop-Probe** — ASI lädt, loggt erfolgreichen QI + VkImage/Device (noch kein Headset-Bild).  
2. **Mono-Submit** — ein Augenbild in der Brille.  
3. **Stereo / Kamera** — siehe `docs/VR_MOD_PLAYBOOK.md` (Leitbild BotW BetterVR + L4D2VR).

## Submodule

| Pfad | Rolle |
|------|--------|
| `dxvk/` (bisher IronWolf) | Referenz / optional; **nicht** mehr Deliverable-Basis |
| Stock 3.0.2 Binary | Deliverable-`d3d9.dll` (wie jetzt im Spiel) |
| Optional später: `doitsujin/dxvk` @ `v3.0.2` | nur wenn wir DXVK selbst patchen müssen |

## Was du im Spielordner lässt

- `d3d9.dll` = DXVK **3.0.2** x32 (bewährt)  
- `vulkan.dll` = FF-Original, unangetastet  
- FusionFix installiert  
