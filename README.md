# gtaiv-dxvk-vr

Eigenständiges Projekt: **GTA IV Complete Edition in VR** über eine VR-fähige DXVK-`d3d9.dll` und **OpenVR (SteamVR)** — nach dem Muster von [L4D2VR](https://github.com/sd805/l4d2vr) / HL2VR.

```
GTAIV.exe (32-bit, D3D9)
  → unsere d3d9.dll  (VR-DXVK-Fork + GTA-Glue)
       → Vulkan-Augenbilder
  → OpenVR Submit (SteamVR)
  → Headset (z. B. HP Reverb G2)
```

**Status:** Scaffold **0.3.0** — Doku & Build-Hinweise. Noch keine fertige VR-`d3d9.dll`.

---

## Für dich (kein Programmierer)

1. **Zuhause komplett starten:** → [`docs/HOME_CURSOR.md`](docs/HOME_CURSOR.md)  
2. **Kurzfragen (Fork, FusionFix, …):** → [`docs/FAQ.md`](docs/FAQ.md)  
3. **Cursor-Chat starten:** Prompt aus [`AGENTS.md`](AGENTS.md) pasten  
4. **Fortschritt:** → [`docs/CURRENT-STATE.md`](docs/CURRENT-STATE.md)

---

## Ziel-Meilensteine

| # | Ziel |
|---|------|
| 1 | Submodule holen, **x86**-`d3d9.dll` bauen |
| 2 | Flach unter FusionFix starten (Monitor, noch kein VR) |
| 3 | Mono-Bild → OpenVR `Submit` → sichtbar in der Brille |
| 4 | Stereo + First-Person-Kamera |
| 5 | Später: Motion Controls / Komfort |

---

## Docs

| Datei | Inhalt |
|-------|--------|
| [docs/HOME_CURSOR.md](docs/HOME_CURSOR.md) | **Komplette Anleitung Cursor zu Hause** |
| [AGENTS.md](AGENTS.md) | Vertrag für den Cursor-Agenten |
| [docs/CURRENT-STATE.md](docs/CURRENT-STATE.md) | Status / nächste Schritte |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Technik-Überblick |
| [docs/CONSTRAINTS.md](docs/CONSTRAINTS.md) | Harte Regeln (Win32, OpenVR, …) |
| [docs/REFERENCES.md](docs/REFERENCES.md) | Alle externen Links |
| [docs/IRONWOLF_DXVK.md](docs/IRONWOLF_DXVK.md) | Was der IronWolf-DXVK-Fork macht |
| [docs/FAQ.md](docs/FAQ.md) | Häufige Fragen |
| [docs/BUILD.md](docs/BUILD.md) | Build-Notizen |
| [HANDOFF.md](HANDOFF.md) | Kurze Übergabe (zeigt auf HOME_CURSOR) |

---

## Lizenz

MIT für *unsere* Scaffolding-Dateien. DXVK / L4D2VR / OpenVR behalten ihre Lizenzen (Submodule).
