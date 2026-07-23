# CURRENT-STATE — gtaiv-dxvk-vr

## Version

Scaffold **0.2.0** — **OpenVR-first** (L4D2VR/HL2VR-shaped); still no built `d3d9.dll`.

## Goal

L4D2VR-shaped pipeline for GTA IV CE: **VR-patched DXVK → Vulkan eyes → OpenVR Submit (SteamVR)** on Reverb G2.

## Done

| Item | Status |
|------|--------|
| Repo + HANDOFF / AGENTS | Done |
| Lessons from Phase A | Done |
| IronWolf fork summary | Done (`docs/IRONWOLF_DXVK.md`) |
| Strategy: OpenVR primary | Done (0.2.0) |
| Submodule script | Done (fetch at home) |
| Build / VR hooks / Submit | **Not started** |

## Next (home)

1. `.\scripts\init-submodules.ps1`
2. Confirm x86 build of baseline `d3d9.dll` loads in GTA IV CE + FusionFix (flat Vulkan OK)
3. Study L4D2VR OpenVR submit points (`GetVRDesc` → `Submit`)
4. Minimal GTA glue: mono backbuffer → eye → OpenVR `Submit`; log heavily
5. Only then: camera / stereo
6. OpenXR: defer (Phase A or later Phase B)

## Failure ledger

| Date | Symptom | Hypothesis | Result | Action |
|------|---------|------------|--------|--------|
| (empty) | | | | |

## Phase A pointer

Sibling: `../gtaiv-openxr` (ASI + D3D11 + OpenXR). Do not abandon until pivot criteria in `RELATION_TO_PHASE_A.md`.
