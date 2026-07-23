# Agent contract — gtaiv-dxvk-vr (Phase B)

## Paste when starting a Cursor chat on this project

```text
Lies AGENTS.md und docs/CURRENT-STATE.md.
Phase B: GTA IV CE VR via VR-DXVK d3d9.dll (L4D2VR-shaped), Win32, Reverb G2.
Primary compositor: OpenVR (SteamVR) — like L4D2VR/HL2VR, not OpenXR.
Phase A bleibt ../gtaiv-openxr — hier nur DXVK + OpenVR.
Ich bin kein Programmierer — konkrete Schritte.
Als Nächstes: init-submodules.ps1 und Build-Notizen lesen.
```

## Non-negotiable

1. **Win32 / x86 only** for anything loaded into `GTAIV.exe`.
2. Singleplayer / offline while developing.
3. Do **not** vendor Rockstar assets or closed HL2VR / Luke Ross binaries.
4. **OpenVR (SteamVR) is primary** for Phase B. Match L4D2VR/HL2VR: `GetVRDesc` → `IVRCompositor::Submit`. Do not require OpenXR for the first mono HMD frame.
5. OpenXR is **optional later** (or stays in Phase A). Phase A’s “SteamVR OpenXR broken on 32-bit” lesson does **not** block OpenVR.
6. L4D2VR / sd805/dxvk / IronWolf are **references** — GTA still needs its own eye-submit timing + camera work.
7. Optional async/gplasync stutter fixes are welcome inside the DXVK fork (HL2VR lesson).
8. Update `docs/CURRENT-STATE.md` after headset tests.
9. If the user is still on Phase A, do not delete or “replace” `gtaiv-openxr`.

## Definition of done (first milestone)

x86 `d3d9.dll` builds, loads under FusionFix/CE, **SteamVR + OpenVR** init, **mono** eye texture visible in HMD via `Submit` (stereo later).
