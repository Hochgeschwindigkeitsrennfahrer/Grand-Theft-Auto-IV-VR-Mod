# Lessons carried from Phase A (`gtaiv-openxr`)

Do not re-learn these the hard way.

## Hard constraints

1. **GTA IV CE is Win32 (32-bit).** Everything injected must be x86.
2. **OpenXR on Win32:** SteamVR OpenXR often has **no usable 32-bit runtime**. Prefer **WMR OpenXR** on HP Reverb G2 (openRBRVR FAQ + crysis_vrmod notes).
3. **OpenXR does not accept raw D3D9 textures.** HL2/L4D2 solve this by submitting **Vulkan** (or D3D11) from inside/near DXVK — not by “Vulkan magically = VR.”
4. **Stock DXVK / Steam “Vulkan guide”** = FPS only. **VR** needs a **fork** with compositor hooks (L4D2VR, openRBRVR, HL2VR custom `d3d9.dll`).
5. **Dropping another game’s `d3d9.dll` into GTA will not magically enable VR.**
6. FusionFix = ASI loader baseline on CE; watch conflicts if both FusionFix Vulkan toggle and our `d3d9.dll` fight.
7. Async / gplasync = **stutter**, not image-to-HMD.
8. Camera FP / CE offsets still needed later (IV-SDK addresses are 1.0.7/1.0.8; CE needs AOB or natives).
9. Work PC: avoid risky network tooling; do submodule fetch / GitHub push at home if needed.
10. One behavioral change per test build; fail safe; log to file for non-programmers.

## Useful Phase A artifacts to reuse conceptually

| Phase A | Phase B use |
|---------|-------------|
| `docs/OPENXR_WIN32.md` | Same runtime policy |
| `docs/HL2VR_PATH.md` | This *is* that path |
| `docs/OSS_VR_SURVEY.md` | L4D2VR / openRBRVR / crysis rows |
| `config/dxvk.conf.example` | Starting conf for async |
| `sigscan` / camera layouts | Port later into `src/gtaiv/` |
| HANDOFF testing loop | Same: build → deploy → log → headset note |

## Performance

HL2VR: custom DXVK 1.10.3 + async.  
We: prefer modern gplasync ideas inside the VR fork when practical; don’t block first mono HMD frame on async perfection.
