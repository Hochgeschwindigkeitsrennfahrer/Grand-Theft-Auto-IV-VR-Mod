# IronWolf DXVK — verständlich

Upstream: [TheIronWolfModding/dxvk](https://github.com/TheIronWolfModding/dxvk)

---

## Zweige (wichtig: kein Branch namens `openxr`)

| Zweig | Rolle |
|-------|--------|
| [`master`](https://github.com/TheIronWolfModding/dxvk/tree/master) | GitHub-Default. **Keine** Spiel-VR-Mailbox (`d3d9_vr.h` fehlt). |
| [`vr-dx9-rel`](https://github.com/TheIronWolfModding/dxvk/tree/vr-dx9-rel) | Ältere **OpenVR**-Mailbox. Basis für L4D2VR/sd805. |
| [`tiw-rel-241-*`](https://github.com/TheIronWolfModding/dxvk/tree/tiw-rel-241-260612) | Aktuellere VR-Linie. OpenVR + optionale OpenXR-Helfer. Für uns: **nur OpenVR** nutzen. |

---

## In Alltagssprache

Normales DXVK: Spiel denkt D3D9 → GPU bekommt Vulkan → **Monitor**.

IronWolf VR-Linien: zusätzlich ein **Briefkasten**, damit ein Mod Vulkan-Augenbilder an **OpenVR** (SteamVR) geben kann.

Die DLL allein ist **kein** fertiger VR-Mod. Autor:

> Game-side changes needed… dropping this `.dll` into a random game **won't magically** add Vulkan VR.

---

## Nützliche Änderungen

| Änderung | Bedeutung |
|----------|-----------|
| DX9→Vulkan OpenVR-Mailbox | `IDirect3DVR9` / `GetVRDesc` … |
| OpenXR-Helfer (tiw-rel) | Optional; **dieses Projekt v0 nutzt sie nicht** |
| Multi-View (detegr) | Stereo-Effizienz; Spiel steuert weiter |
| GTR2_SPECIFIC | Für GTA **ignorieren** |
| AA / Depth-Optionen | Nice-to-have |

---

## Handshake (unser Weg = OpenVR)

1. OpenVR starten (`VR_Init` / SteamVR)  
2. `GetVRDesc` — Vulkan-Info zur Textur  
3. Begin/Lock Submit  
4. `IVRCompositor::Submit`  
5. Unlock/End  

**Wer entscheidet „linkes Auge“?** Unser Glue — nicht IronWolf allein.

---

## L4D2VR-Aufteilung (Vorbild)

- **DXVK:** Zeichnen + Mailbox  
- **L4D2VR-Code:** Kamera, Augen, Controls  

Gleiche Trennung für GTA IV.

---

## Anwendung hier

```
GTAIV.exe
  → d3d9.dll (IronWolf/sd805-basiert, x86)
  → GTA-Glue (Submit-Timing, später Kamera)
  → OpenVR / SteamVR → Brille
```

Schritte: Fork-Basis → x86 bauen → flach booten → Mono-Submit → Stereo/Kamera.

Siehe `docs/FAQ.md`, `docs/REFERENCES.md`, `docs/HOME_CURSOR.md`.
