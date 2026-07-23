# CURRENT-STATE — gtaiv-dxvk-vr

## Version

Scaffold **0.1.2** — IronWolf branches clarified (`master` ≠ VR mailbox; prefer `tiw-rel-241-*`); still no built `d3d9.dll`.

## Goal

L4D2VR-shaped pipeline for GTA IV CE: **VR-patched DXVK (IronWolf / openRBRVR family) → Vulkan eyes → OpenXR** (WMR-first on Reverb G2).

## Done

| Item | Status |
|------|--------|
| Repo + HANDOFF / AGENTS | Done |
| Lessons from Phase A | Done |
| IronWolf fork summary | Done (`docs/IRONWOLF_DXVK.md`) |
| Submodule script | Done (fetch at home) |
| Build / VR hooks / submit | **Not started** |

## Next (home)

1. `.\scripts\init-submodules.ps1` (sd805/dxvk as starting point)
2. Confirm x86 build of baseline `d3d9.dll` loads in GTA IV CE + FusionFix (flat Vulkan OK)
3. Study L4D2VR VR submit points in their DXVK fork + openRBRVR OpenXR path
4. Port/adapt eye Submit to OpenXR; log heavily
5. Only then: camera / stereo

## Failure ledger

| Date | Symptom | Hypothesis | Result | Action |
|------|---------|------------|--------|--------|
| (empty) | | | | |

## Phase A pointer

Sibling: `../gtaiv-openxr` (ASI + D3D11). Do not abandon until pivot criteria in `RELATION_TO_PHASE_A.md`.
