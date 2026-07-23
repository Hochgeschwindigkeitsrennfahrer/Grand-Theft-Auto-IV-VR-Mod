# Übergabe — gtaiv-dxvk-vr (Phase B)

Zweites Projekt neben **gtaiv-openxr** (Phase A).  
Nutzen, wenn der D3D11-Companion-Pfad scheitert oder du umschwenken willst.

---

## 1. Wann hierher wechseln?

Siehe `docs/RELATION_TO_PHASE_A.md`. Kurz: Phase A weiter, bis Present/OpenXR oder Blit klar scheitert — dann diesen Ordner in Cursor öffnen.

## 2. Ordner

```
Documents\gtaiv-openxr      ← Phase A (ASI + D3D11 + OpenXR)
Documents\gtaiv-dxvk-vr     ← Phase B (diese Repo)
```

## 3. Einmalig zu Hause

1. Cursor → Open Folder → `gtaiv-dxvk-vr`
2. VS2022 C++ (x86), Git, CMake/Meson je nach DXVK-Build
3. Submodule holen (Internet):

```powershell
.\scripts\init-submodules.ps1
```

4. Chat-Prompt aus `AGENTS.md` pasten

## 4. Architektur-Ziel (wie L4D2VR)

1. Fork/Submodule: VR-fähiges DXVK → Output **`d3d9.dll` (x86)**
2. Nach `GTAIV\` legen (Konflikt mit FusionFix-Vulkan-Toggle klären — siehe Gillian/FusionFix Docs)
3. Eye RTs in Vulkan erzeugen / abgreifen
4. An **OpenXR** submitten (WMR zuerst)
5. Kamera später (Natives / AOB) — gleiches Problem wie Phase A

## 5. Was schon „mitgebracht“ ist

Alle harten Constraints aus Phase A: Win32, WMR vs SteamVR OpenXR, kein Drop-in fremder `d3d9.dll`, async nur Stutter, CE vs 1.0.8.0 — in `docs/LESSONS_FROM_PHASE_A.md`.

## 6. Sicherheit

Firmen-PC: kein Netzwerk-Scan; Submodule/Push eher zu Hause. Kein Multiplayer während der Entwicklung.
