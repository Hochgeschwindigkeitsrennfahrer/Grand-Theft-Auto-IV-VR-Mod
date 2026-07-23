# FAQ

## Warum nicht einfach IronWolf-`d3d9.dll`?

Auf diesem Setup (RTX 4070 Ti) sind alte VR-Forks (IronWolf ~2.4, FF-DXVK 2.6.2) flach kaputt.  
**Stock-DXVK 3.0.2** flach ok. Modernes DXVK liefert die Vulkan-Handles über **`ID3D9VkInteropDevice`** — das reicht für OpenVR-Submit. Unser Code steckt in einer **ASI**, nicht zwingend in einem eigenen DXVK-Fork. Details: `docs/VR_STRATEGY.md`.

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
