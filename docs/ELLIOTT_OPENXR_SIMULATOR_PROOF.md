# Elliott OpenXR Simulator headless full-game proof

This is the repeatable, no-headset validation route for GTA IV's corrected
literal Mode **204** OpenXR provider target. Mode 58 remains documented below as
the historical OpenXR proof. The route does not control a simulator window or
register a system OpenXR runtime. The launcher supplies the pinned Elliott
manifest only to the x64 host through its child `XR_RUNTIME_JSON` environment.

## Literal Mode 204 OpenXR provider proof

The corrected OpenXR route does not change or reimplement upstream Mode 204.
It treats OpenXR as a provider adapter at the same finished-eye, pose, and input
seams used by the retained OpenVR/SteamVR provider:

```text
unchanged upstream Mode 204 render/camera/menu behavior
  -> completed upstream left/right textures
  -> thin x86 Mode 204 submit adapter
  -> isolated GPU texture bridge
  -> x64 OpenXR host presents the same eyes at the exact capture pose
```

The launcher transactionally stages the exact stable values from
`scripts/pack-mode204-zip.ps1` (`stereo=204`, `vres=2048`, and the existing
Mode 204 camera/input/HUD values). It does not stage `buildid`, overwrite other
user extras, or install the Mode 58 square `commandline.txt`. Every staged file
is restored to its original byte hash or original absence after the run.

For Elliott proof only, the launcher selects the simulator's Quest 3-like
asymmetric FOV/IPD profile through acknowledged JSON file IPC. That changes the
simulated OpenXR view configuration; it does not change GTA's Mode 204 renderer
or configuration. No simulator GUI is focused or controlled.

After building the ASI and x64 host, the sustained command-file proof is:

```powershell
.\scripts\run-openxr-gta-steam-safe.ps1 `
  -GameDir "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV" `
  -StereoMode 204 `
  -RuntimeManifest "D:\code\gta-iv\out-openxr\external\OpenXR-Simulator\bin\openxr_simulator.json" `
  -ElliottCliProof `
  -ElliottWalkSeconds 60 `
  -StopWhenProofComplete `
  -MaxSeconds 900 `
  -Authorized
```

This acceptance route is GPU-only. A live Mode 204 CPU-mailbox transaction
fails the proof. Required evidence includes:

- `OpenXRSubmitAdapter: Mode204 ... parent=0x4DE020 headHide=1`;
- `OpenXRBridge: READY Vulkan-imported D3D11 NT frame resources`;
- advancing `OpenXRBridge: frame ... presentation=world-stereo` transactions;
- `GameBridge: isolated GPU ingest queue READY` and
  `GameBridge: CONNECTED frame source`;
- advancing, pixel-distinct host frames with
  `presentation=world-parent-dual-stereo`;
- exact parent-dual capture pose, stationary menu/pause quad, sRGB path,
  acknowledged Start/A/stick/neutral input, and the 60-second pose/controller
  interval.

`result.txt` must report the Mode 204 config installed and restored, all four
Mode 204 adapter/GPU gates true, the Quest profile acknowledged and reset, and
`sustainedPoseControllerProof=True`.

## Current verification status: literal Mode 204 PASS

The authoritative complete run is:

```text
out-openxr/runs/20260728-214242-steam-safe/
```

It reports `outcome=PROOF_COMPLETE`, `stereoMode=204`,
`parentDualProofComplete=True`, `firstPersonProofComplete=True`, and
`walkQualityReady=True`. The automated route reached the apartment world,
completed a menu -> world -> pause UI -> world round trip, and then held the
simulated Touch left stick forward for 60 seconds.

Measured proof facts:

```text
producer transactions:     2280 -> 6840
host transactions:         2265 -> 6823
walk producer advance:     4080
walk host advance:         3883
heartbeat pauses:          0
first-person planar travel: 7.118 m
accepted walk attempt:     1
OpenXR swapchain format:   29 (sRGB)
```

The camera moved from `(892.425, -499.725, 20.079)` to
`(892.796, -492.617, 20.057)`. The exact Mode 204 L/R texture route, exact
capture pose, isolated GPU queue, stationary UI quad, first-person hard hooks,
native head hide, Start/A/stick/release/neutral controller path, D3D9 Reset
recovery, cleanup, and exact configuration restoration all passed. No
simulator window was controlled and SteamVR remained absent.

Tested binary hashes:

```text
x86 ASI SHA-256:
8E6E9CBC242869535C983518A15502EC92DC11785819B8F6ECB7224A10E1EE07

x64 host SHA-256:
C40C2A1D3CF5F8AE02F97108B44D8EA5E210FC001515AB21D7A9030662C2FF1F
```

This simulator result proves the code/transport path and sustained automated
gameplay. It cannot certify physical Quest optics, human fusion, perceived
scale, comfort, or Link latency.

## Historical verification status: Mode 58 PASS

The authoritative full-game run is:

```text
out-openxr/runs/20260728-150553-steam-safe/
```

Its `result.txt` reports `outcome=PROOF_COMPLETE`. This is the first complete
Mode 58 result and supersedes Mode 57 as the current simulator acceptance result.
The run proved:

