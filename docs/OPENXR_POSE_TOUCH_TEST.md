# Quest 3 pose and Touch-controller log test

This is the first direct OpenXR input gate. It starts GTA and the x64 host, but it
does **not** move the GTA camera or inject controller input yet. Its only job is to
prove that the x64 host and x86 ASI see the same fresh HMD and Quest Touch samples.

## Before starting

1. Start **Meta Quest Link** and enter the Link dashboard in the headset.
2. Close **SteamVR** completely.
3. Keep GTA IV offline/singleplayer.

## Run

Open PowerShell in `D:\code\gta-iv` and run:

```powershell
.\scripts\run-openxr-gta.ps1 -GameDir "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV" -Build
```

In the headset, start GTA as usual. Move both Touch controllers, squeeze each grip,
pull each trigger, move and click both sticks, press X/Y and A/B, and press the left
Menu button. The game is expected to remain neutral during this test.

## Passing log lines

Open these files after about 30 seconds:

```text
GTAIV\gtaiv_dxvk_vr.log
D:\code\gta-iv\out-openxr\gtaiv_xr_host.log
```

The host log needs:

```text
XRHost: Touch actions ready (...)
PoseBridge: READY producer=x64 ...
XRHost: Pose/Input publishing started ...
```

The GTA log needs lines similar to:

```text
OpenXRPose: x64 host mapping found ...
OpenXRPose: host connected ...
OpenXRPose: seq=... hmd=(...) L[t=... stick=... btn=...] R[t=... stick=... btn=...]
```

Report whether each item changed in the `OpenXRPose` line:

```text
HMD position: yes/no
Left grip + trigger: yes/no
Right grip + trigger: yes/no
Left stick: yes/no
Right stick: yes/no
X/Y: yes/no
A/B: yes/no
Left Menu: yes/no
```

## What comes after a pass

The next build will make one controlled behavior change: use the validated OpenXR
HMD pose as GTA's camera source. Controller-to-GTA input injection and same-frame
left/right rendering remain separate tests, so a failed mapping never also changes
the camera or gameplay controls.
