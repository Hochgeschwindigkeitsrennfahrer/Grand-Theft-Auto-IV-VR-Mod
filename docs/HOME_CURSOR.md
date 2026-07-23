# Cursor zu Hause — komplette Anleitung

Dieses Dokument reicht, um **gtaiv-dxvk-vr** allein zu Hause weiterzuführen.  
Kein Programmierer nötig: Cursor schreibt den Code; du installierst Tools, startest Builds und testest mit Headset.

---

## 0. Was du baust

Ein VR-Mod für **GTA IV Complete Edition**:

- Spiel spricht weiter **Direct3D 9**
- Unsere **`d3d9.dll`** (32-bit) übersetzt nach **Vulkan** und liefert Augenbilder
- **SteamVR / OpenVR** zeigt das Bild in der Brille (wie L4D2VR / HL2VR)

Als Nächstes (Reihenfolge fest): Submodule → flache DLL → Mono in der Brille → später Kamera.

Offline schon vorbereitet: Glue-Stubs (`src/gtaiv/`), `deploy.ps1`, `docs/IRONWOLF_API.md`, `docs/L4D2VR_MAP.md`.


---

## 1. Einmalig: Software installieren

| Tool | Wozu | Hinweis |
|------|------|---------|
| [Cursor](https://cursor.com) | KI-Coding | Diesen Ordner öffnen |
| [Git for Windows](https://git-scm.com/download/win) | Submodule, Commits | „Git from command line“ ok |
| [Visual Studio 2022](https://visualstudio.microsoft.com/) | C++ x86 bauen | Workload **Desktopentwicklung mit C++** |
| Vulkan SDK (optional zuerst) | DXVK-Build | Später nachrüsten, wenn Build danach fragt |
| Meson / Ninja / Python | DXVK-Build (je nach Fork) | Agent hilft bei der Installation |
| [Steam](https://store.steampowered.com/) + **SteamVR** | OpenVR-Runtime | Reverb G2 darüber anbinden |
| GTA IV **Complete Edition** | Zielspiel | Singleplayer / offline beim Entwickeln |
| [FusionFix](https://github.com/ThirteenAG/III.VC.SA.FusionFix) (CE-Variante) | ASI-Loader + CE-Fixes | Vulkan-/DXVK-Toggle von FusionFix **aus**, wenn unsere `d3d9.dll` aktiv ist |

**Firmen-PC:** Submodule/GitHub eher zu Hause (Netzwerk). Lokale Builds/Tests ok, wenn Tools schon da sind.

---

## 2. Projekt in Cursor öffnen

1. Cursor starten  
2. **File → Open Folder…**  
3. Ordner wählen:

```text
C:\Users\amien.krause\Documents\gtaiv-dxvk-vr
```

4. Warten, bis der Explorer links diesen Projektbaum zeigt (nicht `gtaiv-openxr`, nicht Home).

---

## 3. Neuen Chat mit dem richtigen Prompt starten

Neuen Agent-Chat öffnen und **genau das** pasten (steht auch in `AGENTS.md`):

```text
Lies AGENTS.md und docs/CURRENT-STATE.md und docs/HOME_CURSOR.md.
Projekt: gtaiv-dxvk-vr — GTA IV CE VR via VR-DXVK d3d9.dll + OpenVR (SteamVR), Win32, Reverb G2.
Wie L4D2VR/HL2VR. Kein OpenXR für den ersten Meilenstein.
Ich bin kein Programmierer — konkrete Klick-/Befehlsschritte.
Als Nächstes: Submodule initialisieren und x86-Build-Plan.
```

Der Agent soll danach **einen klaren nächsten Schritt** nennen (nicht zehn parallele Baustellen).

---

## 4. Submodule holen (Internet nötig)

In Cursor: Terminal öffnen (`` Ctrl+` ``) und:

```powershell
cd C:\Users\amien.krause\Documents\gtaiv-dxvk-vr
.\scripts\init-submodules.ps1
```

Erfolg: Ordner `dxvk\` enthält Quellcode (nicht leer).  
Bei Fehlern: komplette Terminal-Ausgabe an den Agenten pasten.

---

## 5. Ersten Build (noch ohne VR-Logik)

Ziel: eine **32-bit** `d3d9.dll`, die GTA IV **flach** (Monitor) unter FusionFix startet.

1. Agent: „Bauplan für x86 d3d9.dll aus dem Submodule schreiben und Schritt für Schritt anleiten.“  
2. Du folgst den Befehlen / VS-Schritten.  
3. DLL nach `GTAIV\` kopieren (Backup der alten Datei behalten).  
4. FusionFix-Vulkan-Toggle **deaktivieren**, falls vorhanden.  
5. Spiel starten → wenn Monitor-Bild ok: in `docs/CURRENT-STATE.md` vermerken.

Details: `docs/BUILD.md`.

---

## 6. Headset-Test-Schleife (ab Mono-Submit)

Immer dieselbe Reihenfolge:

1. **SteamVR** starten, Brille erkannt  
2. Eine Verhaltensänderung vom Agenten (nur **eine** pro Build)  
3. Neu bauen → DLL deployen  
4. GTA starten (offline)  
5. Log-Datei neben `GTAIV.exe` öffnen (Name legt der Agent fest, z. B. `gtaiv_dxvk_vr.log`)  
6. In den Chat pasten: Log-Ausschnitt + „Brille: ja/nein, was zu sehen war“

Regel: **eine Änderung pro Test** — sonst weißt du nicht, was kaputt ging.

---

## 7. Was du dem Agenten nie vergessen solltest

- Spiel ist **32-bit** → alles **x86 / Win32**  
- Compositor = **OpenVR / SteamVR** (nicht OpenXR)  
- Fremde `d3d9.dll` (L4D2VR-Release) **nicht** einfach reinwerfen  
- Kein Multiplayer während der Entwicklung  
- Nach Tests: `docs/CURRENT-STATE.md` aktualisieren lassen  

---

## 8. Checkliste „Zuhause Tag 1“

- [ ] Cursor zeigt Ordner `gtaiv-dxvk-vr`  
- [ ] VS2022 C++ + Git installiert  
- [ ] SteamVR + Reverb G2 ok  
- [ ] FusionFix in GTA IV CE  
- [ ] Chat-Prompt aus Abschnitt 3 benutzt  
- [ ] `.\scripts\init-submodules.ps1` erfolgreich  
- [ ] Nächster Agent-Schritt: x86-Build der flachen `d3d9.dll`  

---

## 9. Wenn etwas hakt

| Problem | Aktion |
|---------|--------|
| Falscher Ordner in Cursor | Open Folder → `Documents\gtaiv-dxvk-vr` |
| Submodule leer | Script erneut; Fehlermeldung pasten |
| Spiel startet nicht nach DLL | Backup zurück; FusionFix-Vulkan prüfen; Log pasten |
| SteamVR sieht Brille nicht | Zuerst SteamVR allein fixen, dann GTA |
| Agent redet von OpenXR | Auf `AGENTS.md` / dieses Doc verweisen: **OpenVR only** für v0 |

Mehr Kontext: `docs/FAQ.md`, `docs/CONSTRAINTS.md`, `docs/REFERENCES.md`.