- OpenXR backend active with SteamVR absent and `openvr_api.dll` isolated;
- hard first-person GTA camera hooks active, first-person gameplay lock active,
  and native five-component player-head hide operational;
- same-tick L/R rendering through upstream Mode 204's exact parent at
  `0x4DE020`, with one pinned OpenXR pose/FOV sample across both walks;
- 6,240 accepted parent-dual pairs and a final measured camera baseline of
  `9.1 cm`;
- pixel-distinct L/R mailbox content, fresh camera generations
  `30708/30709`, exact host capture pose `12485`, and producer transaction
  `8040`;
- stationary menu/loading/pause presentation, followed by a proved
  world -> pause UI -> world round trip;
- Touch-to-XInput Start, A, forward-stick, release, and neutral behavior;
- a 20-second commanded forward walk that moved the first-person camera about
  `7.1 m`, from approximately world Y `-499.7` to `-492.6`;
- sRGB decode and OpenXR swapchain format `29`;
- advancing producer/host world traffic, device-Reset recovery, final neutral,
  cleanup, and exact installed-file restoration.

The pre-pause gate observed 120 uninterrupted Mode 58 pairs, shared advancing
producer/host transactions, then required three additional continuous gameplay
seconds before sending Start. That settling gate is important: an earlier
`20260728-145535-steam-safe` attempt sent Start too soon and never entered pause.
It is a retained automation-timing negative test, not a Mode 58 stereo failure.

Tested binary hashes:

```text
x86 ASI SHA-256:
39F9D931D95998BEB6A7111DF33ABC9661B2466B9AEA4BB3CA44B9192C3C9F4A

x64 host SHA-256:
22272C3B7EA21396C39438CD351344C0C26253DAF18888060D5B04661F6ECEBC
```

