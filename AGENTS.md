# Agent contract — gtaiv-dxvk-vr (Phase B)

## Paste when starting a Cursor chat on this project

```text
Lies AGENTS.md und docs/CURRENT-STATE.md.
Phase B: GTA IV CE VR via VR-DXVK d3d9.dll (L4D2VR-shaped), Win32, Reverb G2.
Phase A bleibt ../gtaiv-openxr — hier nur DXVK-Pfad.
Ich bin kein Programmierer — konkrete Schritte.
Als Nächstes: init-submodules.ps1 und Build-Notizen lesen.
```

## Non-negotiable

1. **Win32 / x86 only** for anything loaded into `GTAIV.exe`.
2. Singleplayer / offline while developing.
3. Do **not** vendor Rockstar assets or closed HL2VR / Luke Ross binaries.
4. Prefer **OpenXR + WMR** on Reverb G2; do not assume SteamVR OpenXR works in 32-bit.
5. L4D2VR / sd805/dxvk are **references** — GTA needs its own eye-submit + camera work.
6. Optional async/gplasync stutter fixes are welcome inside the DXVK fork (HL2VR lesson).
7. Update `docs/CURRENT-STATE.md` after headset tests.
8. If the user is still on Phase A, do not delete or “replace” `gtaiv-openxr`.

## Definition of done (first milestone)

x86 `d3d9.dll` builds, loads under FusionFix/CE, OpenXR session starts, **mono** eye texture visible in HMD (stereo later).
