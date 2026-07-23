# FAQ

## Warum müssen wir „einen Fork bauen“?

Weil **normale** DXVK-`d3d9.dll` nur Vulkan für den **Monitor** macht.

Für VR brauchen wir:

1. eine **VR-Mailbox** (IronWolf/sd805: `GetVRDesc`, Submit-Sync, …)  
2. **unseren** Code: wann welches Bild, später Kamera  

„Fork bauen“ heißt: diesen VR-fähigen DXVK-Stand **selbst als x86-DLL kompilieren** und für GTA erweitern — nicht DXVK von Null schreiben und nicht einfach L4D2VRs fertige DLL kopieren.

## Wozu FusionFix?

FusionFix ist die übliche **CE-Basis**: Fixes + **ASI-Loader**.

- Es macht **kein** VR.  
- Es hilft, Mods zu laden und CE stabiler zu fahren.  
- Wenn **unsere** `d3d9.dll` aktiv ist: FusionFix-**Vulkan/DXVK-Schalter aus**, sonst kämpfen zwei Vulkan-Pfade.

Theoretisch ginge nur DLL neben der EXE — praktisch ist CE + FusionFix der Standard-Setup.

## Warum OpenVR statt OpenXR?

Weil L4D2VR und HL2VR so arbeiten und IronWolfs Mailbox dafür gemacht ist. Weniger Eigenbau für den ersten Brillen-Frame. SteamVR muss laufen (Reverb G2 darüber).

OpenXR kann später kommen; für v0 nicht nötig.

## Reicht es, IronWolf/sd805-DLL in GTA zu legen?

**Nein.** Autor und L4D2VR: ohne **spielseitiges** Glue kein magisches VR. Außerdem sind L4D2-Hacks (Viewport, HUD, …) nicht für GTA gedacht.

## Welchen IronWolf-Branch?

| Branch | Nutzen |
|--------|--------|
| `master` | **Nicht** für VR-Mailbox (`d3d9_vr.h` fehlt) |
| `vr-dx9-rel` | Klassische OpenVR-Mailbox (gut zum Lernen/Abgucken) |
| `tiw-rel-241-*` | Aktuellere Linie; OpenVR nutzen, OpenXR-Helfer ignorieren |

Details: `docs/IRONWOLF_DXVK.md`.

## Was ist der erste Erfolg?

Spiel startet mit unserer x86-`d3d9.dll` **flach** (Monitor).  
Danach: **ein** Bild in der Brille (Mono-Submit). Erst dann Stereo/Kamera.

## Wo steht die Cursor-Anleitung zu Hause?

→ [`docs/HOME_CURSOR.md`](HOME_CURSOR.md)
