# Build-Notizen

Genaue Meson/VS-Schritte werden nach dem ersten erfolgreichen Submodule-Sync zu Hause vom Agenten ergänzt.

## Vorbild L4D2VR

https://github.com/sd805/l4d2vr

```text
git clone --recurse-submodules https://github.com/sd805/l4d2vr.git
l4d2vr.sln → Plattform x86 → Build
```

Output-Form: `d3d9.dll` Richtung Spielordner.

## Dieses Repo

1. `.\scripts\init-submodules.ps1`  
2. `dxvk\` prüfen (nicht leer; VR-Zweig laut `.gitmodules`)  
3. **x86 / Win32** `d3d9.dll` erzeugen  
4. Nach `GTAIV\` deployen (Backup!)  
5. FusionFix-Vulkan-Toggle **aus**  
6. Flach testen → dann OpenVR-Submit-Glue  

## Deploy-Checkliste

- [ ] DLL ist 32-bit (nicht x64)  
- [ ] Backup der vorherigen `d3d9.dll` / System-DLL-Situation geklärt  
- [ ] SteamVR aus / an je nach Testphase  
- [ ] Log-Datei-Pfad bekannt  

## Konflikte

- FusionFix Vulkan/DXVK vs. unsere DLL  
- Niemals x64-DXVK in GTA IV  
- Antivirus kann neue `d3d9.dll` blockieren  

Siehe auch `config/dxvk.conf.example`.
