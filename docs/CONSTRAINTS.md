# Hard Rules (Constraints)

Do not learn these the hard way again.

1. **GTA IV CE = Win32 (32-bit).** Everything in the process: **x86**.  
2. **Proven v0 compositor = OpenVR via SteamVR.** Keep it as fallback. The post-v0
   Quest 3 path may use OpenXR only through the gated x64 sidecar in
   `docs/FULL_VR_PLAN.md`; it does not make GTA or its ASI x64.
3. **Neither OpenVR nor OpenXR accept raw D3D9 textures.** Submit needs Vulkan (or D3D11) images from the DXVK path.  
4. **Stock-DXVK / Steam "Vulkan guide" = FPS only.** VR needs a fork with mailbox + our glue.  
5. **Foreign `d3d9.dll` (e.g. L4D2VR release) in GTA ≠ VR.** Wrong game, wrong hooks.  
6. **FusionFix** = CE base / ASI loader — not VR. Turn FusionFix Vulkan toggle off when our DLL is active.  
7. **Async / gplasync** = stutter, not "image in the headset".  
8. **First-person camera** later: CE offsets not 1:1 from old IV SDK versions; use AOB/natives.  
9. **Reverb G2 + OpenVR** ⇒ SteamVR must see the headset.  
10. **One behavior change per test build**; paste logs.  
11. Development **offline / Singleplayer**.  
12. Work PC: avoid risky network tools; submodules/push at home.
