# CURRENT-STATE — gtaiv-dxvk-vr

## Version

**0.3.0** — eigenständiges Projektpaket (HOME_CURSOR, FAQ, REFERENCES). Noch keine gebaute VR-`d3d9.dll`.

## Ziel

GTA IV Complete Edition: **VR-DXVK → Vulkan-Augen → OpenVR Submit (SteamVR)** auf Reverb G2 (L4D2VR/HL2VR-Muster).

## Erledigt

| Punkt | Status |
|-------|--------|
| Repo-Scaffold, Lizenz, .gitignore | Done |
| Eigenständige Doku (HOME_CURSOR, FAQ, …) | Done |
| IronWolf / Branch-Erklärung | Done |
| OpenVR-first Strategie | Done |
| Submodule-Script | Done (ausführen zu Hause) |
| x86-Build / Submit-Glue | **Offen** |

## Als Nächstes (Zuhause)

1. Cursor: Ordner `gtaiv-dxvk-vr` öffnen, Prompt aus `AGENTS.md`  
2. `.\scripts\init-submodules.ps1`  
3. x86-`d3d9.dll` flach unter FusionFix booten  
4. Mono OpenVR `Submit`  
5. Stereo + Kamera später  

## Failure-Ledger

| Datum | Symptom | Hypothese | Ergebnis | Aktion |
|-------|---------|-----------|----------|--------|
| (leer) | | | | |

## Deploy-Hinweis

Bei aktiver eigener `d3d9.dll`: FusionFix-**Vulkan/DXVK-Toggle aus**. Backup der Original-DLL behalten.
