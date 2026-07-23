# CURRENT-STATE — gtaiv-dxvk-vr

## Version

**0.3.1** — Offline-Vorarbeit: Glue-Stubs, IronWolf-API-Doku, L4D2VR-Map, `deploy.ps1`.  
Noch keine gebaute VR-`d3d9.dll`, Submodule noch nicht geholt.

## Ziel

GTA IV CE: **VR-DXVK → Vulkan → OpenVR Submit (SteamVR)** (L4D2VR/HL2VR-Muster).

## Erledigt

| Punkt | Status |
|-------|--------|
| Repo + Home-Doku-Paket | Done |
| OpenVR-first Strategie | Done |
| IronWolf Branch-/API-Doku | Done (`IRONWOLF_DXVK.md`, `IRONWOLF_API.md`) |
| L4D2VR Abguck-Map | Done (`L4D2VR_MAP.md` → `vr.cpp`) |
| Glue-Stubs `src/gtaiv/` | Done (noch nicht gelinkt) |
| `deploy.ps1` | Done |
| Submodule / x86-Build / Submit | **Offen (Zuhause)** |

## Als Nächstes (Zuhause)

1. `.\scripts\init-submodules.ps1`  
2. x86-`d3d9.dll` flach unter FusionFix  
3. L4D2VR `vr.cpp` lesen (Submit)  
4. Glue an `GetVRDesc` + `Submit` anbinden  
5. `deploy.ps1` + Mono in der Brille  

## Failure-Ledger

| Datum | Symptom | Hypothese | Ergebnis | Aktion |
|-------|---------|-----------|----------|--------|
| (leer) | | | | |