The run was made after fetching `origin/master` at
`715d40f0c57a648041a5b4f585602b07c664a7d0` ("Ship Mode 204 fused stereo
milestone as the master build"). That upstream state is contained by the
integration work; Mode 58 adds the OpenXR layer without replacing the proven
Mode 204 GTA render parent.

This headless proof does not replace a real Quest 3 visual acceptance test. It
cannot judge human stereo fusion, comfort, perceived scale, Link latency, or
optical headset smoothness.

## Recorded first-person OpenXR proof

The matching recording control directory and video are:

```text
out-openxr/video-20260728-150552-vr/
out-openxr/video-20260728-150552-vr/gtaiv-openxr-mode58-vr-sbs-12s.mp4
```

This is the Elliott runtime's composed side-by-side OpenXR preview, not the flat
GTA window and not a reconstructed eye pair. The simulator remained
headless/file-IPC controlled. Its preview HWND was exposed without activation
only for Windows Graphics Capture.

Verified media facts:

```text
capture=PASS
duration=12.00 seconds
dimensions=3840x1920 side-by-side
frames=713
average frame rate=59.42 fps
sampled frame hashes=120/120 unique
freezeEvents=0
bytes=27359559
SHA-256=CE09CAF99F8CF51272E89C70A412C6A4B9CEF30FEE19A131AF08DD16F55F998D
```

The video begins only after the world, stereo, first-person, pause/resume, and
locomotion gates pass. Its 12 seconds are inside the acknowledged 20-second
forward-stick interval with the Mode 58 head-pose sweep active. Extracted frames
show the first-person view advancing through the apartment with visibly offset
left and right eyes. `frame-sample.framemd5` contains 120 distinct sample hashes;
`freezedetect.log` contains no freeze event.

## Mode 58 proof contract

Mode 58 minimally marries the current OpenXR transport to the known upstream
Mode 204 render seam:

```text
one native GTA source frame
  -> exact Mode 204 parent @ 0x4DE020
  -> pin one OpenXR HMD/eye/FOV sample
  -> left camera receipt + native parent walk + left canvas copy
  -> right camera receipt + native parent walk + right canvas copy
  -> one same-tick parent-dual pair
  -> pixel-distinct CPU mailbox transaction
  -> x64 OpenXR host presents both eyes with the exact capture pose
```

Publication is fail-closed. A Mode 58 world pair must carry all of these facts:

- same GTA source frame and same OpenXR pose for both eyes;
- verified Mode 204 parent-dual route, never the Mode 120 replay path;
- fresh, ordered per-eye camera generations on the render thread;
- correct L/R eye selection and an applied stereo baseline;
- first-person camera anchor on both walks;
- successful native player-head hide;
- both eye copies complete and downstream pixels distinct;
- exact pose/FOV lookup accepted by the x64 host.

Menu, loading, phone, and pause states deliberately use one fresh GTA image on a
stationary local-space quad. UI transactions cannot carry the Mode 58
parent-dual/first-person proof flags.

The OpenXR route never initializes OpenVR. A Mode 58 structural hook failure
falls back to Mode **57** temporal OpenXR stereo; Mode **55** remains the
center-eye OpenXR mono fallback. The imported OpenVR/Reverb G2 modes are retained
separately as fallback/history.

## Build the pinned simulator

From PowerShell in `D:\code\gta-iv`:

```powershell
.\scripts\setup-elliott-openxr-simulator.ps1
```

The setup script checks out
`48a70f440ac7d9bda385994937e3da8e15a4d9bb`, verifies the packaged patch and
file-IPC asset hashes, builds x64 Release, verifies the PE machine, and prints
the manifest path. It does not activate or launch the runtime.

## Build GTA artifacts

```powershell
.\scripts\build-asi.ps1
.\scripts\build-openxr-host.ps1
.\tests\openxr_runtime_info_test.ps1
.\tests\elliott_cli_proof_contract_test.ps1
```

## Run the locked Mode 58 proof

Use the manifest printed by the setup script:

```powershell
.\scripts\run-openxr-gta-steam-safe.ps1 `
  -GameDir "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV" `
  -StereoMode 58 `
  -RuntimeManifest "D:\code\gta-iv\out-openxr\external\OpenXR-Simulator\bin\openxr_simulator.json" `
  -ElliottCliProof `
  -StopWhenProofComplete `
  -MaxSeconds 600 `
  -Authorized
```

The launcher:

1. rejects non-empty GTA IV Steam launch options;
2. deploys the candidate ASI with `backend=openxr` and `stereo=58`;
3. backs up/removes `openvr_api.dll` and creates the OpenVR disable sentinel;
4. starts the x64 host with a child-only Elliott runtime override;
5. checks SteamVR absence at audited launch boundaries, without continuous
   process polling;
6. uses ordinary Steam app 12210 authentication with no title arguments and one
   bounded Rockstar handoff retry;
7. drives Start, A, stick, neutral, and pose through acknowledged JSON command
   files, never simulator-window control;
8. requires sustained fresh world transactions, Mode 58 parent-dual and
   first-person proofs, then three additional advancing gameplay seconds before
   testing pause;
9. proves stationary pause UI, resume to world, forward locomotion, and final
   neutral;
10. records `PROOF_COMPLETE`, stops the title and host, removes command files,
    and restores every saved game file.

## Record the composed in-game VR view

```powershell
.\scripts\run-and-record-elliott-vr.ps1 `
  -GameDir "D:\SteamLibrary\steamapps\common\Grand Theft Auto IV\GTAIV" `
  -RuntimeManifest "D:\code\gta-iv\out-openxr\external\OpenXR-Simulator\bin\openxr_simulator.json" `
  -StereoMode 58 `
  -WalkSeconds 20 `
  -DurationSeconds 12 `
  -FrameRate 60 `
  -Authorized
```

The wrapper waits on the atomic `elliott-walk-ready.marker` and captures only the
host-owned preview HWND. It does not read the live status log, continuously poll
SteamVR, focus a window, or control simulator input. Windows Graphics Capture
scales the runtime preview to 3840x1920 SBS and records H.264 through NVENC.

## Acceptance markers

Require all of these in `status.log`:

```text
Mode58 hard first-person camera hooks READY
Mode58 gameplay first-person lock ACTIVE
Mode58 native player head hide operational
Mode58 fused Mode204 parent L/R capture READY
Mode58 parent-dual exact capture pose active in OpenXR host
Mode58 uninterrupted pre-pause fused world continuity PASS
gameplay settle PASS with advancing producer/host transactions
distinct in-game pause UI observed
in-game world->pause UI->world round trip PASS
Mode58 pose sweep active for recorded walk
GTA consumed Elliott left-stick locomotion
GTA consumed neutral after Elliott stick hold
Mode58 recorded walk pose sweep and stick/neutral automation COMPLETE
PROOF COMPLETE
RESTORED: original ASI/backend/stereo/OpenVR state
```

Also require these `result.txt` facts:

```text
outcome=PROOF_COMPLETE
stereoMode=58
controllerProofComplete=True
stationaryUiQuad=True
pauseRoundTrip=True
stereoPixelDistinct=True
stereoCameraDistanceReady=True
stereoCameraDistanceCm=9.1
stereoAcceptedPairOrdinal=6240
mode58PrePauseAcceptedPairSpan=120
mode58PrePauseContinuityReady=True
parentDualProofComplete=True
firstPersonProofComplete=True
parentDualHostExactPoseReady=True
hostPresentationContinuityReady=True
hostSwapchainFreshUpdatesReady=True
deviceResetHealthy=True
elliottProofAutomationComplete=True
cleanupFailures=
restoreFailures=
```

For the video, require the recorder receipt, 713 decoded frames over 12 seconds,
120 unique hashes from 120 samples, zero `freezedetect` events, and the exact
video SHA-256 recorded above.

## Historical Mode 57 result

Mode 57's two-boundary temporal proof remains useful as the fail-safe OpenXR
stereo route. Its successful runs established the CPU mailbox, exact per-eye
capture-pose transport, UI quad, color, and controller infrastructure. It is not
the current primary because its eyes come from consecutive GTA frames. Mode 58
reuses that transport while replacing temporal eye capture with the proved
same-tick Mode 204 parent dual.
