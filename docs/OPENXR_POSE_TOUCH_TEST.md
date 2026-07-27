# Quest 3 pose, Touch, haptics, and menu gate

This direct GTA/OpenXR test remains **quarantined**. The replacement GPU transport,
camera pose path, Touch-to-XInput mapping, haptics, and menu quad now build offline,
but none of those changes has been deployed or headset-tested.

On 2026-07-26 the Meta-only host gate reached `FOCUSED` on Quest 3 with Touch
actions ready. The subsequent argument-free `GTAIV.exe` process exited into
`PlayGTAIV.exe`/Steam authentication before the candidate ASI produced a fresh
in-game log. The route was stopped, SteamVR remained absent, and the installed
files/settings were restored. This is not a GTA stereo/input pass.

A second authorized run used normal Steam authentication. GTA reached a fresh
OpenXR/Mode-54 ASI session and logged Quest poses, Touch-to-XInput, and all six WVP
draw hooks. SteamVR then started because shared OpenVR display helpers were still
reachable from the Mode-54 path. It was stopped and the install was restored before
any verified world transaction.

Commit `400294e` removes those OpenVR calls, uses OpenXR `PoseBridge` FOV for canvas
sizing, and fails closed if `openvr_api.dll` loads. The supervised launcher also
creates `gtaiv_dxvk_vr.disable` and temporarily removes/backups `openvr_api.dll`.
These post-fix guards pass offline checks but have not been live-tested.

## Safe checks now

These commands compile and self-test only:

```powershell
cd D:\code\gta-iv
.\scripts\build-asi.ps1
.\scripts\build-openxr-host.ps1
.\scripts\run-openxr-gta.ps1 -Build
```

The last command is preflight-only. It cannot start GTA, the OpenXR host, Steam, or
SteamVR and does not deploy a game file.

## Live gate

Do not repeat a live test yet. It requires a separate explicit authorization and one
behavior change at a time: GPU transport first, then same-frame world stereo, then
camera, controls/haptics, and finally pause/map/loading/phone quad transitions.
