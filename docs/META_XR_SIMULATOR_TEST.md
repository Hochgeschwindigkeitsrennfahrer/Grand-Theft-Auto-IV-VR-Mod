# Meta XR Simulator test — no headset and no SteamVR

Meta XR Simulator is a standalone OpenXR runtime from Meta. Version **205.0**
was released on 2026-07-23 and adds Meta XR Operator support, including
screenshots, simulated head pose, controller input, and Codex compatibility.

Official links:

- [Meta XR Simulator 205.0 download and release notes](https://developers.meta.com/horizon/downloads/package/meta-xr-simulator-windows/)
- [Native/standalone simulator setup](https://developers.meta.com/horizon/documentation/native/xrsim-getting-started/)
- [Meta XR Operator Standalone 205.1](https://developers.meta.com/horizon/downloads/package/meta-xr-operator-standalone/)
- [Connecting an AI agent to Operator Standalone](https://developers.meta.com/horizon/documentation/unity/meta-xr-operator/connecting-ai-agents/)
- [Using Operator with Meta XR Simulator](https://developers.meta.com/horizon/documentation/unity/meta-xr-operator/xr-simulator/)

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

## Optional Meta XR Operator control

Meta XR Operator Standalone is a separate download from Meta XR Simulator. It is
an experimental OpenXR API layer for test automation, not a renderer dependency.
For this native project it wraps only the separate x64
`gtaiv_xr_host.exe`; it never loads into 32-bit GTA and never changes Mode 204.

When enabled, Operator can inspect the OpenXR session and frame state, capture
the composed simulator image, set simulated head and controller poses, and send
Touch buttons, thumbsticks, triggers, and grips. Those normal OpenXR actions flow
through the host's existing `PoseBridge` into GTA.

The supervised launcher accepts Operator only through the explicit
`-MetaXrOperatorDirectory` option. It:

- permits only Meta XR Simulator or Meta Quest Link;
- finds the manifest by the exact declared layer name
  `XR_APILAYER_METAX_operator`;
- requires the manifest's layer DLL and the MCP proxy in the same Windows bundle
  directory and verifies both executables are x64;
- rejects a pre-existing listener on port `8720`;
- sets `XR_API_LAYER_PATH` and `XR_ENABLE_API_LAYERS` only while starting the
  x64 host, then restores the parent environment immediately;
- never launches the MCP proxy and never enables Operator by default.

After extracting Meta's official bundle, a supervised native-host run uses:

```powershell
.\scripts\run-openxr-gta-steam-safe.ps1 `
  -GameDir "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV" `
  -StereoMode 204 `
  -MetaXrOperatorDirectory "<extracted bundle>\windows" `
  -MaxSeconds 900 `
  -Authorized
```

The MCP proxy remains agent-owned so it can reconnect independently while the
host starts and stops. Do not package Meta's binaries with this project.

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
