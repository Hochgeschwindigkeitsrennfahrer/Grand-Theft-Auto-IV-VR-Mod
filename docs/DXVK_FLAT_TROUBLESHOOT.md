# Flach-DXVK unter FusionFix (Troubleshooting)

Ziel: Irgendeine x86-DXVK-`d3d9`/`vulkan.dll` zeigt GTA IV CE **flach** korrekt.  
Ohne das kein OpenVR-Submit-Meilenstein.

## Schnellschalter

Datei `GTAIV\d3d9.cfg`:

```ini
[MAIN]
API = 0   ; DirectX 9
API = 1   ; Vulkan (lädt vulkan.dll = DXVK)
```

Nach jeder Änderung: Spiel **neu starten**.

## Bekannte Fehlversuche (dieses Setup)

| Test | Ergebnis |
|------|----------|
| IronWolf tiw-rel als `d3d9.dll` | Texturen weg, Vertex-Explosion |
| FF Vulkan 2.6.2 | schwarz, Crash |
| ohne `dxvk.conf` | gleich |
| Admin | gleich |
| OneDrive-Rockstar | nicht vorhanden |
| GPL=False / hideNvidia=False | gleich |
| neuer NVIDIA-Treiber | gleich |

## Bekannt gut (dieses Setup)

| Datei | Version / Hinweis |
|-------|-------------------|
| `GTAIV\d3d9.dll` | Stock [DXVK 3.0.2](https://github.com/doitsujin/dxvk/releases/tag/v3.0.2) → Ordner **`x32\d3d9.dll`** (direkt so benannt) |
| `GTAIV\vulkan.dll` | **unverändert** gelassen (FF-Original) |
| FusionFix | installiert |

Wichtig: Erfolg war **`d3d9.dll` tauschen**, nicht `vulkan.dll` umbenennen/löschen.

FF-mitgeliefertes älteres DXVK und unser IronWolf **tiw-rel (~2.4.1)** → auf RTX 4070 Ti hier flach **nicht** nutzbar.

## d3d9.dll tauschen (Rezept, wie es hier funktionierte)

1. Backup: vorhandene `d3d9.dll` → z. B. `d3d9.dll.bak`  
2. [DXVK Releases](https://github.com/doitsujin/dxvk/releases) → Archiv → **`x32\d3d9.dll`**  
3. Nach `GTAIV\` als **`d3d9.dll`** kopieren  
4. `vulkan.dll` nicht anfassen  
5. Spiel starten, flach prüfen  

Alternativ (nicht der erfolgreiche Weg hier): FF-Doku nennt oft Umbenennen zu `vulkan.dll` + `API = 1` — bei Bedarf separat testen.

## Folge für VR

Stock 3.0.2 hat **keine** IronWolf-Mailbox (`GetVRDesc` / `IDirect3DVR9`).  
Nächster Schritt: VR-Patches auf eine **neue** DXVK-Basis (Richtung 3.0.x), Deliverable weiter als x86-`d3d9.dll`.
