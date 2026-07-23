# Übergabe (kurz)

**Vollständige Anleitung:** [`docs/HOME_CURSOR.md`](docs/HOME_CURSOR.md)  
**Aktueller Stand + Verbote + Stereo-Wissen:** [`docs/CURRENT-STATE.md`](docs/CURRENT-STATE.md)  
**Strategie / Playbook:** [`docs/VR_STRATEGY.md`](docs/VR_STRATEGY.md), [`docs/VR_MOD_PLAYBOOK.md`](docs/VR_MOD_PLAYBOOK.md)

## In einem Satz

GTA IV CE → Stock DXVK 3.0.2 `d3d9.dll` (x86) → `gtaiv_dxvk_vr.asi` → OpenVR/SteamVR → Reverb G2.  
**Jetzt:** stabiler Mono-Submit + FP-Cam (~90 FPS). Stereo-Spike pausiert (Mode 0).

## Sofort

1. Cursor: Open Folder → `Documents\cursor\gtaiv-dxvk-vr`  
2. Status lesen: `docs/CURRENT-STATE.md`  
3. Build ASI: `.\scripts\build-asi.ps1`  
4. Deploy (Spiel zu):  
   `.\scripts\deploy-asi.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"`  
5. `gtaiv_dxvk_vr.stereo` = `0` lassen, bis Dual-Draw steht  

## Sicherheit

Offline/Singleplayer. Freeze → stereo-Datei auf `0`.

## Start beschleunigen

Siehe [`docs/STARTUP_SPEED.md`](docs/STARTUP_SPEED.md) — Launcher warm halten; optional RGLess; kein CE-Downgrade wegen unserer AOBs.
