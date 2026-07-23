# Übergabe — gtaiv-dxvk-vr (Phase B)

Zweites Projekt neben **gtaiv-openxr** (Phase A).  
Hier: **DXVK + OpenVR**, wie L4D2VR / HL2VR.

---

## 1. Wann hierher wechseln?

Siehe `docs/RELATION_TO_PHASE_A.md`. Kurz: Phase A weiter, bis Present/Blit klar scheitert — oder du bewusst den L4D2VR-Weg willst — dann diesen Ordner in Cursor öffnen.

## 2. Ordner

```
Documents\gtaiv-openxr      ← Phase A (ASI + D3D11 + OpenXR)
Documents\gtaiv-dxvk-vr     ← Phase B (diese Repo, OpenVR)
```

## 3. Einmalig zu Hause

1. Cursor → Open Folder → `gtaiv-dxvk-vr`
2. VS2022 C++ (x86), Git, CMake/Meson je nach DXVK-Build
3. **SteamVR** installiert (Reverb G2 darüber anbinden)
4. Submodule holen (Internet):

```powershell
.\scripts\init-submodules.ps1
```

5. Chat-Prompt aus `AGENTS.md` pasten

## 4. Architektur-Ziel (wie L4D2VR / HL2VR)

1. Fork/Submodule: VR-fähiges DXVK → Output **`d3d9.dll` (x86)**
2. Nach `GTAIV\` legen (Konflikt mit FusionFix-Vulkan-Toggle klären)
3. Eye RTs / Backbuffer → Vulkan-Info via IronWolf mailbox (`GetVRDesc` …)
4. An **OpenVR** submitten (`IVRCompositor::Submit`) — **nicht** OpenXR für v0
5. Kamera später (Natives / AOB) — gleiches Problem wie Phase A

## 5. Was schon „mitgebracht“ ist

Constraints aus Phase A (Win32, kein Drop-in fremder `d3d9.dll`, async nur Stutter, CE vs 1.0.8.0) — in `docs/LESSONS_FROM_PHASE_A.md`.  
**Neu für Phase B:** OpenVR-first; SteamVR OpenXR-Probleme aus Phase A gelten hier nicht als Blocker.

## 6. Sicherheit

Firmen-PC: kein Netzwerk-Scan; Submodule/Push eher zu Hause. Kein Multiplayer während der Entwicklung.
