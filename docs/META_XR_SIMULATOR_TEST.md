# Meta XR Simulator test — no headset and no SteamVR

Meta XR Simulator is a standalone OpenXR runtime from Meta. Version **205.0**
was released on 2026-07-23 and adds Meta XR Operator support, including
screenshots, simulated head pose, controller input, and Codex compatibility.

Official links:

- [Meta XR Simulator 205.0 download and release notes](https://developers.meta.com/horizon/downloads/package/meta-xr-simulator-windows/)
- [Native/standalone simulator setup](https://developers.meta.com/horizon/documentation/native/xrsim-getting-started/)

## What this can prove

- the x64 host creates a native OpenXR D3D11 session without SteamVR;
- left and right projections are submitted independently;
- head-pose changes update the rendered view;
- Touch action bindings reach the host;
- the GTA CPU mailbox advances and carries distinct L/R images in Mode 57;
- menu/game route switching, stationary menu placement, aspect, and sRGB color;
- repeatable screenshots and scripted pose/controller checks without wearing the
  headset.

It cannot prove Quest optics, human stereo comfort/fusion, Link latency, or real
Quest performance. One final Quest 3 test remains required after simulator gates
pass.

## Install and activate

1. Download and install version 205.0 from the official page above.
2. Open **Meta XR Simulator**.
3. Turn on its **Active OpenXR runtime** toggle.

Meta documents that activation changes the system OpenXR `ActiveRuntime` to
`meta_openxr_simulator.json`. Turning the toggle off restores the previous runtime.
Only one OpenXR runtime can be active, so activation temporarily replaces Meta
Quest Link as the active runtime.

No repository script changes the active runtime.

## Verify the route without launching anything

From PowerShell in `D:\code\gta-iv`:

```powershell
.\tests\openxr_runtime_info_test.ps1
.\scripts\run-openxr-gta.ps1
```

Expected:

```text
OpenXrRuntimeInfoTest: PASS cases=8
OPENXR GTA PREFLIGHT PASS
Route:   Meta XR Simulator
Launch:  disabled; this script cannot start Steam, SteamVR, GTA, or the OpenXR host
```

## Run the standalone stereo calibration

```powershell
.\scripts\run-openxr-calibration.ps1 -FrameLimit 900 -TimeoutSeconds 30
```

The simulator should show the procedural 3D calibration scene, a cyan **L** in
the left eye, and an orange **R** in the right eye. Change the simulated head pose
and confirm the world stays anchored.

This command starts only the x64 OpenXR host. It does not deploy or start GTA,
Steam, or SteamVR.

## Run GTA through the simulator

This is a live, supervised operation and remains locked behind `-Authorized`:

```powershell
.\scripts\run-openxr-gta-steam-safe.ps1 `
  -GameDir "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV" `
  -StereoMode 57 `
  -MaxSeconds 900 `
  -Authorized
```

Regular Steam may open solely for GTA IV authentication. The script removes the
game-folder OpenVR DLL during the run, writes the OpenVR disable sentinel, watches
for SteamVR only at audited launch boundaries, performs no continuous process
polling, and restores the prior
game files afterward.

For the fully automated no-window proof, use the pinned Elliott simulator instead:
[`ELLIOTT_OPENXR_SIMULATOR_PROOF.md`](ELLIOTT_OPENXR_SIMULATOR_PROOF.md).

Mode 55 remains the center-eye mono diagnostic fallback:

```powershell
.\scripts\run-openxr-gta-steam-safe.ps1 `
  -GameDir "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV" `
  -StereoMode 55 `
  -MaxSeconds 900 `
  -Authorized
```

## Restore Quest Link

When simulator testing is finished, turn off the simulator's active-runtime
toggle. Then rerun:

```powershell
.\scripts\run-openxr-gta.ps1
```

The route should report **Meta Quest Link**. The script will again require the
Meta `OVRService` only for that physical-headset route.
