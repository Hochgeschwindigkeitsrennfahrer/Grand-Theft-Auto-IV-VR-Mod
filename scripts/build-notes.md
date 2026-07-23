# Build notes — Phase B

## Reference (L4D2VR)

From https://github.com/sd805/l4d2vr :

```text
git clone --recurse-submodules https://github.com/sd805/l4d2vr.git
Open l4d2vr.sln → x86 Debug/Release → Build
```

Output: `d3d9.dll` copied toward the game dir.

## This repo

1. `.\scripts\init-submodules.ps1`
2. Inspect `dxvk\` — confirm it is the VR/async-oriented fork
3. Prefer bringing up an **x86** DXVK `d3d9.dll` that at least runs GTA IV flat (FusionFix)
4. Then graft OpenXR eye submit (study openRBRVR + L4D2VR VR texture path)
5. Log file next to `GTAIV.exe`

Exact VS/meson steps will be filled after first successful submodule sync at home.

## Conflicts

- FusionFix may ship its own Vulkan/DXVK toggle — disable or rename per Gillian guide before dropping our `d3d9.dll`
- Never mix x64 DXVK into GTA IV
