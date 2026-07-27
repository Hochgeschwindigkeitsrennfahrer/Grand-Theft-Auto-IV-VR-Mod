# Quest 3 pose, Touch, haptics, and menu gate

This direct GTA/OpenXR test remains **quarantined**. The replacement GPU transport,
camera pose path, Touch-to-XInput mapping, haptics, and menu quad now build offline,
but none of those changes has been deployed or headset-tested.

On 2026-07-26 the Meta-only host gate reached `FOCUSED` on Quest 3 with Touch
actions ready. The subsequent argument-free `GTAIV.exe` process exited into
`PlayGTAIV.exe`/Steam authentication before the candidate ASI produced a fresh
in-game log. The route was stopped, SteamVR remained absent, and the installed
files/settings were restored. This is not a GTA stereo/input pass.

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
