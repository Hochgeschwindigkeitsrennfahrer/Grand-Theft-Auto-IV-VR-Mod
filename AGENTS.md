# Agent Contract — gtaiv-dxvk-vr

Standalone project. Do not assume any other repo.

## Prompt to start a chat

```text
Read AGENTS.md and docs/CURRENT-STATE.md and docs/HOME_CURSOR.md and docs/VR_STRATEGY.md.
Project: gtaiv-dxvk-vr — GTA IV CE VR: Stock-DXVK 3.0.2 d3d9.dll + ASI Glue + OpenVR (SteamVR), Win32, Reverb G2.
Flat with DXVK 3.0.2 as d3d9.dll ok. No OpenXR for v0. Do not use IronWolf-tiw-rel as the base.
I am not a programmer — give concrete click/command steps.
Mono-Submit milestone reached. Next: Stability / Stereo / Camera (one thing per session).
Post-v0 plan: Quest 3 via a separate x64 OpenXR host; preserve the OpenVR/Reverb G2 fallback.
```

## Non-negotiable

1. **Win32 / x86** for everything that loads into `GTAIV.exe`.  
2. Singleplayer / offline during development.  
3. Do not distribute Rockstar assets or closed HL2VR/closed-VR binaries.  
4. **OpenVR (SteamVR) is the compositor** — first milestone without OpenXR.  
5. L4D2VR / IronWolf / sd805 are **references**; GTA needs its own glue.  
6. Do not sell a foreign game `d3d9.dll` 1:1 as "VR for GTA".  
7. One behavior change per test build; logs in a file next to the EXE.  
8. After headset tests, update `docs/CURRENT-STATE.md`.  
9. Instructions for the user in **English**, concrete and short.

## Definition of Done (first milestone)

Stock-DXVK **3.0.2** `d3d9.dll` flat ok + ASI Glue: SteamVR/OpenVR, **Mono** eye image via `ID3D9VkInterop*` → `IVRCompositor::Submit`.

That first milestone is complete. The post-v0 Quest 3/OpenXR architecture and gates are
defined in `docs/FULL_VR_PLAN.md`. GTA-loaded code remains x86; OpenXR lives in a
separate x64 host; OpenVR remains the fallback.

## Doc map

| File | When to read |
|------|--------------|
| `docs/NEW_PC_SETUP.md` | Fresh Windows PC — tools + deps |
| `docs/HOME_CURSOR.md` | User setup at home |
| `docs/CURRENT-STATE.md` | Status / Next |
| `docs/VR_STRATEGY.md` | Interop instead of IronWolf |
| `docs/FULL_VR_PLAN.md` | Quest 3 OpenXR-primary implementation plan |
| `docs/OPENXR_QUEST3_TEST.md` | Exact Quest 3 calibration-host test |
| `docs/ELLIOTT_OPENXR_SIMULATOR_PROOF.md` | Headless full-game OpenXR proof |
| `docs/META_XR_SIMULATOR_TEST.md` | No-headset Meta XR Simulator stereo/input test |
| `docs/DXVK_FLAT_TROUBLESHOOT.md` | Flat-DXVK lessons |
| `docs/ARCHITECTURE.md` | Modules |
| `docs/CONSTRAINTS.md` | Hard rules |
| `docs/REFERENCES.md` | Links |
| `docs/IRONWOLF_DXVK.md` | Historical mailbox (reference) |
| `docs/FAQ.md` | User questions |
| `docs/BUILD.md` | Build |
