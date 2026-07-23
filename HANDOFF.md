# Handoff (short)

**Full guide:** [`docs/HOME_CURSOR.md`](docs/HOME_CURSOR.md)  
**Current state + prohibitions + stereo knowledge:** [`docs/CURRENT-STATE.md`](docs/CURRENT-STATE.md)  
**Strategy / playbook:** [`docs/VR_STRATEGY.md`](docs/VR_STRATEGY.md), [`docs/VR_MOD_PLAYBOOK.md`](docs/VR_MOD_PLAYBOOK.md)

## In one sentence

GTA IV CE → Stock DXVK 3.0.2 `d3d9.dll` (x86) → `gtaiv_dxvk_vr.asi` → OpenVR/SteamVR → Reverb G2.  
**Now:** stable Mono-Submit + FP cam (~90 FPS). Stereo spike paused (Mode 0).

## Immediate steps

1. Cursor: Open Folder → `Documents\cursor\gtaiv-dxvk-vr`  
2. Read status: `docs/CURRENT-STATE.md`  
3. Build ASI: `.\scripts\build-asi.ps1`  
4. Deploy (to game folder):  
   `.\scripts\deploy-asi.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"`  
5. Leave `gtaiv_dxvk_vr.stereo` = `0` until dual-draw works  

## Safety

Offline/Singleplayer. Freeze → set stereo file to `0`.

## Faster startup

See [`docs/STARTUP_SPEED.md`](docs/STARTUP_SPEED.md) — keep launcher warm; optional RGLess; no CE downgrade because of our AOBs.
