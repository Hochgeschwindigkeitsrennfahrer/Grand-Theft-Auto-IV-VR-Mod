# Quest 3 pose and Touch-controller log test

This direct GTA/OpenXR test is **quarantined** after a failed GPU-handle transport
test. The launcher is now preflight-only and starts no game, host, Steam, or SteamVR
process.

## Before starting

1. Start **Meta Quest Link** and enter the Link dashboard in the headset.
2. Close **SteamVR** completely.
3. Keep GTA IV offline/singleplayer.

## Run

Open PowerShell in `D:\code\gta-iv` and run:

```powershell
.\scripts\run-openxr-gta.ps1 -Build
```

This safely confirms the Meta route but starts nothing. It uses the script's detected
GTA folder and does **not** invoke Steam or SteamVR. Do not attempt a direct GTA/OpenXR
launch until a replacement transport is reviewed and explicitly re-enabled.
