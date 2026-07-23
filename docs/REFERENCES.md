# Referenzen

Alle wichtigen externen Links für dieses Projekt.

## Vorbilder (Architektur)

| Projekt | Link | Was wir davon nehmen |
|---------|------|----------------------|
| **L4D2VR** | https://github.com/sd805/l4d2vr | Gesamtform: Hooks + DXVK-`d3d9.dll` + **OpenVR Submit** |
| **sd805/dxvk** | https://github.com/sd805/dxvk | L4D2VR-DXVK (IronWolf-basiert + async + L4D2-Hacks) |
| **IronWolf DXVK** | https://github.com/TheIronWolfModding/dxvk | VR-Mailbox (`d3d9_vr.h`); Zweige: siehe `IRONWOLF_DXVK.md` |
| IronWolf `vr-dx9-rel` | https://github.com/TheIronWolfModding/dxvk/tree/vr-dx9-rel | Ältere OpenVR-Mailbox (L4D2VR-Ära) |
| IronWolf `tiw-rel-241-260612` | https://github.com/TheIronWolfModding/dxvk/tree/tiw-rel-241-260612 | Aktuellere VR-Linie (OpenVR + optionale OpenXR-Helfer) |
| HL2VR | (geschlossen) | Lektion: custom DXVK + OpenVR + async — nur Konzept |

## Optional / später

| Projekt | Link | Hinweis |
|---------|------|---------|
| openRBRVR | https://github.com/Detegr/openRBRVR | DX9 + OpenXR, 32-bit — **nicht** v0 |
| dxvk-openRBRVR | https://github.com/Detegr/dxvk-openRBRVR | Falls jemals OpenXR-Pfad |
| doitsujin/dxvk | https://github.com/doitsujin/dxvk | Upstream ohne VR-Mailbox |

## Spiel / CE-Setup

| Ding | Link / Ort |
|------|------------|
| FusionFix (CE) | https://github.com/ThirteenAG/III.VC.SA.FusionFix — CE-Build/Docs beachten |
| GTA IV CE | Steam / Rockstar Launcher |
| SteamVR | Steam-Bibliothek |
| OpenVR | https://github.com/ValveSoftware/openvr |

## Tools

| Tool | Link |
|------|------|
| Cursor | https://cursor.com |
| Git | https://git-scm.com/download/win |
| VS 2022 | https://visualstudio.microsoft.com/ |
| Vulkan SDK | https://vulkan.lunarg.com/ |
| Meson | https://mesonbuild.com/ |

## Verwandtes (optional, nicht Voraussetzung)

Ein älteres Experiment mit ASI + D3D11 + OpenXR kann unter `Documents\gtaiv-openxr` liegen. **Dieses Repo ist eigenständig** und braucht das nicht.
