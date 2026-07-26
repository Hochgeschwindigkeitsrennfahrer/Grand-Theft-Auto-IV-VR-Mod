# Quest 3 OpenXR calibration test

This test runs only the separate x64 host. It does not start GTA, deploy an ASI,
change the active OpenXR runtime, or launch SteamVR.

> **Result, 2026-07-24 PT: presentation gate passed.** The Quest 3 displayed distinct
> left/right output while the host submitted 900 frames and exited cleanly. GTA was
> not visible because this calibration build contains no game-frame bridge.

## Prepare the headset

1. Open the **Meta Quest Link** desktop app.
2. Connect Quest 3 by Link cable, or enable Air Link.
3. Put on Quest 3 and launch **Quest Link** so the PC Link dashboard is visible.
4. Keep the headset awake.
5. Close SteamVR.

## Build and run

Open PowerShell in `D:\code\gta-iv`:

```powershell
.\scripts\build-openxr-host.ps1
.\scripts\run-openxr-calibration.ps1 -FrameLimit 900 -TimeoutSeconds 30
```

The second command runs for roughly ten seconds at 90 Hz, then exits. Press
`Ctrl+C` if you need to stop sooner.

## What should be visible

- a dark, head-tracked 3D scene with a checkered floor and three colored spheres;
- a cyan **L** marker in the left eye;
- an orange **R** marker in the right eye;
- a centered reticle and faint grid;
- one comfortable fused scene, with nearby objects showing more stereo depth than
  distant objects;
- the world remains anchored when you turn and lean.

The eye-specific colors and letters make an eye swap obvious.

## Passing log

Open `out-openxr\gtaiv_xr_host.log`. The passing run includes:

```text
XRHost: runtime=Oculus ...
XRHost: system=Meta Quest 3 ...
XRHost: adapterLuid=...
XRHost: session READY
XRHost: session RUNNING
XRHost: view[0] fov=...
XRHost: view[1] fov=...
XRHost: projection submit ok frame=1
```

Report four things after the run:

```text
L in left eye: yes/no
R in right eye: yes/no
Scene fuses and has depth: yes/no
Scene stays fixed while turning/leaning: yes/no
```

## If it stays IDLE

`IDLE` means the runtime and headset were found, but the PC VR session was not active.
Put on the headset, launch Quest Link inside Quest 3, remain in the Link dashboard,
and rerun the command.

## Other runtimes and headsets

The host uses standard OpenXR plus `XR_KHR_D3D11_enable`; it contains no Meta-only
render path. For a deliberate SteamVR OpenXR compatibility test, make SteamVR the
active OpenXR runtime and run:

```powershell
.\scripts\run-openxr-calibration.ps1 -FrameLimit 900 -TimeoutSeconds 30 -AllowSteamVR
```

That is a best-effort compatibility gate. Quest 3 with direct Meta OpenXR remains the
required primary test, and the existing in-game OpenVR path remains untouched.
