# gtaiv-dxvk-vr

Eigenständiges Projekt: **GTA IV Complete Edition in VR** über eine VR-fähige DXVK-`d3d9.dll` und **OpenVR (SteamVR)** — nach dem Muster von [L4D2VR](https://github.com/sd805/l4d2vr) / HL2VR.

```
GTAIV.exe (32-bit, D3D9)
  → unsere d3d9.dll  (VR-DXVK-Fork + GTA-Glue)
       → Vulkan-Augenbilder
  → OpenVR Submit (SteamVR)
  → Headset (z. B. HP Reverb G2)
```

**Status:** Scaffold **0.3.1** — Doku, Glue-Stubs, `deploy.ps1`. Noch keine fertige VR-`d3d9.dll`.

---

## Für dich (kein Programmierer)

1. **Zuhause komplett starten:** → [`docs/HOME_CURSOR.md`](docs/HOME_CURSOR.md)  
2. **Kurzfragen:** → [`docs/FAQ.md`](docs/FAQ.md)  
3. **Cursor-Chat:** Prompt aus [`AGENTS.md`](AGENTS.md)  
4. **Fortschritt:** → [`docs/CURRENT-STATE.md`](docs/CURRENT-STATE.md)

---

## Docs

| Datei | Inhalt |
|-------|--------|
| [docs/HOME_CURSOR.md](docs/HOME_CURSOR.md) | **Anleitung Cursor zu Hause** |
| [AGENTS.md](AGENTS.md) | Agent-Vertrag |
| [docs/CURRENT-STATE.md](docs/CURRENT-STATE.md) | Status |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Technik |
| [docs/CONSTRAINTS.md](docs/CONSTRAINTS.md) | Harte Regeln |
| [docs/REFERENCES.md](docs/REFERENCES.md) | Links |
| [docs/IRONWOLF_DXVK.md](docs/IRONWOLF_DXVK.md) | IronWolf-Fork erklärt |
| [docs/IRONWOLF_API.md](docs/IRONWOLF_API.md) | `IDirect3DVR9` Submit-API |
| [docs/L4D2VR_MAP.md](docs/L4D2VR_MAP.md) | L4D2VR-Dateien zum Abgucken |
| [docs/FAQ.md](docs/FAQ.md) | FAQ |
| [docs/BUILD.md](docs/BUILD.md) | Build |
| [HANDOFF.md](HANDOFF.md) | Kurze Übergabe |

---

## Scripts

| Script | Zweck |
|--------|--------|
| `scripts/init-submodules.ps1` | DXVK-Submodule holen |
| `scripts/deploy.ps1` | `d3d9.dll` nach GTAIV kopieren (+ Backup) |

---

## Lizenz

MIT für *unsere* Scaffolding-Dateien. DXVK / L4D2VR / OpenVR behalten ihre Lizenzen (Submodule).
